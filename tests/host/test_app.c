/* End-to-end tests of mc_app against the fake platform. */
#include "mc_test.h"
#include "fake_platform.h"
#include "mc_app.h"
#include "mc_config.h"
#include <stdlib.h>

#define PC  MC_PORT_PC
#define DC(k) MC_PORT_DC(k)

static void pump(int n) { while (n-- > 0) mc_app_poll(); }

/* Fresh app with every DataController already talking, boot noise cleared. */
static void boot(void)
{
    fake_reset();
    mc_app_init();
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        fake_inject(DC(k), "PZT Temp: 24.0 C\r\n");
    pump(50);
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i)
        fake_out_clear(i);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += len) ++n;
    return n;
}

/* Every CRLF-terminated line of s must satisfy pred; returns line count. */
static int all_lines(const char *s, int (*pred)(const char *, size_t))
{
    int n = 0;
    const char *p = s;
    while (*p) {
        const char *e = strstr(p, "\r\n");
        if (!e) return -1;                      /* unterminated tail       */
        if (!pred(p, (size_t)(e - p))) return -1;
        ++n;
        p = e + 2;
    }
    return n;
}

static void test_boot_banner(void)
{
    fake_reset();
    mc_app_init();
    pump(3);
    CHECK(strstr(fake_out(PC), "[MC] MasterController " MC_FW_VERSION " ready (reset: TEST)") != NULL);
    CHECK(strstr(fake_out(MC_PORT_DBG), "[MC] MasterController") != NULL);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(fake_out_len(DC(k)) == 0);        /* nothing sent unasked    */
}

static void test_board_routing(void)
{
    boot();
    fake_inject(PC, "@7 start_sweep\r\n");
    pump(5);
    CHECK_STR(fake_out(DC(4)), "@1 start_sweep\r\n");
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        if (k != 4) CHECK(fake_out_len(DC(k)) == 0);

    fake_inject(PC, "@12 get freq\n@1 get volt\r");  /* LF-only and CR-only */
    pump(5);
    CHECK_STR(fake_out(DC(6)), "@2 get freq\r\n");
    CHECK_STR(fake_out(DC(1)), "@1 get volt\r\n");
    CHECK(fake_out_len(PC) == 0);               /* no echo from the MC     */
}

static void test_dc_raw_and_broadcast(void)
{
    boot();
    fake_inject(PC, "#3 enable_pump\r\n");
    pump(5);
    CHECK_STR(fake_out(DC(3)), "enable_pump\r\n");
    CHECK(fake_out_len(DC(2)) == 0);

    fake_inject(PC, "start_defog\r\n");
    pump(5);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(strstr(fake_out(DC(k)), "start_defog\r\n") != NULL);

    fake_inject(PC, "@0 play_all\r\n");
    pump(5);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(strstr(fake_out(DC(k)), "@0 play_all\r\n") != NULL);
}

static void test_invalid_rejected(void)
{
    boot();
    fake_inject(PC, "@13 start_sweep\r\n#9 x\r\nmc_nope\r\n");
    pump(5);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(fake_out_len(DC(k)) == 0);
    CHECK(strstr(fake_out(PC), "[MC] E: invalid EVS2 address") != NULL);
    CHECK(strstr(fake_out(PC), "@13 start_sweep") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] E: invalid DataController address") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] E: unknown MasterController command: mc_nope") != NULL);
}

static void test_upstream_retag(void)
{
    boot();
    fake_inject(DC(3), "[ESV2-1] hello\r\n[ESV2-2] world\r\nPZT Temp: 25.0 C\r\n");
    fake_inject(DC(6), "TX to ESV2-2: start_sweep\r\nBoard1 Temp: 30\r\n");
    pump(20);
    const char *o = fake_out(PC);
    CHECK(strstr(o, "[ESV2-5] hello\r\n") != NULL);
    CHECK(strstr(o, "[ESV2-6] world\r\n") != NULL);
    CHECK(strstr(o, "[DC-3] PZT Temp: 25.0 C\r\n") != NULL);
    CHECK(strstr(o, "TX to ESV2-12: start_sweep\r\n") != NULL);
    CHECK(strstr(o, "Board11 Temp: 30\r\n") != NULL);
}

static int is_whole_tagged(const char *s, size_t n)
{
    /* Each line must be one of the two complete test lines. */
    return (n == 40 && memcmp(s, "[ESV2-1] ", 9) == 0 && s[9] == 'a' && s[39] == 'a') ||
           (n == 40 && memcmp(s, "[ESV2-4] ", 9) == 0 && s[9] == 'b' && s[39] == 'b');
}

static void test_upstream_never_interleaves(void)
{
    char a[64], b[64];
    boot();
    memset(a, 'a', 31); a[31] = '\0';
    memset(b, 'b', 31); b[31] = '\0';
    /* Two DataControllers deliver their lines in small interleaved pieces. */
    for (int round = 0; round < 20; ++round) {
        fake_inject(DC(1), "[ESV2-1] ");
        fake_inject(DC(2), "[ESV2-2] ");
        pump(1);
        fake_inject(DC(1), a);
        pump(1);
        fake_inject(DC(2), b);
        fake_inject(DC(1), "\r\n");
        pump(1);
        fake_inject(DC(2), "\r\n");
        pump(1);
    }
    pump(100);
    CHECK(all_lines(fake_out(PC), is_whole_tagged) == 40);
}

static void test_broadcast_all_or_nothing(void)
{
    boot();
    fake_tx_mode(DC(3), FAKE_TX_HOLD);
    /* Fill DataController 3 exactly up to its reserve with @5 traffic. */
    char line[32];
    int accepted = 0;
    const int fill = (int)(MC_DC_TXQ_SLOTS - MC_TXQ_RESERVE);
    for (int i = 0; i < fill; ++i) {
        snprintf(line, sizeof line, "@5 fill %d\r\n", i);
        fake_inject(PC, line);
        pump(1);
    }
    CHECK(strstr(fake_out(PC), "busy") == NULL);
    fake_inject(PC, "start_sweep\r\n");
    pump(5);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(strstr(fake_out(DC(k)), "start_sweep") == NULL);   /* nobody got it */
    CHECK(strstr(fake_out(PC), "[MC] E: busy (DC3 queue full), not sent: start_sweep") != NULL);

    /* Drain DataController 3 and the same broadcast now reaches everyone. */
    while (fake_complete(DC(3))) { pump(1); ++accepted; }
    CHECK(accepted == fill);
    fake_tx_mode(DC(3), FAKE_TX_AUTO);
    pump(2);
    fake_inject(PC, "start_sweep\r\n");
    pump(5);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(strstr(fake_out(DC(k)), "start_sweep\r\n") != NULL);
    /* Fill lines were delivered in order, none lost, none duplicated. */
    const char *o = fake_out(DC(3));
    CHECK(strstr(o, "@1 fill 0\r\n@1 fill 1\r\n@1 fill 2\r\n") == o);
    CHECK(count_of(o, "@1 fill ") == (int)(MC_DC_TXQ_SLOTS - MC_TXQ_RESERVE));
}

static void test_stop_overtakes_and_purges_same_board(void)
{
    boot();
    fake_tx_mode(DC(2), FAKE_TX_HOLD);
    fake_inject(PC, "@3 first\r\n");            /* goes on the wire        */
    pump(2);
    fake_inject(PC, "@3 x1\r\n@4 y1\r\n@3 x2\r\n@4 y2\r\n#2 both\r\n");
    pump(5);
    fake_out_clear(PC);
    fake_inject(PC, "@3 stop\r\n");
    pump(3);
    CHECK(strstr(fake_out(PC), "[MC] W: 'stop' discarded 3 queued line(s) for DC2") != NULL);
    while (fake_complete(DC(2))) pump(1);
    pump(3);
    CHECK_STR(fake_out(DC(2)), "@1 first\r\n@1 stop\r\n@2 y1\r\n@2 y2\r\n");
}

static void test_broadcast_stop_purges_everything(void)
{
    boot();
    for (unsigned k = 1; k <= MC_NUM_DC; ++k) fake_tx_mode(DC(k), FAKE_TX_HOLD);
    fake_inject(PC, "start_sweep\r\n");
    pump(2);                                    /* in flight everywhere    */
    fake_inject(PC, "@1 a\r\n@2 b\r\n@11 c\r\nstart_defog\r\n");
    pump(5);
    fake_inject(PC, "stop\r\n");
    pump(3);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k) {
        while (fake_complete(DC(k))) pump(1);
    }
    pump(3);
    CHECK_STR(fake_out(DC(1)), "start_sweep\r\nstop\r\n");
    CHECK_STR(fake_out(DC(4)), "start_sweep\r\nstop\r\n");
    CHECK_STR(fake_out(DC(6)), "start_sweep\r\nstop\r\n");
}

static void test_later_stop_never_discards_earlier_stop(void)
{
    boot();
    fake_tx_mode(DC(1), FAKE_TX_HOLD);
    fake_inject(PC, "@1 long_running_command\r\n");
    pump(2);
    fake_inject(PC, "@1 queued\r\n@0 stop_all\r\n");
    pump(2);
    fake_inject(PC, "cancel\r\n@2 stop\r\n");
    pump(3);
    while (fake_complete(DC(1))) pump(1);
    pump(2);
    CHECK_STR(fake_out(DC(1)),
              "@1 long_running_command\r\n@0 stop_all\r\ncancel\r\n@2 stop\r\n");
}

static void test_stop_fits_even_when_queue_full(void)
{
    boot();
    fake_tx_mode(DC(1), FAKE_TX_HOLD);
    char line[32];
    for (int i = 0; i < (int)MC_DC_TXQ_SLOTS; ++i) {  /* fill with board 2 */
        snprintf(line, sizeof line, "@2 fill %d\r\n", i);
        fake_inject(PC, line);
        pump(1);
    }
    fake_out_clear(PC);
    fake_inject(PC, "@1 stop\r\n");             /* different board: no purge */
    pump(2);
    CHECK(strstr(fake_out(PC), "NOT sent") == NULL);
    fake_complete(DC(1));
    pump(2);
    fake_complete(DC(1));
    pump(2);
    CHECK(strstr(fake_out(DC(1)), "@2 fill 0\r\n@1 stop\r\n") != NULL);
}

static int no_nul(const char *s, size_t n) { (void)s; (void)n; return 1; }

static void test_pc_binary_and_overlong_never_forwarded(void)
{
    boot();
    static const char frame[] = "\0\x05stop\x0a\r\n\x01\0";
    fake_inject_bytes(PC, frame, sizeof frame - 1u);
    fake_inject(PC, "@1 ok\r\n");
    char *big = malloc(MC_LINE_MAX + 20u);
    memset(big, 'z', MC_LINE_MAX + 10u);
    memcpy(big + MC_LINE_MAX + 10u, "\r\n", 3);
    fake_inject(PC, big);
    free(big);
    fake_inject(PC, "@1 ok2\r\n");
    pump(10);
    CHECK_STR(fake_out(DC(1)), "@1 ok\r\n@1 ok2\r\n");
    for (unsigned k = 2; k <= MC_NUM_DC; ++k)
        CHECK(fake_out_len(DC(k)) == 0);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(memchr(fake_out(DC(k)), 0, fake_out_len(DC(k))) == NULL);
    CHECK(strstr(fake_out(PC), "binary protocol frames are not supported") != NULL);
    CHECK(strstr(fake_out(PC), "line longer than 512 characters discarded") != NULL);
    (void)no_nul;
}

static void test_pc_garbage_never_forwarded(void)
{
    boot();
    /* A connect glitch glued onto the next command, plus a stray byte. */
    fake_inject(PC, "\xff" "start_sweep\r\n@3 get\x80" "freq\r\n");
    fake_inject(PC, "start_sweep\r\n@4 get\tfreq\r\n");   /* tab is fine */
    pump(10);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK_STR(fake_out(DC(k)), k == 2 ? "start_sweep\r\n@2 get\tfreq\r\n"
                                          : "start_sweep\r\n");
    CHECK(count_of(fake_out(PC), "[MC] E: line contains non-printable characters") == 2);
    fake_inject(PC, "mc_status\r\n");
    pump(20);
    CHECK(strstr(fake_out(PC), "garbled=2 ") != NULL);
}

static void test_pc_uart_errors_discard_affected_lines(void)
{
    mc_hw_stats_t hw = { 0, 0, 0, 0, 0, 0 };
    boot();
    fake_inject(PC, "@1 before\r\n");
    pump(3);
    /* A framing error is flagged while these bytes are being read. */
    fake_inject(PC, "@1 damaged\r\n@1 par");
    hw.framing = 1;
    fake_set_hw(PC, &hw);
    pump(1);
    fake_inject(PC, "tial\r\n@1 next\r\n@1 after\r\n");
    pump(10);
    /* The damaged chunk, the line straddling it, then the rest resumes. */
    CHECK_STR(fake_out(DC(1)), "@1 before\r\n@1 next\r\n@1 after\r\n");
    CHECK(strstr(fake_out(PC), "[MC] E: line received with UART errors, not sent: @1 damaged") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] W: PC link: 1 UART error(s)") != NULL);
    fake_inject(PC, "mc_status\r\n");
    pump(20);
    CHECK(strstr(fake_out(PC), "uart_err_lines=2 ") != NULL);   /* damaged + partial */

    /* A clean line after an error-free poll goes straight through. */
    fake_out_clear(DC(1));
    fake_inject(PC, "@1 clean\r\n");
    pump(3);
    CHECK_STR(fake_out(DC(1)), "@1 clean\r\n");
}

static void test_dc_uart_errors_warned_not_blocked(void)
{
    mc_hw_stats_t hw = { 3, 2, 0, 0, 0, 0 };
    boot();
    fake_set_hw(DC(4), &hw);
    fake_inject(DC(4), "[ESV2-1] still forwarded\r\n");
    pump(10);
    CHECK(strstr(fake_out(PC), "[ESV2-7] still forwarded\r\n") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] W: DC4 DC4: 5 UART error(s)") != NULL);
    /* Rate limited: more errors within the interval give no new warning. */
    fake_out_clear(PC);
    hw.framing = 10;
    fake_set_hw(DC(4), &hw);
    pump(10);
    CHECK(strstr(fake_out(PC), "UART error") == NULL);
    fake_advance(MC_WARN_INTERVAL_MS);
    fake_inject(DC(4), "PZT Temp: 1\r\n");
    pump(10);
    CHECK(strstr(fake_out(PC), "[MC] W: DC4 DC4: 7 UART error(s)") != NULL);
}

static void test_rx_overrun_discards_partial_line(void)
{
    mc_hw_stats_t hw = { 0, 0, 0, 0, 0, 0 };
    boot();
    fake_inject(DC(2), "[ESV2-1] first half of a li");
    pump(2);
    hw.rx_overruns = 1;                 /* main loop stalled: data lost */
    fake_set_hw(DC(2), &hw);
    fake_inject(DC(2), "ne\r\n[ESV2-2] whole\r\n");
    pump(10);
    CHECK(strstr(fake_out(PC), "first half") == NULL);
    CHECK(strstr(fake_out(PC), "[DC-2] ne\r\n") == NULL);   /* tail not a line */
    CHECK(strstr(fake_out(PC), "[ESV2-4] whole\r\n") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] W: DC2 input lost (main loop stalled") != NULL);
}

/* Review finding 1: the CRLF after an abort must precede queued stops. */
static void test_abort_delimiter_precedes_queued_stops(void)
{
    boot();
    fake_tx_mode(DC(1), FAKE_TX_STALL);
    fake_inject(PC, "@1 first\r\n");
    pump(3);
    fake_inject(PC, "@1 stop\r\n@2 cancel\r\n@1 later\r\n");
    pump(3);
    fake_advance(200);
    pump(1);                                    /* abort "first"           */
    fake_tx_mode(DC(1), FAKE_TX_AUTO);
    pump(10);
    /* "@1 later" was sent after the stop, so it rightly follows it. */
    CHECK_STR(fake_out(DC(1)), "\r\n@1 stop\r\n@2 cancel\r\n@1 later\r\n");
}

/* Review finding 2: an error behind buffered lines must still taint the
   damaged line, however many lines and read chunks come first.           */
static void test_error_behind_buffered_lines(void)
{
    mc_hw_stats_t hw = { 0, 0, 0, 0, 0, 0 };
    boot();
    for (int i = 0; i < 40; ++i) fake_inject(PC, "@1 clean\r\n");
    fake_inject(PC, "@1 tart_sweep\r\n");       /* 's' lost to the error   */
    hw.framing = 1;
    fake_set_hw(PC, &hw);
    pump(20);
    CHECK(strstr(fake_out(DC(1)), "tart_sweep") == NULL);
    CHECK(fake_out_len(DC(1)) == 0);            /* all 41 were buffered     */
    /* A command typed later is not affected by the old error. */
    fake_advance(MC_TAINT_GRACE_MS + 50u);
    fake_inject(PC, "@1 after\r\nmc_status\r\n");
    pump(60);
    CHECK_STR(fake_out(DC(1)), "@1 after\r\n");
    CHECK(strstr(fake_out(PC), "uart_err_lines=41 ") != NULL);
}

static void test_error_behind_service_budget(void)
{
    mc_hw_stats_t hw = { 0, 0, 0, 0, 0, 0 };
    char line[40];
    boot();
    /* ~10 KB: more than two 4096-byte service passes, many 256-byte reads. */
    for (int i = 0; i < 700; ++i) {
        snprintf(line, sizeof line, "#1 clean %03d\r\n", i);
        fake_inject(PC, line);
    }
    fake_inject(PC, "@1 damaged\r\n");
    hw.noise = 1;
    fake_set_hw(PC, &hw);
    pump(40);
    CHECK(strstr(fake_out(DC(1)), "damaged") == NULL);
    CHECK(fake_out_len(DC(1)) == 0);            /* everything was buffered  */
    fake_advance(MC_TAINT_GRACE_MS + 50u);
    fake_inject(PC, "@1 fresh\r\n");
    pump(5);
    CHECK_STR(fake_out(DC(1)), "@1 fresh\r\n");
}

static void test_error_before_next_line_starts(void)
{
    mc_hw_stats_t hw = { 0, 0, 0, 0, 0, 0 };
    boot();
    fake_inject(PC, "@1 ok\r\n");
    pump(3);
    /* The error hits the first byte of a line nothing of which has been
       received yet: "@1 get" arrives as "1 get".                         */
    hw.framing = 1;
    fake_set_hw(PC, &hw);
    pump(1);
    fake_inject(PC, "1 get\r\n@1 good\r\n");
    pump(5);
    CHECK_STR(fake_out(DC(1)), "@1 ok\r\n@1 good\r\n");
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(strstr(fake_out(DC(k)), "1 get") == NULL);
}

/* Review finding 3: a DMA restart is a discontinuity on every port. */
static void test_dma_restart_resyncs(void)
{
    mc_hw_stats_t hw = { 0, 0, 0, 0, 0, 0 };
    boot();
    fake_inject(PC, "@1 sta");
    pump(2);
    hw.rx_restarts = 1;
    fake_set_hw(PC, &hw);
    fake_inject(PC, "rt_sweep\r\n");
    pump(5);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(strstr(fake_out(DC(k)), "sweep") == NULL);
    CHECK(strstr(fake_out(PC), "[MC] W: PC input lost (receive DMA restarted)") != NULL);
    fake_inject(PC, "@1 next\r\n");
    pump(5);
    CHECK_STR(fake_out(DC(1)), "@1 next\r\n");

    /* Downstream: partial telemetry is not joined across the restart. */
    memset(&hw, 0, sizeof hw);
    fake_inject(DC(3), "[ESV2-1] par");
    pump(2);
    hw.rx_restarts = 1;
    fake_set_hw(DC(3), &hw);
    fake_inject(DC(3), "tial\r\n[ESV2-2] whole\r\n");
    pump(10);
    CHECK(strstr(fake_out(PC), "tial") == NULL);
    CHECK(strstr(fake_out(PC), "[ESV2-6] whole\r\n") != NULL);
}

static void test_dc_binary_frames_dropped(void)
{
    boot();
    static const char s[] = "[ESV2-1] a\r\n\0\x03\x0a\x0d\x09\0[ESV2-2] b\r\n";
    fake_inject_bytes(DC(2), s, sizeof s - 1u);
    pump(10);
    CHECK_STR(fake_out(PC), "[ESV2-3] a\r\n[ESV2-4] b\r\n");
}

static void test_health_events(void)
{
    fake_reset();
    mc_app_init();
    pump(2);
    fake_inject(DC(1), "PZT Temp: 1\r\n");
    pump(2);
    CHECK(strstr(fake_out(PC), "[MC] I: DC1 online (DC1)") != NULL);
    /* Keep DC1 alive, let the grace period expire for the rest. */
    for (int i = 0; i < 8; ++i) {
        fake_advance(500);
        fake_inject(DC(1), "PZT Temp: 1\r\n");
        pump(2);
    }
    CHECK(count_of(fake_out(PC), "not detected") == 5);
    CHECK(strstr(fake_out(PC), "[MC] W: DC2 not detected on DC2") != NULL);
    CHECK(mc_app_fault_active());
    /* DC1 goes silent. */
    pump(50);                                   /* drain the PC backlog    */
    fake_out_clear(PC);
    fake_advance(MC_DC_SILENT_MS + 1u);
    pump(5);
    CHECK(strstr(fake_out(PC), "[MC] W: DC1 offline: silent for") != NULL);
    fake_inject(DC(1), "PZT Temp: 1\r\n");
    pump(5);
    CHECK(strstr(fake_out(PC), "[MC] I: DC1 online") != NULL);
    /* Transitions are reported once, not repeatedly. */
    pump(20);
    fake_out_clear(PC);
    pump(20);
    CHECK(fake_out_len(PC) == 0);
}

static void test_status_and_local_commands(void)
{
    boot();
    mc_hw_stats_t hw = { 7, 1, 0, 2, 0, 0 };
    fake_set_hw(DC(5), &hw);
    fake_inject(PC, "mc_status\r\n");
    pump(20);
    const char *o = fake_out(PC);
    CHECK(strstr(o, "[MC] status up=") != NULL);
    CHECK(strstr(o, "[MC] PC  PC 921600 baud") != NULL);
    CHECK(strstr(o, "[MC] DC1 DC1 online @1,@2") != NULL);
    CHECK(strstr(o, "[MC] DC6 DC6 online @11,@12") != NULL);
    CHECK(strstr(o, "fe=7 ne=1 overruns=0 dma_restarts=2") != NULL);

    fake_out_clear(PC);
    fake_inject(PC, "mc_reset_stats\r\nmc_status\r\n");
    pump(20);
    CHECK(strstr(fake_out(PC), "fe=7") == NULL);
    fake_inject(PC, "mc_ping \r\nmc_version\r\nmc_help\r\n");
    pump(20);
    CHECK(strstr(fake_out(PC), "[MC] pong\r\n") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] MasterController " MC_FW_VERSION " (built") != NULL);
    CHECK(strstr(fake_out(PC), "[MC] @1..@12 <cmd>") != NULL);
    for (unsigned k = 1; k <= MC_NUM_DC; ++k)
        CHECK(fake_out_len(DC(k)) == 0);        /* local commands stay local */

    fake_out_clear(PC);
    mc_app_request_status();
    pump(3);
    CHECK(strstr(fake_out(PC), "[MC] status up=") != NULL);
}

static void test_tx_stall_recovers(void)
{
    boot();
    fake_tx_mode(DC(2), FAKE_TX_STALL);
    fake_inject(PC, "@3 one\r\n@3 two\r\n");
    pump(5);
    CHECK(fake_tx_starts(DC(2)) == 1);
    fake_advance(MC_TX_STALL_MARGIN_MS + 10u);
    pump(1);
    CHECK(fake_aborts(DC(2)) == 1);
    fake_tx_mode(DC(2), FAKE_TX_AUTO);
    pump(5);
    /* A CRLF terminates whatever part of "one" escaped before the abort. */
    CHECK_STR(fake_out(DC(2)), "\r\n@1 two\r\n");
    fake_inject(PC, "mc_status\r\n");
    pump(20);
    CHECK(strstr(fake_out(PC), "stalls=1") != NULL);
}

static void test_pc_congestion_keeps_mc_replies(void)
{
    boot();
    fake_tx_mode(PC, FAKE_TX_HOLD);
    char line[48];
    for (int i = 0; i < (int)MC_PC_TXQ_SLOTS + 50; ++i) {
        snprintf(line, sizeof line, "[ESV2-1] data %d\r\n", i);
        fake_inject(DC(1), line);
        pump(1);
    }
    fake_inject(PC, "mc_ping\r\n");
    pump(2);
    int sent = 0;
    while (fake_complete(PC)) { pump(1); ++sent; }
    fake_tx_mode(PC, FAKE_TX_AUTO);
    fake_advance(MC_WARN_INTERVAL_MS);
    pump(20);
    const char *o = fake_out(PC);
    CHECK(strstr(o, "[ESV2-1] data 0\r\n") == o);
    CHECK(strstr(o, "[MC] pong\r\n") != NULL);
    CHECK(strstr(o, "[MC] W: PC link congested") != NULL);
    /* Everything admitted before the reserve was reached arrived, in order. */
    CHECK(count_of(o, "[ESV2-1] data ") == (int)(MC_PC_TXQ_SLOTS - MC_TXQ_RESERVE));
    CHECK(all_lines(o, no_nul) > 0);            /* everything is whole lines */
    (void)sent;
    fake_out_clear(PC);
    fake_inject(PC, "mc_status\r\n");
    pump(20);
    char expect[32];
    snprintf(expect, sizeof expect, "up_drop=%d ",
             (int)MC_PC_TXQ_SLOTS + 50 - (int)(MC_PC_TXQ_SLOTS - MC_TXQ_RESERVE));
    CHECK(strstr(fake_out(PC), expect) != NULL);
}

static void test_upload_burst_in_order(void)
{
    boot();
    fake_tx_mode(DC(1), FAKE_TX_HOLD);
    char line[48];
    const int lines = 80;
    for (int i = 0; i < lines; ++i) {
        snprintf(line, sizeof line, "@2 seq_line %d,abc,def\r\n", i);
        fake_inject(PC, line);                  /* one burst, 921600 style */
    }
    pump(3);
    while (fake_complete(DC(1))) pump(1);
    pump(2);
    const char *o = fake_out(DC(1));
    CHECK(count_of(o, "@2 seq_line ") == lines);
    char expect[48];
    const char *p = o;
    int in_order = 1;
    for (int i = 0; i < lines && p; ++i) {
        snprintf(expect, sizeof expect, "@2 seq_line %d,abc,def\r\n", i);
        if (strncmp(p, expect, strlen(expect)) != 0) { in_order = 0; break; }
        p += strlen(expect);
    }
    CHECK(in_order);
    CHECK(strstr(fake_out(PC), "busy") == NULL);
}

int mc_test_failures, mc_test_checks;

int main(void)
{
    RUN(test_boot_banner);
    RUN(test_board_routing);
    RUN(test_dc_raw_and_broadcast);
    RUN(test_invalid_rejected);
    RUN(test_upstream_retag);
    RUN(test_upstream_never_interleaves);
    RUN(test_broadcast_all_or_nothing);
    RUN(test_stop_overtakes_and_purges_same_board);
    RUN(test_broadcast_stop_purges_everything);
    RUN(test_later_stop_never_discards_earlier_stop);
    RUN(test_stop_fits_even_when_queue_full);
    RUN(test_pc_binary_and_overlong_never_forwarded);
    RUN(test_pc_garbage_never_forwarded);
    RUN(test_pc_uart_errors_discard_affected_lines);
    RUN(test_dc_uart_errors_warned_not_blocked);
    RUN(test_rx_overrun_discards_partial_line);
    RUN(test_abort_delimiter_precedes_queued_stops);
    RUN(test_error_behind_buffered_lines);
    RUN(test_error_behind_service_budget);
    RUN(test_error_before_next_line_starts);
    RUN(test_dma_restart_resyncs);
    RUN(test_dc_binary_frames_dropped);
    RUN(test_health_events);
    RUN(test_status_and_local_commands);
    RUN(test_tx_stall_recovers);
    RUN(test_pc_congestion_keeps_mc_replies);
    RUN(test_upload_burst_in_order);
    printf("\n%d checks, %d failures\n", mc_test_checks, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
