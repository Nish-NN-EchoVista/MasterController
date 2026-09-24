/*
 * mc_app.c - MasterController application core.
 *
 * Data flow (all in the main loop; interrupts only move bytes):
 *
 *   PC RX -> line assembler -> router -> DC TX queues (whole lines)
 *   DC RX -> line assembler -> retag  -> PC TX queue  (whole lines)
 *
 * Guarantees:
 *  - Lines are only ever queued, sent and dropped whole. Output from six
 *    DataControllers can never interleave inside a line.
 *  - A broadcast is admitted to all six DataController queues or to none.
 *  - stop / cancel / @0 stop_all overtake queued traffic; unsent lines for
 *    the same boards are discarded first so nothing queued can restart them.
 *  - Every drop is counted, and reported on the PC link as an [MC] line.
 */
#include "mc_app.h"
#include "mc_config.h"
#include "mc_platform.h"
#include "mc_txq.h"
#include "mc_line.h"
#include "mc_router.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t rx_bytes, rx_lines;
    uint32_t tx_lines, tx_bytes;
    uint32_t overlong;          /* input lines longer than MC_LINE_MAX       */
    uint32_t binary;            /* binary frames discarded                   */
    uint32_t rejected;          /* lines refused by this port's TX queue     */
    uint32_t purged;            /* queued lines discarded by stop/cancel     */
    uint32_t stalls;            /* transmissions aborted                     */
    uint32_t up_dropped;        /* DC only: lines lost on the way to the PC  */
    uint32_t garbled;           /* PC only: lines with non-printable bytes   */
    uint32_t tainted;           /* PC only: lines discarded after UART errors */
} counters_t;

typedef struct {
    mc_line_t     line;
    mc_txq_t      txq;
    counters_t    c;
    mc_hw_stats_t hw_base;      /* hardware counters at the last reset       */
    uint32_t      tx_start_ms;
    uint32_t      tx_budget_ms;
    uint32_t      last_rx_ms;
    uint32_t      up_reported;  /* up_dropped value already reported         */
    uint32_t      up_report_ms;
    uint32_t      err_seen;     /* raw error total at the last RX sample     */
    uint32_t      overrun_seen; /* raw rx_overruns at the last RX sample     */
    uint32_t      err_reported; /* raw error total already warned about      */
    uint32_t      err_report_ms;
    uint8_t       taint_next;   /* PC: discard the next completed line       */
    uint8_t       seen, online, missing_reported;
} port_t;

/* Rate-limited repeated event: the first occurrence is reported at once,
   later ones are summed and reported at most every MC_WARN_INTERVAL_MS.    */
typedef struct {
    uint32_t last_ms;
    uint32_t pending;
    uint8_t  armed;
} ratelimit_t;

static port_t ports[MC_NUM_PORTS];

static mc_slot_t pc_slots[MC_PC_TXQ_SLOTS];
static uint16_t  pc_order[MC_PC_TXQ_SLOTS], pc_free[MC_PC_TXQ_SLOTS];
static mc_slot_t dc_slots[MC_NUM_DC][MC_DC_TXQ_SLOTS];
static uint16_t  dc_order[MC_NUM_DC][MC_DC_TXQ_SLOTS], dc_free[MC_NUM_DC][MC_DC_TXQ_SLOTS];
static mc_slot_t dbg_slots[MC_DBG_TXQ_SLOTS];
static uint16_t  dbg_order[MC_DBG_TXQ_SLOTS], dbg_free[MC_DBG_TXQ_SLOTS];

static uint32_t boot_ms, last_activity_ms, last_drop_ms, loop_max_ms;
static uint32_t mc_lines_lost;         /* [MC] replies that found no room   */
static uint8_t  had_drop;
static volatile uint8_t status_requested;
static ratelimit_t rl_busy, rl_pc_binary, rl_pc_overlong;
static uint32_t busy_total;

static port_t *pc(void)            { return &ports[MC_PORT_PC]; }
static port_t *dc(unsigned k)      { return &ports[MC_PORT_DC(k)]; }
static uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }

/* ------------------------------------------------------------ emission */

static void push_mc_line(const char *text, size_t n)
{
    char buf[MC_SLOT_BYTES];
    int head = snprintf(buf, sizeof buf, "[MC] ");
    size_t room = sizeof buf - (size_t)head - 2u;
    if (n > room) n = room;
    memcpy(buf + head, text, n);
    n += (size_t)head;
    buf[n++] = '\r';
    buf[n++] = '\n';

    /* [MC] lines may use the reserve that DataController traffic cannot. */
    if (!mc_txq_push(&pc()->txq, (const uint8_t *)buf, (uint16_t)n, 0, 0))
        ++mc_lines_lost;
    if (mc_plat_port_present(MC_PORT_DBG))
        (void)mc_txq_push(&ports[MC_PORT_DBG].txq, (const uint8_t *)buf, (uint16_t)n, 0, 0);
}

static void emit(const char *fmt, ...)
{
    char text[MC_SLOT_BYTES];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof text) n = (int)sizeof text - 1;
    push_mc_line(text, (size_t)n);
}

/* Returns 1 if the event should be reported now; otherwise it is counted. */
static int ratelimit_hit(ratelimit_t *r, uint32_t now)
{
    if (!r->armed || elapsed(now, r->last_ms) >= MC_WARN_INTERVAL_MS) {
        r->armed = 1;
        r->last_ms = now;
        return 1;
    }
    ++r->pending;
    return 0;
}

/* Report suppressed repeats once their interval has passed. */
static void ratelimit_flush(ratelimit_t *r, uint32_t now, const char *what)
{
    if (r->pending && elapsed(now, r->last_ms) >= MC_WARN_INTERVAL_MS) {
        emit("%s (%lu more)", what, (unsigned long)r->pending);
        r->pending = 0;
        r->last_ms = now;
    }
}

static void note_drop(uint32_t now) { had_drop = 1; last_drop_ms = now; }

static int clip(size_t len) { return (int)(len > 60u ? 60u : len); }

/* ---------------------------------------------------------- PC -> DCs */

/* Admission threshold for ordinary traffic: the reserve is left for urgent
   lines (DC queues) and [MC] replies (PC queue).                          */
static int has_room(const port_t *p) { return mc_txq_free(&p->txq) > MC_TXQ_RESERVE; }

static void send_urgent(const mc_route_t *r, const uint8_t *out, uint16_t n,
                        unsigned first, unsigned last, uint32_t now)
{
    for (unsigned k = first; k <= last; ++k) {
        port_t *p = dc(k);
        uint16_t purged = mc_txq_purge(&p->txq, r->tag);
        if (purged) {
            p->c.purged += purged;
            emit("W: '%.*s' discarded %u queued line(s) for DC%u",
                 clip(r->payload_len), r->payload, (unsigned)purged, k);
        }
        if (!mc_txq_push(&p->txq, out, n, r->tag, 1)) {
            ++p->c.rejected;
            note_drop(now);
            emit("E: DC%u queue full, '%.*s' NOT sent to DC%u",
                 k, clip(r->payload_len), r->payload, k);
        }
    }
}

static void send_normal(const mc_route_t *r, const uint8_t *out, uint16_t n,
                        unsigned first, unsigned last, uint32_t now,
                        const char *line, uint16_t len)
{
    /* All-or-nothing: check every target before committing to any. */
    for (unsigned k = first; k <= last; ++k) {
        if (!has_room(dc(k))) {
            ++dc(k)->c.rejected;
            ++busy_total;
            note_drop(now);
            if (ratelimit_hit(&rl_busy, now))
                emit("E: busy (DC%u queue full), not sent: %.*s", k, clip(len), line);
            return;
        }
    }
    for (unsigned k = first; k <= last; ++k)
        (void)mc_txq_push(&dc(k)->txq, out, n, r->tag, 0);
}

static void handle_local(const char *line, uint16_t len);

/* Commands are printable ASCII. Anything else is line noise (a glitch while
   the adapter was plugged in, a baud mismatch) and must not be broadcast. */
static int printable(const char *s, uint16_t len)
{
    for (uint16_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20u && c != '\t') || c > 0x7Eu)
            return 0;
    }
    return 1;
}

static void handle_pc_line(const char *line, uint16_t len, uint32_t now)
{
    mc_route_t r;
    if (!printable(line, len)) {
        ++pc()->c.garbled;
        note_drop(now);
        emit("E: line contains non-printable characters, not sent");
        return;
    }
    mc_route_pc_line(line, len, &r);

    if (r.kind == MC_ROUTE_LOCAL) { handle_local(line, len); return; }
    if (r.kind == MC_ROUTE_INVALID) {
        emit("E: %s, not sent: %.*s", r.error, clip(len), line);
        return;
    }

    uint8_t out[MC_SLOT_BYTES];
    size_t plen = strlen(r.prefix);
    size_t n = plen + r.payload_len + 2u;
    if (n > sizeof out) return;          /* impossible: lines are bounded  */
    memcpy(out, r.prefix, plen);
    memcpy(out + plen, r.payload, r.payload_len);
    out[n - 2u] = '\r';
    out[n - 1u] = '\n';

    unsigned first = r.kind == MC_ROUTE_BROADCAST ? 1u : r.dc;
    unsigned last  = r.kind == MC_ROUTE_BROADCAST ? MC_NUM_DC : r.dc;
    if (r.urgent)
        send_urgent(&r, out, (uint16_t)n, first, last, now);
    else
        send_normal(&r, out, (uint16_t)n, first, last, now, line, len);
    last_activity_ms = now;
}

/* ---------------------------------------------------------- DCs -> PC */

static void handle_dc_line(unsigned k, const char *line, uint16_t len, uint32_t now)
{
    char out[MC_SLOT_BYTES];
    size_t n = mc_retag_dc_line(k, line, len, out, sizeof out);
    port_t *p = dc(k);
    if (n && has_room(pc()) &&
        mc_txq_push(&pc()->txq, (const uint8_t *)out, (uint16_t)n, 0, 0)) {
        last_activity_ms = now;
        return;
    }
    ++p->c.up_dropped;
    note_drop(now);
}

/* ------------------------------------------------------------- status */

static void hw_now(unsigned port, mc_hw_stats_t *d)
{
    mc_hw_stats_t s;
    const mc_hw_stats_t *b = &ports[port].hw_base;
    mc_plat_hw_stats(port, &s);
    d->framing     = s.framing - b->framing;
    d->noise       = s.noise - b->noise;
    d->parity      = s.parity - b->parity;
    d->rx_restarts = s.rx_restarts - b->rx_restarts;
    d->tx_aborts   = s.tx_aborts - b->tx_aborts;
    d->rx_overruns = s.rx_overruns - b->rx_overruns;
}

static void report_status(uint32_t now)
{
    mc_hw_stats_t h;
    port_t *p = pc();
    hw_now(MC_PORT_PC, &h);
    emit("status up=%lus reset=%s loop_max=%lums mc_lost=%lu busy=%lu",
         (unsigned long)(elapsed(now, boot_ms) / 1000u), mc_plat_reset_cause(),
         (unsigned long)loop_max_ms, (unsigned long)mc_lines_lost,
         (unsigned long)busy_total);
    emit("PC  %s %lu baud rx=%lu lines=%lu tx=%lu q=%u/%u overlong=%lu binary=%lu "
         "garbled=%lu uart_err_lines=%lu fe=%lu ne=%lu overruns=%lu stalls=%lu",
         mc_plat_port_name(MC_PORT_PC), (unsigned long)mc_plat_port_baud(MC_PORT_PC),
         (unsigned long)p->c.rx_bytes, (unsigned long)p->c.rx_lines,
         (unsigned long)p->c.tx_lines, (unsigned)mc_txq_count(&p->txq),
         (unsigned)MC_PC_TXQ_SLOTS, (unsigned long)p->c.overlong,
         (unsigned long)p->c.binary, (unsigned long)p->c.garbled,
         (unsigned long)p->c.tainted, (unsigned long)h.framing,
         (unsigned long)h.noise, (unsigned long)h.rx_overruns,
         (unsigned long)p->c.stalls);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k) {
        p = dc(k);
        hw_now(MC_PORT_DC(k), &h);
        char age[16];
        if (p->seen)
            snprintf(age, sizeof age, "%lums", (unsigned long)elapsed(now, p->last_rx_ms));
        else
            snprintf(age, sizeof age, "never");
        emit("DC%u %s %s @%u,@%u last_rx=%s rx=%lu lines=%lu up_drop=%lu tx=%lu q=%u/%u "
             "rejected=%lu purged=%lu overlong=%lu binary=%lu fe=%lu ne=%lu "
             "overruns=%lu dma_restarts=%lu stalls=%lu",
             k, mc_plat_port_name(MC_PORT_DC(k)), p->online ? "online" : "OFFLINE",
             2u * k - 1u, 2u * k, age,
             (unsigned long)p->c.rx_bytes, (unsigned long)p->c.rx_lines,
             (unsigned long)p->c.up_dropped, (unsigned long)p->c.tx_lines,
             (unsigned)mc_txq_count(&p->txq), (unsigned)MC_DC_TXQ_SLOTS,
             (unsigned long)p->c.rejected, (unsigned long)p->c.purged,
             (unsigned long)p->c.overlong, (unsigned long)p->c.binary,
             (unsigned long)h.framing, (unsigned long)h.noise,
             (unsigned long)h.rx_overruns, (unsigned long)h.rx_restarts,
             (unsigned long)p->c.stalls);
    }
}

static void reset_stats(uint32_t now)
{
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i) {
        memset(&ports[i].c, 0, sizeof ports[i].c);
        ports[i].up_reported = 0;
        if (mc_plat_port_present(i))
            mc_plat_hw_stats(i, &ports[i].hw_base);
    }
    mc_lines_lost = 0;
    busy_total = 0;
    loop_max_ms = 0;
    had_drop = 0;
    (void)now;
}

static void handle_local(const char *line, uint16_t len)
{
    uint32_t now = mc_plat_now_ms();
    /* Ignore trailing blanks so "mc_status " still works. */
    while (len && (line[len - 1u] == ' ' || line[len - 1u] == '\t')) --len;

#define IS(cmd) (len == sizeof(cmd) - 1u && memcmp(line, cmd, len) == 0)
    if (IS("mc_status")) {
        report_status(now);
    } else if (IS("mc_version")) {
        emit("MasterController %s (built %s %s), %u DataControllers x %u EVS2",
             MC_FW_VERSION, __DATE__, __TIME__, (unsigned)MC_NUM_DC,
             (unsigned)MC_BOARDS_PER_DC);
    } else if (IS("mc_ping")) {
        emit("pong");
    } else if (IS("mc_reset_stats")) {
        reset_stats(now);
        emit("statistics reset");
    } else if (IS("mc_help")) {
        emit("@1..@12 <cmd>: one EVS2 | #1..#6 <cmd>: one DataController | "
             "<cmd>: all six DataControllers");
        emit("stop, cancel, @0 stop_all: sent ahead of queued lines, which are discarded");
        emit("mc_status mc_version mc_ping mc_reset_stats mc_help");
    } else {
        emit("E: unknown MasterController command: %.*s", clip(len), line);
    }
#undef IS
}

/* ------------------------------------------------------------ service */

static void service_tx(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    if (mc_plat_tx_busy(port)) {
        if (mc_txq_head_locked(&p->txq) &&
            elapsed(now, p->tx_start_ms) > p->tx_budget_ms) {
            mc_plat_tx_abort(port);
            ++p->c.stalls;
            mc_txq_pop(&p->txq);
            note_drop(now);
            /* Part of the aborted line may have gone out. Terminate it so it
               cannot merge into the next line (which might be a stop).    */
            (void)mc_txq_push(&p->txq, (const uint8_t *)"\r\n", 2u, 0, 1);
        }
        return;
    }
    if (mc_txq_head_locked(&p->txq)) {           /* previous line finished */
        const mc_slot_t *done = mc_txq_head(&p->txq);
        ++p->c.tx_lines;
        p->c.tx_bytes += done->len;
        mc_txq_pop(&p->txq);
    }
    const mc_slot_t *next = mc_txq_head(&p->txq);
    if (next && mc_plat_tx_start(port, next->data, next->len)) {
        mc_txq_lock_head(&p->txq);
        p->tx_start_ms = now;
        /* Wire time of 10 bits per byte, plus a generous margin. */
        uint32_t baud = mc_plat_port_baud(port);
        p->tx_budget_ms = (baud ? (uint32_t)next->len * 10000u / baud : 0u) +
                          MC_TX_STALL_MARGIN_MS;
    }
}

static uint32_t err_total(const mc_hw_stats_t *s)
{
    return s->framing + s->noise + s->parity + s->rx_overruns;
}

/*
 * Line errors on the PC link: the UART drops a byte received with a framing
 * or noise error, which could be a terminator (two commands merge) or a
 * letter ("stop" becomes something else). Error flags are sampled after
 * each read, so they cover every byte read so far. When they have moved,
 * every line completed from that chunk, and the next line after it, is
 * discarded rather than forwarded. This may discard a clean line, but a
 * damaged one is never forwarded.
 */
static void service_rx(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    uint8_t buf[256];
    size_t total = 0, n;
    /* Bounded per pass so one busy port cannot starve the others. */
    for (;;) {
        n = total < 4096u ? mc_plat_rx_read(port, buf, sizeof buf) : 0u;

        mc_hw_stats_t hw;
        mc_plat_hw_stats(port, &hw);            /* after the read: see above */
        int tainted_chunk = 0;
        if (err_total(&hw) != p->err_seen) {
            p->err_seen = err_total(&hw);
            if (port == MC_PORT_PC) {
                tainted_chunk = 1;
                p->taint_next = 1;
            }
        }
        if (hw.rx_overruns != p->overrun_seen) {
            /* Unread input may have been overwritten: never join what is
               left of a line to whatever follows it.                     */
            p->overrun_seen = hw.rx_overruns;
            mc_line_init(&p->line);
            note_drop(now);
            emit("W: %s receive buffer may have overflowed (main loop stalled); "
                 "partial line discarded", port == MC_PORT_PC ? "PC" : mc_plat_port_name(port));
        }
        if (n == 0u)
            break;

        total += n;
        p->c.rx_bytes += (uint32_t)n;
        p->last_rx_ms = now;
        p->seen = 1;
        for (size_t i = 0; i < n; ++i) {
            mc_line_event_t e = mc_line_feed(&p->line, buf[i]);
            if (e == MC_LINE_READY) {
                ++p->c.rx_lines;
                if (port == MC_PORT_PC && (tainted_chunk || p->taint_next)) {
                    p->taint_next = 0;
                    ++p->c.tainted;
                    note_drop(now);
                    emit("E: line received with UART errors, not sent: %.*s",
                         clip(p->line.len), p->line.buf);
                } else if (port == MC_PORT_PC) {
                    handle_pc_line(p->line.buf, p->line.len, now);
                } else {
                    handle_dc_line(port, p->line.buf, p->line.len, now);
                }
            } else if (e == MC_LINE_OVERLONG) {
                ++p->c.overlong;
                note_drop(now);
                if (port == MC_PORT_PC && ratelimit_hit(&rl_pc_overlong, now))
                    emit("E: line longer than %u characters discarded", (unsigned)MC_LINE_MAX);
            } else if (e == MC_LINE_BINARY) {
                ++p->c.binary;
                if (port == MC_PORT_PC && ratelimit_hit(&rl_pc_binary, now))
                    emit("E: binary protocol frames are not supported by the "
                         "MasterController, frame discarded");
            }
        }
        if (tainted_chunk)
            p->taint_next = 1;      /* the damage may lie in the next line */
    }
}

/* At most one warning per port per interval while line errors keep rising. */
static void report_line_errors(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    if (p->err_seen == p->err_reported ||
        elapsed(now, p->err_report_ms) < MC_WARN_INTERVAL_MS)
        return;
    uint32_t delta = p->err_seen - p->err_reported;
    p->err_reported = p->err_seen;
    p->err_report_ms = now;
    if (port == MC_PORT_PC)
        emit("W: PC link: %lu UART error(s); affected lines discarded "
             "(check adapter, baud %lu, ground)", (unsigned long)delta,
             (unsigned long)mc_plat_port_baud(port));
    else
        emit("W: DC%u %s: %lu UART error(s) (check wiring, ground, baud %lu)",
             port, mc_plat_port_name(port), (unsigned long)delta,
             (unsigned long)mc_plat_port_baud(port));
}

static void service_health(uint32_t now)
{
    int after_grace = elapsed(now, boot_ms) >= MC_STARTUP_GRACE_MS;
    report_line_errors(MC_PORT_PC, now);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k) {
        port_t *p = dc(k);
        int alive = p->seen && elapsed(now, p->last_rx_ms) < MC_DC_SILENT_MS;
        if (alive && !p->online) {
            p->online = 1;
            emit("I: DC%u online (%s)", k, mc_plat_port_name(MC_PORT_DC(k)));
        } else if (!alive && p->online) {
            p->online = 0;
            emit("W: DC%u offline: silent for %lu ms", k,
                 (unsigned long)elapsed(now, p->last_rx_ms));
        } else if (!p->seen && after_grace && !p->missing_reported) {
            p->missing_reported = 1;
            emit("W: DC%u not detected on %s", k, mc_plat_port_name(MC_PORT_DC(k)));
        }
        report_line_errors(MC_PORT_DC(k), now);
        if (p->c.up_dropped != p->up_reported &&
            elapsed(now, p->up_report_ms) >= MC_WARN_INTERVAL_MS) {
            emit("W: PC link congested, %lu line(s) from DC%u dropped",
                 (unsigned long)(p->c.up_dropped - p->up_reported), k);
            p->up_reported = p->c.up_dropped;
            p->up_report_ms = now;
        }
    }
    ratelimit_flush(&rl_busy, now, "E: busy, further lines not sent");
    ratelimit_flush(&rl_pc_binary, now, "E: binary frames discarded");
    ratelimit_flush(&rl_pc_overlong, now, "E: overlong lines discarded");
}

/* ---------------------------------------------------------------- API */

void mc_app_init(void)
{
    memset(ports, 0, sizeof ports);
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i)
        mc_line_init(&ports[i].line);
    mc_txq_init(&pc()->txq, pc_slots, pc_order, pc_free, MC_PC_TXQ_SLOTS);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        mc_txq_init(&dc(k)->txq, dc_slots[k - 1u], dc_order[k - 1u],
                    dc_free[k - 1u], MC_DC_TXQ_SLOTS);
    mc_txq_init(&ports[MC_PORT_DBG].txq, dbg_slots, dbg_order, dbg_free,
                MC_DBG_TXQ_SLOTS);

    memset(&rl_busy, 0, sizeof rl_busy);
    memset(&rl_pc_binary, 0, sizeof rl_pc_binary);
    memset(&rl_pc_overlong, 0, sizeof rl_pc_overlong);
    boot_ms = mc_plat_now_ms();
    last_activity_ms = boot_ms;
    status_requested = 0;
    reset_stats(boot_ms);
    /* Errors counted before this boot of the app are not "new". */
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i) {
        if (!mc_plat_port_present(i)) continue;
        mc_hw_stats_t hw;
        mc_plat_hw_stats(i, &hw);
        ports[i].err_seen = ports[i].err_reported = err_total(&hw);
        ports[i].overrun_seen = hw.rx_overruns;
    }

    emit("MasterController %s ready (reset: %s). %u DataControllers, EVS2 @1..@%u. "
         "Type mc_help.", MC_FW_VERSION, mc_plat_reset_cause(),
         (unsigned)MC_NUM_DC, (unsigned)MC_NUM_BOARDS);
}

void mc_app_poll(void)
{
    uint32_t now = mc_plat_now_ms();

    /* Receive first so freshly queued lines go out in this same pass. */
    service_rx(MC_PORT_PC, now);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        service_rx(MC_PORT_DC(k), now);

    service_health(now);
    if (status_requested) {
        status_requested = 0;
        report_status(now);
    }

    for (unsigned i = 0; i < MC_NUM_PORTS; ++i)
        if (mc_plat_port_present(i))
            service_tx(i, now);
}

uint32_t mc_app_last_activity_ms(void) { return last_activity_ms; }

int mc_app_fault_active(void)
{
    uint32_t now = mc_plat_now_ms();
    if (had_drop && elapsed(now, last_drop_ms) < 2000u)
        return 1;
    if (elapsed(now, boot_ms) < MC_STARTUP_GRACE_MS)
        return 0;
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        if (!dc(k)->online)
            return 1;
    return 0;
}

void mc_app_note_loop_time(uint32_t ms)
{
    if (ms > loop_max_ms)
        loop_max_ms = ms;
}

void mc_app_request_status(void) { status_requested = 1; }
