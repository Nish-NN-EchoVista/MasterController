/* Unit tests for the pure modules: mc_txq, mc_line, mc_router. */
#include "mc_test.h"
#include "mc_txq.h"
#include "mc_line.h"
#include "mc_router.h"

/* ------------------------------------------------------------------ txq */
#define QCAP 4u
static mc_slot_t q_slots[QCAP];
static uint16_t  q_order[QCAP], q_free[QCAP];
static mc_txq_t  q;

static void q_reset(void) { mc_txq_init(&q, q_slots, q_order, q_free, QCAP); }

static int q_push(const char *s, uint8_t tag, int urgent)
{
    return mc_txq_push(&q, (const uint8_t *)s, (uint16_t)strlen(s), tag, urgent);
}

/* Drain the queue into a comma-separated string. */
static const char *q_drain(void)
{
    static char out[256];
    out[0] = '\0';
    const mc_slot_t *s;
    while ((s = mc_txq_head(&q)) != NULL) {
        if (out[0]) strcat(out, ",");
        strncat(out, (const char *)s->data, s->len);
        mc_txq_pop(&q);
    }
    return out;
}

static void test_txq_fifo_and_capacity(void)
{
    q_reset();
    CHECK(mc_txq_free(&q) == QCAP);
    CHECK(q_push("a", 1, 0) && q_push("b", 1, 0) && q_push("c", 1, 0) && q_push("d", 1, 0));
    CHECK(!q_push("e", 1, 0));                     /* full: rejected whole */
    CHECK(mc_txq_count(&q) == QCAP && mc_txq_free(&q) == 0);
    CHECK_STR(q_drain(), "a,b,c,d");
    CHECK(mc_txq_free(&q) == QCAP);
    /* Wraps around the ring correctly. */
    for (int round = 0; round < 5; ++round) {
        CHECK(q_push("x", 1, 0) && q_push("y", 1, 0) && q_push("z", 1, 0));
        CHECK_STR(q_drain(), "x,y,z");
    }
}

static void test_txq_rejects_bad_lengths(void)
{
    static uint8_t big[MC_SLOT_BYTES + 1];
    q_reset();
    CHECK(!mc_txq_push(&q, big, 0, 1, 0));
    CHECK(!mc_txq_push(&q, big, MC_SLOT_BYTES + 1u, 1, 0));
    CHECK(mc_txq_push(&q, big, MC_SLOT_BYTES, 1, 0));
}

static void test_txq_urgent_unlocked_goes_first(void)
{
    q_reset();
    q_push("a", 1, 0); q_push("b", 1, 0);
    CHECK(q_push("STOP", 1, 1));
    CHECK_STR(q_drain(), "STOP,a,b");
}

static void test_txq_urgent_never_displaces_locked_head(void)
{
    q_reset();
    q_push("a", 1, 0); q_push("b", 1, 0);
    mc_txq_lock_head(&q);                          /* "a" is on the wire   */
    CHECK(q_push("STOP", 1, 1));
    CHECK(mc_txq_head_locked(&q));
    CHECK_STR((const char *)mc_txq_head(&q)->data, "a");
    CHECK_STR(q_drain(), "a,STOP,b");
    /* Urgent on an empty queue simply becomes the only entry. */
    CHECK(q_push("STOP", 1, 1));
    CHECK_STR(q_drain(), "STOP");
}

static void test_txq_urgent_runs_keep_order_and_survive_purge(void)
{
    q_reset();
    q_push("a", MC_TAG_BOTH, 0);
    mc_txq_lock_head(&q);
    q_push("b", MC_TAG_BOTH, 0);
    CHECK(q_push("U1", MC_TAG_BOTH, 1));
    CHECK(mc_txq_purge(&q, MC_TAG_BOTH) == 1);     /* only "b"             */
    CHECK(q_push("U2", MC_TAG_BOTH, 1));
    CHECK(mc_txq_purge(&q, MC_TAG_BOTH) == 0);     /* urgent is immune     */
    CHECK_STR(q_drain(), "a,U1,U2");

    /* Unlocked head: urgent run goes first, in arrival order. */
    q_reset();
    q_push("x", 1, 0); q_push("y", 1, 0);
    CHECK(q_push("U1", 1, 1));
    CHECK(q_push("U2", 1, 1));
    CHECK_STR(q_drain(), "U1,U2,x,y");

    /* Works across the ring wrap point. */
    q_reset();
    q_push("p", 1, 0); q_push("q", 1, 0); q_push("r", 1, 0);
    mc_txq_pop(&q); mc_txq_pop(&q);
    mc_txq_lock_head(&q);                          /* "r" at ring index 2  */
    q_push("s", 1, 0);
    CHECK(q_push("U1", 1, 1));
    CHECK(q_push("U2", 1, 1));
    CHECK(!q_push("U3", 1, 1));                    /* full at cap 4        */
    CHECK_STR(q_drain(), "r,U1,U2,s");
}

static void test_txq_push_front_precedes_urgent(void)
{
    q_reset();
    q_push("n", 1, 0);
    CHECK(q_push("U1", 1, 1));
    CHECK(q_push("U2", 1, 1));
    CHECK(mc_txq_push_front(&q, (const uint8_t *)"CR", 2, 0));
    CHECK(mc_txq_purge(&q, 0xFF) == 1);            /* only "n"; CR immune  */
    CHECK_STR(q_drain(), "CR,U1,U2");

    q_reset();                                     /* behind a locked head */
    q_push("w", 1, 0);
    mc_txq_lock_head(&q);
    q_push("U1", 1, 1);
    CHECK(mc_txq_push_front(&q, (const uint8_t *)"CR", 2, 0));
    CHECK_STR(q_drain(), "w,CR,U1");
}

static void test_txq_purge_by_mask(void)
{
    q_reset();
    q_push("b1", MC_TAG_BOARD1, 0);
    q_push("b2", MC_TAG_BOARD2, 0);
    q_push("both", MC_TAG_BOTH, 0);
    q_push("b2b", MC_TAG_BOARD2, 0);
    CHECK(mc_txq_purge(&q, MC_TAG_BOARD1) == 2);   /* b1 and both          */
    CHECK(mc_txq_free(&q) == 2);
    CHECK_STR(q_drain(), "b2,b2b");

    /* A locked head survives even when it matches. */
    q_reset();
    q_push("wire", MC_TAG_BOTH, 0);
    q_push("x", MC_TAG_BOTH, 0);
    q_push("y", MC_TAG_BOARD2, 0);
    mc_txq_lock_head(&q);
    CHECK(mc_txq_purge(&q, MC_TAG_BOTH) == 2);
    CHECK(mc_txq_count(&q) == 1 && mc_txq_head_locked(&q));
    CHECK(q_push("STOP", MC_TAG_BOTH, 1));
    CHECK_STR(q_drain(), "wire,STOP");

    /* Purge after wrap-around keeps order. */
    q_reset();
    q_push("p", 1, 0); q_push("q", 1, 0); q_push("r", 1, 0);
    mc_txq_pop(&q); mc_txq_pop(&q);                /* head now mid-ring    */
    q_push("s", 2, 0); q_push("t", 1, 0); q_push("u", 2, 0);
    CHECK(mc_txq_purge(&q, 2) == 2);
    CHECK_STR(q_drain(), "r,t");
    CHECK(mc_txq_free(&q) == QCAP);
}

/* ----------------------------------------------------------------- line */
static mc_line_t ln;

/* Feed a string; return the lines it produced joined by '|', and event codes. */
static const char *feed(const char *s, size_t n, int *overlong, int *binary)
{
    static char out[2048];
    out[0] = '\0';
    for (size_t i = 0; i < n; ++i) {
        mc_line_event_t e = mc_line_feed(&ln, (uint8_t)s[i]);
        if (e == MC_LINE_READY) {
            if (out[0]) strcat(out, "|");
            strcat(out, ln.buf);
        } else if (e == MC_LINE_OVERLONG && overlong) {
            ++*overlong;
        } else if (e == MC_LINE_BINARY && binary) {
            ++*binary;
        }
    }
    return out;
}
#define FEED(s) feed((s), sizeof(s) - 1u, NULL, NULL)

static void test_line_terminators(void)
{
    mc_line_init(&ln);
    CHECK_STR(FEED("a\r\nb\nc\rd\r\n\r\n\n"), "a|b|c|d");
    CHECK_STR(FEED("par"), "");
    CHECK_STR(FEED("tial\r\n"), "partial");
}

static void test_line_overlong_dropped_whole(void)
{
    static char big[MC_LINE_MAX + 50];
    int over = 0;
    mc_line_init(&ln);
    memset(big, 'x', sizeof big);
    memcpy(big + sizeof big - 7, "start\r\n", 7);   /* tail must NOT run    */
    CHECK_STR(feed(big, sizeof big, &over, NULL), "");
    CHECK(over == 1);
    CHECK_STR(FEED("ok\r\n"), "ok");

    /* Exactly MC_LINE_MAX characters is accepted. */
    memset(big, 'y', MC_LINE_MAX);
    big[MC_LINE_MAX] = '\n';
    over = 0;
    const char *r = feed(big, MC_LINE_MAX + 1u, &over, NULL);
    CHECK(over == 0 && strlen(r) == MC_LINE_MAX);
}

static void test_line_binary_frames_swallowed(void)
{
    int bin = 0;
    mc_line_init(&ln);
    /* text, frame containing CR/LF bytes, text */
    static const char s[] = "hello\r\n\0\x05\x0a\x0d\x42\x01\0world\r\n";
    CHECK_STR(feed(s, sizeof s - 1u, NULL, &bin), "hello|world");
    CHECK(bin == 1);

    /* A frame opening mid-line drops the partial line. */
    bin = 0;
    static const char t[] = "hal\0\x02\x03\0f\r\n";
    CHECK_STR(feed(t, sizeof t - 1u, NULL, &bin), "f");
    CHECK(bin == 1);

    /* Back-to-back frames. */
    bin = 0;
    static const char u[] = "\0\x02\x01\0\0\x03\x0a\x01\0end\n";
    CHECK_STR(feed(u, sizeof u - 1u, NULL, &bin), "end");
    CHECK(bin == 2);
}

static void test_line_unterminated_frame_resyncs(void)
{
    static char s[MC_BIN_FRAME_MAX + 40];
    int bin = 0;
    mc_line_init(&ln);
    size_t n = 0;
    s[n++] = 0;
    for (unsigned i = 0; i < MC_BIN_FRAME_MAX + 5u; ++i) s[n++] = 'z';
    memcpy(s + n, "junk\nnext\n", 10); n += 10;
    CHECK_STR(feed(s, n, NULL, &bin), "next");
    CHECK(bin == 1);
}

static void test_line_resync(void)
{
    mc_line_init(&ln);
    CHECK_STR(FEED("@1 sta"), "");
    mc_line_resync(&ln);
    CHECK_STR(FEED("rt_sweep\r\n@1 ok\r\n"), "@1 ok");
    mc_line_resync(&ln);                           /* at a line boundary   */
    CHECK_STR(FEED("\r\nnext\r\n"), "next");
}

/* --------------------------------------------------------------- router */
static mc_route_t rt;
static void route(const char *s) { mc_route_pc_line(s, (uint16_t)strlen(s), &rt); }

static void test_route_boards(void)
{
    static const struct { const char *in; unsigned dc; const char *prefix; uint8_t tag; } cases[] = {
        { "@1 start_sweep",  1, "@1 ", MC_TAG_BOARD1 },
        { "@2 start_sweep",  1, "@2 ", MC_TAG_BOARD2 },
        { "@3 start_sweep",  2, "@1 ", MC_TAG_BOARD1 },
        { "@4 start_sweep",  2, "@2 ", MC_TAG_BOARD2 },
        { "@9 start_sweep",  5, "@1 ", MC_TAG_BOARD1 },
        { "@11 start_sweep", 6, "@1 ", MC_TAG_BOARD1 },
        { "@12 start_sweep", 6, "@2 ", MC_TAG_BOARD2 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        route(cases[i].in);
        CHECK(rt.kind == MC_ROUTE_BOARD);
        CHECK(rt.dc == cases[i].dc);
        CHECK_STR(rt.prefix, cases[i].prefix);
        CHECK(rt.tag == cases[i].tag);
        CHECK_MEM(rt.payload, rt.payload_len, "start_sweep");
        CHECK(!rt.urgent);
    }
    route("@7 get freq extra args");
    CHECK(rt.kind == MC_ROUTE_BOARD && rt.board == 7 && rt.dc == 4);
    CHECK_MEM(rt.payload, rt.payload_len, "get freq extra args");
}

static void test_route_invalid_addresses(void)
{
    static const char *bad[] = {
        "@13 x", "@99 x", "@100 x", "@1234 x", "@01 x", "@1", "@1 ", "@ 1 x",
        "@x", "@", "@1x start", "@0", "@00 x",
        "#0 x", "#7 x", "#1", "#1 ", "#", "#a x",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        route(bad[i]);
        if (rt.kind != MC_ROUTE_INVALID) printf("    input: \"%s\"\n", bad[i]);
        CHECK(rt.kind == MC_ROUTE_INVALID && rt.error != NULL);
    }
}

static void test_route_dc_local_broadcast(void)
{
    route("#3 enable_pump");
    CHECK(rt.kind == MC_ROUTE_DC && rt.dc == 3 && rt.tag == MC_TAG_BOTH);
    CHECK_MEM(rt.payload, rt.payload_len, "enable_pump");

    route("#6 @1 get volt");                        /* DC-level addressing  */
    CHECK(rt.kind == MC_ROUTE_DC && rt.dc == 6);
    CHECK_MEM(rt.payload, rt.payload_len, "@1 get volt");

    route("mc_status");
    CHECK(rt.kind == MC_ROUTE_LOCAL);

    route("start_defog");
    CHECK(rt.kind == MC_ROUTE_BROADCAST && rt.tag == MC_TAG_BOTH && !rt.urgent);
    CHECK_MEM(rt.payload, rt.payload_len, "start_defog");

    route("@0 play_all");                           /* DC-owned: broadcast  */
    CHECK(rt.kind == MC_ROUTE_BROADCAST);
    CHECK_MEM(rt.payload, rt.payload_len, "@0 play_all");
}

static void test_route_urgent(void)
{
    route("stop");          CHECK(rt.kind == MC_ROUTE_BROADCAST && rt.urgent);
    route("cancel");        CHECK(rt.kind == MC_ROUTE_BROADCAST && rt.urgent);
    route("@0 stop_all");   CHECK(rt.kind == MC_ROUTE_BROADCAST && rt.urgent);
    route(" stop ");        CHECK(rt.urgent);
    route("@5 stop");       CHECK(rt.kind == MC_ROUTE_BOARD && rt.urgent && rt.tag == MC_TAG_BOARD1);
    route("@6 cancel");     CHECK(rt.kind == MC_ROUTE_BOARD && rt.urgent && rt.tag == MC_TAG_BOARD2);
    route("#2 stop");       CHECK(rt.kind == MC_ROUTE_DC && rt.urgent && rt.tag == MC_TAG_BOTH);
    route("#2 @0 stop_all");CHECK(rt.kind == MC_ROUTE_DC && rt.urgent);
    route("stopx");         CHECK(!rt.urgent);
    route("stop_sweep");    CHECK(!rt.urgent);
    route("@1 cancel_all"); CHECK(!rt.urgent);
}

static void test_retag(void)
{
    static const struct { unsigned dc; const char *in, *out; } cases[] = {
        { 1, "[ESV2-1] Evaporation Sweep Completed", "[ESV2-1] Evaporation Sweep Completed\r\n" },
        { 1, "[ESV2-2] ok",                          "[ESV2-2] ok\r\n" },
        { 2, "[ESV2-1] ok",                          "[ESV2-3] ok\r\n" },
        { 3, "[ESV2-2] ok",                          "[ESV2-6] ok\r\n" },
        { 5, "[ESV2-1] IL_data 1,2,3",               "[ESV2-9] IL_data 1,2,3\r\n" },
        { 6, "[ESV2-2] x",                           "[ESV2-12] x\r\n" },
        { 6, "[ESV2-1]",                             "[ESV2-11]\r\n" },
        { 4, "TX to ESV2-1: start_sweep",            "TX to ESV2-7: start_sweep\r\n" },
        { 4, "TX to ESV2-2: start_sweep",            "TX to ESV2-8: start_sweep\r\n" },
        { 6, "Board1 Temp: 25.1",                    "Board11 Temp: 25.1\r\n" },
        { 6, "Board2 Temp: 25.1",                    "Board12 Temp: 25.1\r\n" },
        { 3, "PZT Temp: 24.5 C",                     "[DC-3] PZT Temp: 24.5 C\r\n" },
        { 2, "[ESV2-0] upload_ack",                  "[DC-2] [ESV2-0] upload_ack\r\n" },
        { 1, "I: Pump enabled (PB12 gated by sequence)", "[DC-1] I: Pump enabled (PB12 gated by sequence)\r\n" },
        { 2, "[ESV2-10] impossible",                 "[DC-2] [ESV2-10] impossible\r\n" },
        { 2, "[ESV2-1x] y",                          "[DC-2] [ESV2-1x] y\r\n" },
    };
    char out[MC_SLOT_BYTES];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        size_t n = mc_retag_dc_line(cases[i].dc, cases[i].in, strlen(cases[i].in), out, sizeof out);
        CHECK_MEM(out, n, cases[i].out);
    }
}

static void test_retag_bounds(void)
{
    static char line[MC_LINE_MAX];
    char out[MC_SLOT_BYTES];
    memset(line, 'q', sizeof line);
    /* Worst case fits a slot exactly or with room to spare. */
    CHECK(mc_retag_dc_line(6, line, sizeof line, out, sizeof out) == 7u + MC_LINE_MAX + 2u);
    memcpy(line, "[ESV2-2]", 8);
    CHECK(mc_retag_dc_line(6, line, sizeof line, out, sizeof out) == MC_LINE_MAX + 1u + 2u);
    /* Too small an output buffer is refused, not truncated. */
    CHECK(mc_retag_dc_line(1, "hello", 5, out, 8) == 0);
    CHECK(mc_retag_dc_line(1, "hello", 5, out, 14) == 14);
}

int mc_test_failures, mc_test_checks;

int main(void)
{
    RUN(test_txq_fifo_and_capacity);
    RUN(test_txq_rejects_bad_lengths);
    RUN(test_txq_urgent_unlocked_goes_first);
    RUN(test_txq_urgent_never_displaces_locked_head);
    RUN(test_txq_urgent_runs_keep_order_and_survive_purge);
    RUN(test_txq_purge_by_mask);
    RUN(test_line_terminators);
    RUN(test_line_overlong_dropped_whole);
    RUN(test_line_binary_frames_swallowed);
    RUN(test_line_unterminated_frame_resyncs);
    RUN(test_line_resync);
    RUN(test_txq_push_front_precedes_urgent);
    RUN(test_route_boards);
    RUN(test_route_invalid_addresses);
    RUN(test_route_dc_local_broadcast);
    RUN(test_route_urgent);
    RUN(test_retag);
    RUN(test_retag_bounds);
    printf("\n%d checks, %d failures\n", mc_test_checks, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
