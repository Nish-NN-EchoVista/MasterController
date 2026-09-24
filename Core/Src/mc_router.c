/*
 * mc_router.c - Pure routing rules. See mc_router.h.
 */
#include "mc_router.h"
#include "mc_config.h"
#include <string.h>
#include <stdio.h>

static int starts_with(const char *s, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    return len >= n && memcmp(s, prefix, n) == 0;
}

int mc_is_urgent(const char *cmd, size_t len)
{
    while (len && (*cmd == ' ' || *cmd == '\t')) { ++cmd; --len; }
    while (len && (cmd[len - 1u] == ' ' || cmd[len - 1u] == '\t')) --len;
    return (len == 4u && memcmp(cmd, "stop", 4) == 0) ||
           (len == 6u && memcmp(cmd, "cancel", 6) == 0) ||
           (len == 11u && memcmp(cmd, "@0 stop_all", 11) == 0);
}

/* Parse "<digits> <payload>" after an address marker. Returns the number, or
   0 if malformed. Leading zeros and empty payloads are rejected.            */
static unsigned parse_address(const char *s, size_t len, size_t *payload_at)
{
    size_t i = 0;
    unsigned n = 0;
    if (!len || s[0] < '1' || s[0] > '9')
        return 0;
    while (i < len && s[i] >= '0' && s[i] <= '9' && i < 3u)
        n = n * 10u + (unsigned)(s[i++] - '0');
    if (i >= len || s[i] != ' ' || i + 1u >= len)
        return 0;
    *payload_at = i + 1u;
    return n;
}

void mc_route_pc_line(const char *line, uint16_t len, mc_route_t *r)
{
    memset(r, 0, sizeof *r);
    r->prefix = "";
    r->payload = line;
    r->payload_len = len;
    r->tag = MC_TAG_BOTH;

    if (starts_with(line, len, "mc_")) {
        r->kind = MC_ROUTE_LOCAL;
        return;
    }

    if (len && line[0] == '@' && !starts_with(line, len, "@0 ")) {
        size_t at = 0;
        unsigned n = parse_address(line + 1, len - 1u, &at);
        if (n < 1u || n > MC_NUM_BOARDS) {
            r->kind = MC_ROUTE_INVALID;
            r->error = "invalid EVS2 address (use @1..@12 <command>)";
            return;
        }
        r->kind = MC_ROUTE_BOARD;
        r->board = (uint8_t)n;
        r->dc = (uint8_t)((n + 1u) / 2u);
        r->tag = (n & 1u) ? MC_TAG_BOARD1 : MC_TAG_BOARD2;
        r->prefix = (n & 1u) ? "@1 " : "@2 ";
        r->payload = line + 1 + at;
        r->payload_len = (uint16_t)(len - 1u - at);
        r->urgent = (uint8_t)mc_is_urgent(r->payload, r->payload_len);
        return;
    }

    if (len && line[0] == '#') {
        size_t at = 0;
        unsigned k = parse_address(line + 1, len - 1u, &at);
        if (k < 1u || k > MC_NUM_DC) {
            r->kind = MC_ROUTE_INVALID;
            r->error = "invalid DataController address (use #1..#6 <command>)";
            return;
        }
        r->kind = MC_ROUTE_DC;
        r->dc = (uint8_t)k;
        r->payload = line + 1 + at;
        r->payload_len = (uint16_t)(len - 1u - at);
        r->urgent = (uint8_t)mc_is_urgent(r->payload, r->payload_len);
        return;
    }

    r->kind = MC_ROUTE_BROADCAST;
    r->urgent = (uint8_t)mc_is_urgent(line, len);
}

/* Append helpers bounded by cap. */
typedef struct { char *p; size_t n, cap; int overflow; } out_t;

static void put(out_t *o, const char *s, size_t len)
{
    if (o->overflow || o->n + len > o->cap) { o->overflow = 1; return; }
    memcpy(o->p + o->n, s, len);
    o->n += len;
}

static void put_str(out_t *o, const char *s) { put(o, s, strlen(s)); }

static void put_uint(out_t *o, unsigned v)
{
    char tmp[12];
    int n = snprintf(tmp, sizeof tmp, "%u", v);
    if (n > 0) put(o, tmp, (size_t)n);
}

size_t mc_retag_dc_line(unsigned dc, const char *line, size_t len,
                        char *out, size_t cap)
{
    /* Each rule: original prefix, text before the number, text after it,
       and which of the DataController's two boards it names.              */
    static const struct { const char *match, *head, *tail; unsigned board; } rules[] = {
        { "[ESV2-1]",       "[ESV2-",      "]",       1u },
        { "[ESV2-2]",       "[ESV2-",      "]",       2u },
        { "TX to ESV2-1:",  "TX to ESV2-", ":",       1u },
        { "TX to ESV2-2:",  "TX to ESV2-", ":",       2u },
        { "Board1 Temp:",   "Board",       " Temp:",  1u },
        { "Board2 Temp:",   "Board",       " Temp:",  2u },
    };
    out_t o = { out, 0, cap, 0 };

    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; ++i) {
        size_t m = strlen(rules[i].match);
        if (starts_with(line, len, rules[i].match)) {
            put_str(&o, rules[i].head);
            put_uint(&o, (dc - 1u) * MC_BOARDS_PER_DC + rules[i].board);
            put_str(&o, rules[i].tail);
            put(&o, line + m, len - m);
            put(&o, "\r\n", 2);
            return o.overflow ? 0 : o.n;
        }
    }

    put_str(&o, "[DC-");
    put_uint(&o, dc);
    put_str(&o, "] ");
    put(&o, line, len);
    put(&o, "\r\n", 2);
    return o.overflow ? 0 : o.n;
}
