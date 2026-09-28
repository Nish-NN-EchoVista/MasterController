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
 *  - A port that stops carrying traffic is rebuilt, then quarantined and
 *    retried with backoff if that does not help; broadcasts continue to the
 *    healthy DataControllers and say which ones were skipped.
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
    uint32_t      err_seen;     /* raw line-error total at the last sample   */
    uint32_t      overrun_seen; /* raw rx_overruns at the last sample        */
    uint32_t      restart_seen; /* raw rx_restarts at the last sample        */
    uint32_t      err_reported; /* raw line-error total already warned about */
    uint32_t      err_report_ms;
    uint32_t      discont_ms;   /* last "input lost" warning                 */
    /* Stream positions, in bytes since boot (wrap-safe comparisons).       */
    uint32_t      rx_pulled;    /* bytes returned by mc_plat_rx_read()       */
    uint32_t      rx_pos;       /* bytes fed to the line assembler           */
    uint32_t      boundary;     /* position just after the last line end     */
    uint32_t      taint_until;  /* PC: newest byte received at the last error */
    uint32_t      taint_ms;     /* PC: when that error was seen              */
    uint32_t      line_first_ms;/* when the current line's first byte came   */
    uint8_t       taint_active;
    uint8_t       discont_armed;
    uint8_t       resync_pending; /* input was lost: see resync_after_loss() */
    uint32_t      resync_pos, resync_quiet_ms;
    uint8_t       seen, online, missing_reported;
    /* Port health supervisor (PC link and DataController ports).           */
    uint8_t       health;       /* H_OK .. H_FAULTED                         */
    uint8_t       stall_strikes, restart_strikes, refusing, storm_reinit_done;
    const char   *fault_reason;
    uint32_t      tx_ok;        /* lines fully sent, never reset             */
    uint32_t      strike_start_ms;
    uint32_t      refused_since_ms;
    uint32_t      restart_sup_seen, restart_fail_seen;
    uint32_t      storm_start_ms, storm_err_base, storm_reinit_ms;
    uint32_t      probation_until_ms, probation_tx_base;
    uint32_t      next_probe_ms, backoff_ms;
    uint32_t      recoveries, quarantines;
} port_t;

/* Rate-limited repeated event: the first occurrence is reported at once,
   later ones are summed and reported at most every MC_WARN_INTERVAL_MS.    */
typedef struct {
    uint32_t last_ms;
    uint32_t pending;
    uint8_t  armed;
    uint8_t  used;          /* reports made in the current interval */
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
static ratelimit_t rl_busy, rl_pc_binary, rl_pc_overlong, rl_tainted;
static ratelimit_t rl_partial, rl_faulted, rl_absent, rl_lost_bcast;
static uint8_t  presence_summary_done;
static uint8_t  board_reset_pending, board_reset_declined;
static uint32_t board_reset_at_ms;
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

/* For replies to individual commands: each refused command gets its own
   reply, up to MC_REPLY_BURST per interval; only a flood is summarised.  */
static int ratelimit_reply(ratelimit_t *r, uint32_t now)
{
    if (!r->armed || elapsed(now, r->last_ms) >= MC_WARN_INTERVAL_MS) {
        r->armed = 1;
        r->last_ms = now;
        r->used = 0;
    }
    if (r->used < MC_REPLY_BURST) {
        ++r->used;
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

/* ------------------------------------------------------ port health */

/*
 * Escalation ladder, per port (thresholds in mc_config.h):
 *
 *   OK / DEGRADED --hard fault--> rebuild the UART --> PROBATION
 *   PROBATION --quiet for MC_SUP_PROBATION_MS, a line sent--> OK
 *   PROBATION --any hard fault, or nothing sent--> FAULTED
 *   FAULTED --probe timer--> rebuild --> PROBATION   (backoff 5 s .. 60 s)
 *
 * Hard faults are the port failing to carry traffic: repeated TX stalls,
 * the driver refusing to transmit, receive DMA that keeps stopping or
 * cannot restart. A storm of line errors is not one: the port still works,
 * so it is marked DEGRADED and rebuilt at most once a minute, never
 * quarantined.
 *
 * A FAULTED DataController port is quarantined: the platform stops
 * touching it, its ordinary queued lines are discarded, commands addressed
 * to its boards are refused and broadcasts go to the others. Stops for it
 * stay queued and go out first if it recovers. The PC link is never
 * quarantined (it is the only way in); it is simply retried with backoff.
 */
enum { H_OK = 0, H_DEGRADED, H_PROBATION, H_FAULTED };

static const uint8_t CRLF[2] = { '\r', '\n' };

static uint32_t line_errors(const mc_hw_stats_t *s)
{
    return s->framing + s->noise + s->parity;
}

/*
 * Input was lost at an unknown point (DMA restart, ring overrun, UART
 * rebuild). Bytes that follow at once may be the tail of a line whose start
 * was lost, so the assembler discards up to the next line end. But if the
 * port then stays quiet for longer than a maximum-length line takes on the
 * wire, whatever arrives next cannot be such a tail (lines are sent as one
 * burst) and is taken as a fresh line.
 */
static void resync_after_loss(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    uint32_t baud = mc_plat_port_baud(port);
    mc_line_resync(&p->line);
    p->resync_pending = 1;
    p->resync_pos = p->rx_pos;
    p->resync_quiet_ms = now + 1u +
        (baud ? (uint32_t)(((uint64_t)MC_LINE_MAX + 2u) * 10000u / baud) : 0u);
}

static int is_dc_port(unsigned port) { return port >= 1u && port <= MC_NUM_DC; }
static int dc_faulted(unsigned k)    { return dc(k)->health == H_FAULTED; }

static const char *health_name(uint8_t h)
{
    static const char *n[] = { "OK", "DEGRADED", "PROBATION", "FAULTED" };
    return h < 4u ? n[h] : "?";
}

static const char *port_label(unsigned port)
{
    static const char *n[MC_NUM_PORTS] = { "PC", "DC1", "DC2", "DC3", "DC4", "DC5", "DC6", "DBG" };
    return port < MC_NUM_PORTS ? n[port] : "?";
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* The line on the wire, if any, is lost by a rebuild or quarantine: part
   of it may have gone out, so terminate it before anything else.          */
static void drop_inflight(unsigned port)
{
    port_t *p = &ports[port];
    if (mc_plat_tx_busy(port))
        mc_plat_tx_abort(port);
    if (mc_txq_head_locked(&p->txq)) {
        mc_txq_pop(&p->txq);
        (void)mc_txq_push_front(&p->txq, CRLF, 2u, 0);
    }
}

static int rebuild_port(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    drop_inflight(port);
    int ok = mc_plat_port_reinit(port);
    resync_after_loss(port, now);       /* unread input was discarded */
    /* A rebuild is not a DMA failure: re-baseline what the supervisor and
       the receive path watch, so it is not counted against the port.     */
    mc_hw_stats_t hw;
    mc_plat_hw_stats(port, &hw);
    p->restart_sup_seen  = hw.rx_restarts;
    p->restart_fail_seen = hw.rx_restart_fails;
    p->restart_seen      = hw.rx_restarts;
    p->overrun_seen      = hw.rx_overruns;
    p->stall_strikes = p->restart_strikes = 0;
    p->refusing = 0;
    p->strike_start_ms = now;
    return ok;
}

static void start_probation(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    p->health = H_PROBATION;
    p->probation_until_ms = now + MC_SUP_PROBATION_MS;
    p->probation_tx_base = p->tx_ok;
    /* Prove the transmitter works even when there is nothing to send. An
       empty line is ignored by the DataController and the GUI alike.     */
    if (mc_txq_count(&p->txq) == 0u)
        (void)mc_txq_push(&p->txq, CRLF, 2u, 0, 0);
}

static void check_board_fault(uint32_t now)
{
    unsigned n = 0;
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        n += dc_faulted(k) ? 1u : 0u;
    if (n < MC_SUP_BOARD_FAULT_DCS || board_reset_pending || board_reset_declined)
        return;
    if (strcmp(mc_plat_reset_cause(), "PORT_FAULTS") == 0) {
        /* Already tried once this power cycle: stay up, keep probing. */
        board_reset_declined = 1;
        emit("E: %u DataController ports faulted again after a board reset; "
             "not resetting again, ports stay quarantined and are retried", n);
        return;
    }
    board_reset_pending = 1;
    board_reset_at_ms = now + 100u;     /* let this message reach the PC */
    emit("E: %u DataController ports faulted at once: resetting the board", n);
}

static void quarantine(unsigned port, const char *reason, uint32_t now)
{
    port_t *p = &ports[port];
    p->health = H_FAULTED;
    p->fault_reason = reason;
    ++p->quarantines;
    p->backoff_ms = p->backoff_ms ? min_u32(p->backoff_ms * 2u, MC_SUP_PROBE_MAX_MS)
                                  : MC_SUP_PROBE_FIRST_MS;
    p->next_probe_ms = now + p->backoff_ms;
    note_drop(now);
    if (!is_dc_port(port)) {
        emit("E: PC link faulted (%s); retrying in %lu s", reason,
             (unsigned long)(p->backoff_ms / 1000u));
        return;
    }
    drop_inflight(port);
    mc_plat_port_enable(port, 0);
    uint16_t purged = mc_txq_purge(&p->txq, 0xFFu);   /* ordinary lines only */
    p->c.purged += purged;
    emit("E: DC%u faulted (%s): quarantined, %u queued line(s) discarded, "
         "EVS2 @%u,@%u unavailable; retrying in %lu s", port, reason,
         (unsigned)purged, 2u * port - 1u, 2u * port,
         (unsigned long)(p->backoff_ms / 1000u));
    check_board_fault(now);
}

/* The port failed to carry traffic. */
static void hard_fault(unsigned port, const char *reason, uint32_t now)
{
    port_t *p = &ports[port];
    if (p->health == H_FAULTED)
        return;
    if (p->health == H_PROBATION) {     /* the rebuild did not help */
        quarantine(port, reason, now);
        return;
    }
    p->fault_reason = reason;
    emit("W: %s %s: rebuilding the UART", port_label(port), reason);
    if (rebuild_port(port, now))
        start_probation(port, now);
    else
        quarantine(port, "reinit_failed", now);
}

static void strike_window(port_t *p, uint32_t now)
{
    if (elapsed(now, p->strike_start_ms) >= MC_SUP_WINDOW_MS) {
        p->strike_start_ms = now;
        p->stall_strikes = p->restart_strikes = 0;
    }
}

static void sup_tx_stalled(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    if (port == MC_PORT_DBG)
        return;
    strike_window(p, now);
    if (++p->stall_strikes >= MC_SUP_STALL_STRIKES || p->health == H_PROBATION)
        hard_fault(port, "tx_stalls", now);
}

static void sup_tx_refused(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    if (!p->refusing) {
        p->refusing = 1;
        p->refused_since_ms = now;
    } else if (elapsed(now, p->refused_since_ms) >= MC_SUP_REFUSED_MS) {
        hard_fault(port, "tx_refused", now);
    }
}

static void supervise_port(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    mc_hw_stats_t hw;
    mc_plat_hw_stats(port, &hw);

    if (p->health == H_FAULTED) {
        if ((int32_t)(now - p->next_probe_ms) < 0)
            return;
        emit("I: %s probing: rebuilding the UART", port_label(port));
        if (rebuild_port(port, now)) {
            mc_plat_port_enable(port, 1);
            start_probation(port, now);
        } else {
            p->backoff_ms = min_u32(p->backoff_ms * 2u, MC_SUP_PROBE_MAX_MS);
            p->next_probe_ms = now + p->backoff_ms;
        }
        return;
    }

    /* Receive DMA that keeps stopping, or cannot be restarted at all. */
    strike_window(p, now);
    if (hw.rx_restart_fails != p->restart_fail_seen) {
        p->restart_fail_seen = hw.rx_restart_fails;
        p->restart_sup_seen = hw.rx_restarts;
        hard_fault(port, "rx_dma_failed", now);
        return;
    }
    if (hw.rx_restarts != p->restart_sup_seen) {
        uint32_t d = hw.rx_restarts - p->restart_sup_seen;
        p->restart_sup_seen = hw.rx_restarts;
        p->restart_strikes = (uint8_t)min_u32(p->restart_strikes + d, 255u);
        if (p->restart_strikes >= MC_SUP_RESTART_STRIKES || p->health == H_PROBATION) {
            hard_fault(port, "rx_dma_restarts", now);
            return;
        }
    }

    /* Line-error storm: degraded, never quarantined. */
    if (elapsed(now, p->storm_start_ms) >= MC_SUP_STORM_WINDOW_MS) {
        uint32_t errs = line_errors(&hw);
        uint32_t d = errs - p->storm_err_base;
        p->storm_err_base = errs;
        p->storm_start_ms = now;
        if (d >= MC_SUP_STORM_ERRORS && p->health == H_OK) {
            p->health = H_DEGRADED;
            p->fault_reason = "line_errors";
            emit("W: %s degraded: %lu line errors in %lu ms (check wiring, ground, baud)",
                 port_label(port), (unsigned long)d, (unsigned long)MC_SUP_STORM_WINDOW_MS);
            if (!p->storm_reinit_done ||
                elapsed(now, p->storm_reinit_ms) >= MC_SUP_STORM_REINIT_MS) {
                p->storm_reinit_done = 1;
                p->storm_reinit_ms = now;
                if (!rebuild_port(port, now))
                    quarantine(port, "reinit_failed", now);
            }
        } else if (d < MC_SUP_STORM_ERRORS && p->health == H_DEGRADED) {
            p->health = H_OK;
            emit("I: %s line errors subsided", port_label(port));
        }
    }

    if (p->health == H_PROBATION && (int32_t)(now - p->probation_until_ms) >= 0) {
        if (p->tx_ok != p->probation_tx_base) {
            p->health = H_OK;
            ++p->recoveries;
            p->backoff_ms = 0;
            emit("I: %s recovered (%s)", port_label(port), p->fault_reason);
        } else {
            quarantine(port, "no_tx_in_probation", now);
        }
    }
}

static void service_supervisor(uint32_t now)
{
    supervise_port(MC_PORT_PC, now);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        supervise_port(MC_PORT_DC(k), now);
    if (board_reset_pending && (int32_t)(now - board_reset_at_ms) >= 0) {
        board_reset_pending = 0;
        board_reset_declined = 1;       /* in case the platform returns */
        mc_plat_system_reset();
    }
}

/* mc_recover <1..6|pc|all>: rebuild now (or probe a faulted port now). */
static void manual_recover(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    if (p->health == H_FAULTED) {
        p->next_probe_ms = now;
        emit("I: %s: probe requested", port_label(port));
        return;
    }
    emit("I: %s: rebuilding the UART on request", port_label(port));
    p->fault_reason = "manual";
    if (rebuild_port(port, now))
        start_probation(port, now);
    else
        quarantine(port, "reinit_failed", now);
}

/* ------------------------------------------------------- presence */

/*
 * A DataController port is, at any moment:
 *   LIVE    traffic within MC_DC_SILENT_MS (every F401 prints every 500 ms)
 *   ABSENT  no traffic at all since boot: nothing plugged in. Quiet: no
 *           warning, no red LED. Running with fewer than six is normal.
 *   LOST    was live, then went silent: a cable or DataController failed.
 *           Warned about, lights the red LED.
 * Computed from the receive timestamps directly, so a command is never
 * judged by a status flag that is one poll out of date.
 */
enum { P_ABSENT = 0, P_LIVE, P_LOST };

static int dc_presence(unsigned k, uint32_t now)
{
    const port_t *p = dc(k);
    if (!p->seen)
        return P_ABSENT;
    return elapsed(now, p->last_rx_ms) < MC_DC_SILENT_MS ? P_LIVE : P_LOST;
}

static const char *presence_name(int pr)
{
    return pr == P_LIVE ? "live" : pr == P_LOST ? "lost" : "absent";
}

/* "DC3 (port 3, UART4 PA0/PA1, @5,@6)" */
static int describe_dc(char *buf, size_t cap, unsigned k)
{
    return snprintf(buf, cap, "DC%u (port %u, %s, @%u,@%u)", k, k,
                    mc_plat_port_name(MC_PORT_DC(k)), 2u * k - 1u, 2u * k);
}

/* List the DataControllers in one presence state: "DC1 DC4" or "none". */
static void list_presence(char *buf, size_t cap, int pr, uint32_t now)
{
    size_t n = 0;
    buf[0] = '\0';
    for (unsigned k = 1; k <= MC_NUM_DC; ++k) {
        if (dc_presence(k, now) != pr) continue;
        int w = snprintf(buf + n, cap - n, "%sDC%u", n ? " " : "", k);
        if (w > 0 && (size_t)w < cap - n) n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "none");
}

/* dc_status: which DataControllers are live, absent and lost. */
static void report_dc_status(uint32_t now)
{
    unsigned count[3] = { 0, 0, 0 };
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        ++count[dc_presence(k, now)];
    emit("dc_status: %u live, %u absent, %u lost", count[P_LIVE], count[P_ABSENT],
         count[P_LOST]);
    static const int order[3] = { P_LIVE, P_ABSENT, P_LOST };
    for (unsigned i = 0; i < 3u; ++i) {
        for (unsigned k = 1; k <= MC_NUM_DC; ++k) {
            int pr = dc_presence(k, now);
            if (pr != order[i]) continue;
            char d[64], extra[64];
            const port_t *p = dc(k);
            describe_dc(d, sizeof d, k);
            extra[0] = '\0';
            if (pr == P_LIVE)
                snprintf(extra, sizeof extra, " last_rx=%lums",
                         (unsigned long)elapsed(now, p->last_rx_ms));
            else if (pr == P_LOST)
                snprintf(extra, sizeof extra, " silent for %lu s",
                         (unsigned long)(elapsed(now, p->last_rx_ms) / 1000u));
            if (p->health != H_OK) {
                size_t e = strlen(extra);
                snprintf(extra + e, sizeof extra - e, " [%s: %s]",
                         health_name(p->health), p->fault_reason ? p->fault_reason : "");
            }
            emit("  %-6s %s%s", presence_name(pr), d, extra);
        }
    }
}

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
        } else if (p->health == H_FAULTED) {
            emit("W: DC%u is faulted: '%.*s' queued, sent first if it recovers",
                 k, clip(r->payload_len), r->payload);
        }
    }
}

static void send_normal(const mc_route_t *r, const uint8_t *out, uint16_t n,
                        unsigned first, unsigned last, uint32_t now,
                        const char *line, uint16_t len)
{
    /* Faulted DataControllers: an addressed line is refused; a broadcast
       goes to the healthy ones and says which were skipped.              */
    char skipped[40];
    size_t sl = 0;
    unsigned available = 0;
    skipped[0] = '\0';
    for (unsigned k = first; k <= last; ++k) {
        if (!dc_faulted(k)) { ++available; continue; }
        if (r->kind != MC_ROUTE_BROADCAST) {
            ++dc(k)->c.rejected;
            note_drop(now);
            if (ratelimit_reply(&rl_faulted, now))
                emit("E: DC%u is faulted (%s), not sent: %.*s", k,
                     dc(k)->fault_reason, clip(len), line);
            return;
        }
        int w = snprintf(skipped + sl, sizeof skipped - sl, "%sDC%u", sl ? " " : "", k);
        if (w > 0 && (size_t)w < sizeof skipped - sl) sl += (size_t)w;
    }
    if (!available) {
        note_drop(now);
        if (ratelimit_reply(&rl_faulted, now))
            emit("E: no DataController available (all faulted), not sent: %.*s",
                 clip(len), line);
        return;
    }

    /* All-or-nothing across the available targets: check every one before
       committing to any.                                                 */
    for (unsigned k = first; k <= last; ++k) {
        if (dc_faulted(k)) continue;
        if (!has_room(dc(k))) {
            ++dc(k)->c.rejected;
            ++busy_total;
            note_drop(now);
            if (ratelimit_reply(&rl_busy, now))
                emit("E: busy (DC%u queue full), not sent: %.*s", k, clip(len), line);
            return;
        }
    }
    for (unsigned k = first; k <= last; ++k)
        if (!dc_faulted(k))
            (void)mc_txq_push(&dc(k)->txq, out, n, r->tag, 0);
    if (sl && ratelimit_hit(&rl_partial, now))
        emit("W: sent to %u of %u DataControllers (faulted: %s): %.*s",
             available, (unsigned)MC_NUM_DC, skipped, clip(len), line);
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

    if (r.kind != MC_ROUTE_BROADCAST) {
        /* Addressed: an ordinary command to a DataController that is not
           there is refused with a reply instead of vanishing. Stops always
           go out: a silent DataController may still be running.          */
        int pr = dc_presence(r.dc, now);
        if (pr != P_LIVE && !r.urgent) {
            ++dc(r.dc)->c.rejected;
            if (ratelimit_reply(&rl_absent, now)) {
                if (pr == P_ABSENT)
                    emit("E: DC%u (port %u) not connected, not sent: %.*s",
                         r.dc, r.dc, clip(len), line);
                else
                    emit("E: DC%u (port %u) lost, silent for %lu s, not sent: %.*s",
                         r.dc, r.dc,
                         (unsigned long)(elapsed(now, dc(r.dc)->last_rx_ms) / 1000u),
                         clip(len), line);
            }
            return;
        }
        if (pr == P_LOST)
            emit("W: DC%u (port %u) is lost; '%.*s' sent anyway", r.dc, r.dc,
                 clip(r.payload_len), r.payload);
    } else {
        /* Broadcast: every port gets it, connected or not. Only lost
           DataControllers are worth a word: they were there before.      */
        char lost[40];
        list_presence(lost, sizeof lost, P_LOST, now);
        if (strcmp(lost, "none") != 0 && ratelimit_hit(&rl_lost_bcast, now))
            emit("W: broadcast also sent to lost DataController(s) %s: %.*s",
                 lost, clip(len), line);
    }

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
         "garbled=%lu uart_err_lines=%lu fe=%lu ne=%lu overruns=%lu stalls=%lu "
         "health=%s recoveries=%lu",
         mc_plat_port_name(MC_PORT_PC), (unsigned long)mc_plat_port_baud(MC_PORT_PC),
         (unsigned long)p->c.rx_bytes, (unsigned long)p->c.rx_lines,
         (unsigned long)p->c.tx_lines, (unsigned)mc_txq_count(&p->txq),
         (unsigned)MC_PC_TXQ_SLOTS, (unsigned long)p->c.overlong,
         (unsigned long)p->c.binary, (unsigned long)p->c.garbled,
         (unsigned long)p->c.tainted, (unsigned long)h.framing,
         (unsigned long)h.noise, (unsigned long)h.rx_overruns,
         (unsigned long)p->c.stalls, health_name(p->health),
         (unsigned long)p->recoveries);
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
             "overruns=%lu dma_restarts=%lu stalls=%lu health=%s%s%s%s "
             "recoveries=%lu quarantines=%lu",
             k, mc_plat_port_name(MC_PORT_DC(k)), presence_name(dc_presence(k, now)),
             2u * k - 1u, 2u * k, age,
             (unsigned long)p->c.rx_bytes, (unsigned long)p->c.rx_lines,
             (unsigned long)p->c.up_dropped, (unsigned long)p->c.tx_lines,
             (unsigned)mc_txq_count(&p->txq), (unsigned)MC_DC_TXQ_SLOTS,
             (unsigned long)p->c.rejected, (unsigned long)p->c.purged,
             (unsigned long)p->c.overlong, (unsigned long)p->c.binary,
             (unsigned long)h.framing, (unsigned long)h.noise,
             (unsigned long)h.rx_overruns, (unsigned long)h.rx_restarts,
             (unsigned long)p->c.stalls, health_name(p->health),
             p->health != H_OK ? "(" : "",
             p->health != H_OK && p->fault_reason ? p->fault_reason : "",
             p->health != H_OK ? ")" : "",
             (unsigned long)p->recoveries, (unsigned long)p->quarantines);
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
    } else if (len >= 9u && memcmp(line, "dc_status", 9) == 0) {
        report_dc_status(now);
    } else if (IS("mc_version")) {
        emit("MasterController %s (built %s %s), %u DataControllers x %u EVS2",
             MC_FW_VERSION, __DATE__, __TIME__, (unsigned)MC_NUM_DC,
             (unsigned)MC_BOARDS_PER_DC);
    } else if (IS("mc_ping")) {
        emit("pong");
    } else if (IS("mc_reset_stats")) {
        reset_stats(now);
        emit("statistics reset");
    } else if (len >= 11u && memcmp(line, "mc_recover ", 11) == 0) {
        const char *a = line + 11;
        size_t al = len - 11u;
        if (al == 3u && memcmp(a, "all", 3) == 0) {
            manual_recover(MC_PORT_PC, now);
            for (unsigned k = 1; k <= MC_NUM_DC; ++k)
                manual_recover(MC_PORT_DC(k), now);
        } else if (al == 2u && memcmp(a, "pc", 2) == 0) {
            manual_recover(MC_PORT_PC, now);
        } else if (al == 1u && a[0] >= '1' && a[0] <= (char)('0' + MC_NUM_DC)) {
            manual_recover(MC_PORT_DC((unsigned)(a[0] - '0')), now);
        } else {
            emit("E: usage: mc_recover <1..%u|pc|all>", (unsigned)MC_NUM_DC);
        }
    } else if (IS("mc_help")) {
        emit("@1..@12 <cmd>: one EVS2 | #1..#6 <cmd>: one DataController | "
             "<cmd>: all six DataControllers");
        emit("stop, cancel, @0 stop_all: sent ahead of queued lines, which are discarded");
        emit("dc_status mc_status mc_version mc_ping mc_reset_stats "
             "mc_recover <1..6|pc|all> mc_help");
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
            /* Part of the aborted line may have gone out. Terminate it before
               anything else is sent, including queued stops, so it cannot
               merge into the next line.                                    */
            (void)mc_txq_push_front(&p->txq, CRLF, 2u, 0);
            sup_tx_stalled(port, now);
        }
        return;
    }
    if (mc_txq_head_locked(&p->txq)) {           /* previous line finished */
        const mc_slot_t *done = mc_txq_head(&p->txq);
        ++p->c.tx_lines;
        ++p->tx_ok;
        p->c.tx_bytes += done->len;
        mc_txq_pop(&p->txq);
    }
    const mc_slot_t *next = mc_txq_head(&p->txq);
    if (!next)
        return;
    if (!mc_plat_tx_start(port, next->data, next->len)) {
        if (port != MC_PORT_DBG)
            sup_tx_refused(port, now);
        return;
    }
    p->refusing = 0;
    mc_txq_lock_head(&p->txq);
    p->tx_start_ms = now;
    /* Wire time of 10 bits per byte, plus a generous margin. */
    uint32_t baud = mc_plat_port_baud(port);
    p->tx_budget_ms = (baud ? (uint32_t)next->len * 10000u / baud : 0u) +
                      MC_TX_STALL_MARGIN_MS;
}

/* Signed distance between two stream positions (wrap-safe). */
static int32_t pos_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

/* Mark everything received so far as possibly damaged. The damage lies at
   or before the newest byte received: those already read plus those still
   waiting in the ring (sampled after the error flags, see mc_platform.h). */
static void taint_received(port_t *p, unsigned port, uint32_t now)
{
    uint32_t until = p->rx_pulled + (uint32_t)mc_plat_rx_pending(port);
    if (!p->taint_active || pos_diff(until, p->taint_until) > 0)
        p->taint_until = until;
    p->taint_ms = now;
    p->taint_active = 1;
}

/* Could a PC line that began at stream position start (its first byte fed
   at first_ms) contain the byte lost to the recorded UART error?          */
static int line_tainted(const port_t *p, uint32_t start, uint32_t first_ms)
{
    if (!p->taint_active)
        return 0;
    int32_t d = pos_diff(start, p->taint_until);
    if (d < 0)
        return 1;                    /* holds bytes received before the error */
    return d == 0 && elapsed(first_ms, p->taint_ms) < MC_TAINT_GRACE_MS;
}

/*
 * Receive integrity.
 *
 * Line errors (PC link): the UART drops a byte received with a framing or
 * noise error. It could be a terminator (two commands merge) or a letter
 * ("stop" becomes something else). The error flags do not say where in the
 * stream the byte was, only that it was received before the flags were
 * sampled. So every line holding a byte received before that moment is
 * discarded, and so is a line starting exactly there if its first byte
 * arrives within MC_TAINT_GRACE_MS (the lost byte may have been its first
 * character). This may discard clean lines, but a damaged line is never
 * forwarded, however much input was buffered.
 *
 * Discontinuities (any port): a DMA restart or a ring overrun loses input
 * at an unknown point. The line assembler is resynchronised, discarding up
 * to the next terminator, so the tail of a line can never be taken as a
 * complete line. Input after that terminator follows the loss and is kept.
 */
static void service_rx(unsigned port, uint32_t now)
{
    port_t *p = &ports[port];
    uint8_t buf[256];
    size_t total = 0, n;
    /* Bounded per pass so one busy port cannot starve the others. */
    for (;;) {
        n = total < 4096u ? mc_plat_rx_read(port, buf, sizeof buf) : 0u;
        p->rx_pulled += (uint32_t)n;

        mc_hw_stats_t hw;
        mc_plat_hw_stats(port, &hw);            /* after the read: see above */
        if (line_errors(&hw) != p->err_seen) {
            p->err_seen = line_errors(&hw);
            if (port == MC_PORT_PC)
                taint_received(p, port, now);
        }
        if (hw.rx_overruns != p->overrun_seen || hw.rx_restarts != p->restart_seen) {
            int restarted = hw.rx_restarts != p->restart_seen;
            p->overrun_seen = hw.rx_overruns;
            p->restart_seen = hw.rx_restarts;
            resync_after_loss(port, now);
            note_drop(now);
            if (!p->discont_armed || elapsed(now, p->discont_ms) >= MC_WARN_INTERVAL_MS) {
                p->discont_armed = 1;
                p->discont_ms = now;
                emit("W: %s input lost (%s); discarding up to the next line end",
                     port == MC_PORT_PC ? "PC" : mc_plat_port_name(port),
                     restarted ? "receive DMA restarted" : "main loop stalled, buffer overflowed");
            }
        }
        if (n == 0u)
            break;

        if (p->resync_pending) {
            if (p->rx_pos != p->resync_pos) {
                p->resync_pending = 0;          /* a tail is being discarded */
            } else if ((int32_t)(now - p->resync_quiet_ms) >= 0) {
                /* Quiet since the loss: these bytes start a fresh line. */
                mc_line_init(&p->line);
                p->boundary = p->rx_pos;
                p->resync_pending = 0;
            }
        }

        total += n;
        p->c.rx_bytes += (uint32_t)n;
        p->last_rx_ms = now;
        p->seen = 1;
        for (size_t i = 0; i < n; ++i) {
            if (p->rx_pos == p->boundary)       /* first byte of a new line */
                p->line_first_ms = now;
            ++p->rx_pos;
            mc_line_event_t e = mc_line_feed(&p->line, buf[i]);
            if (e == MC_LINE_BOUNDARY) {
                p->boundary = p->rx_pos;
            } else if (e == MC_LINE_READY) {
                uint32_t start = p->boundary;   /* line began after this */
                p->boundary = p->rx_pos;
                ++p->c.rx_lines;
                if (port != MC_PORT_PC) {
                    handle_dc_line(port, p->line.buf, p->line.len, now);
                } else if (line_tainted(p, start, p->line_first_ms)) {
                    ++p->c.tainted;
                    note_drop(now);
                    if (ratelimit_hit(&rl_tainted, now))
                        emit("E: line received with UART errors, not sent: %.*s",
                             clip(p->line.len), p->line.buf);
                } else {
                    p->taint_active = 0;        /* starts after the damage */
                    handle_pc_line(p->line.buf, p->line.len, now);
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
        int alive = dc_presence(k, now) == P_LIVE;
        if (alive && !p->online) {
            char d[64];
            describe_dc(d, sizeof d, k);
            emit(p->missing_reported ? "I: %s back online" : "I: %s online", d);
            p->online = 1;
        } else if (!alive && p->online) {
            char d[64];
            describe_dc(d, sizeof d, k);
            p->online = 0;
            p->missing_reported = 1;            /* now "lost", not "absent" */
            emit("W: %s lost: silent for %lu ms", d,
                 (unsigned long)elapsed(now, p->last_rx_ms));
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
    if (after_grace && !presence_summary_done) {
        /* One informational line once the DataControllers have had time to
           speak. Absent ports are normal: no warning, no red LED.         */
        char live[40], absent[40];
        presence_summary_done = 1;
        list_presence(live, sizeof live, P_LIVE, now);
        list_presence(absent, sizeof absent, P_ABSENT, now);
        emit("I: DataControllers live: %s; absent (not connected): %s", live, absent);
    }
    ratelimit_flush(&rl_busy, now, "E: busy, further lines not sent");
    ratelimit_flush(&rl_absent, now, "E: commands to absent/lost DataControllers not sent");
    ratelimit_flush(&rl_pc_binary, now, "E: binary frames discarded");
    ratelimit_flush(&rl_pc_overlong, now, "E: overlong lines discarded");
    ratelimit_flush(&rl_tainted, now, "E: lines received with UART errors not sent");
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
    memset(&rl_tainted, 0, sizeof rl_tainted);
    memset(&rl_partial, 0, sizeof rl_partial);
    memset(&rl_faulted, 0, sizeof rl_faulted);
    memset(&rl_absent, 0, sizeof rl_absent);
    memset(&rl_lost_bcast, 0, sizeof rl_lost_bcast);
    presence_summary_done = 0;
    board_reset_pending = board_reset_declined = 0;
    boot_ms = mc_plat_now_ms();
    last_activity_ms = boot_ms;
    status_requested = 0;
    reset_stats(boot_ms);
    /* Errors counted before this boot of the app are not "new". */
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i) {
        if (!mc_plat_port_present(i)) continue;
        mc_hw_stats_t hw;
        mc_plat_hw_stats(i, &hw);
        ports[i].err_seen = ports[i].err_reported = line_errors(&hw);
        ports[i].overrun_seen = hw.rx_overruns;
        ports[i].restart_seen = hw.rx_restarts;
        ports[i].restart_sup_seen = hw.rx_restarts;
        ports[i].restart_fail_seen = hw.rx_restart_fails;
        ports[i].storm_err_base = line_errors(&hw);
        ports[i].storm_start_ms = boot_ms;
        ports[i].strike_start_ms = boot_ms;
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

    service_supervisor(now);

    for (unsigned i = 0; i < MC_NUM_PORTS; ++i)
        if (mc_plat_port_present(i) && !(is_dc_port(i) && ports[i].health == H_FAULTED))
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
    if (pc()->health != H_OK)
        return 1;
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        if (dc_presence(k, now) == P_LOST || dc(k)->health != H_OK)
            return 1;
    return 0;
}

void mc_app_note_loop_time(uint32_t ms)
{
    if (ms > loop_max_ms)
        loop_max_ms = ms;
}

void mc_app_request_status(void) { status_requested = 1; }
