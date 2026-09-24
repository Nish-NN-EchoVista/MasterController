/*
 * mc_line.c - Bounded line assembler. See mc_line.h.
 */
#include "mc_line.h"

enum { ST_TEXT = 0, ST_DISCARD, ST_BINARY };

void mc_line_init(mc_line_t *l)
{
    l->len = 0;
    l->bin_len = 0;
    l->state = ST_TEXT;
    l->ready = 0;
    l->buf[0] = '\0';
}

void mc_line_resync(mc_line_t *l)
{
    l->len = 0;
    l->bin_len = 0;
    l->ready = 0;
    l->state = ST_DISCARD;
    l->buf[0] = '\0';
}

static mc_line_event_t open_frame(mc_line_t *l)
{
    l->len = 0;
    l->bin_len = 0;
    l->state = ST_BINARY;
    return MC_LINE_NONE;
}

mc_line_event_t mc_line_feed(mc_line_t *l, uint8_t b)
{
    if (l->ready) {                   /* previous line has been consumed    */
        l->ready = 0;
        l->len = 0;
    }

    switch (l->state) {
    case ST_BINARY:
        if (b == 0u) {                /* closing delimiter                  */
            l->state = ST_TEXT;
            return MC_LINE_BINARY;
        }
        if (++l->bin_len > MC_BIN_FRAME_MAX) {
            /* No closing delimiter: treat as noise, resync on a newline.   */
            l->state = ST_DISCARD;
            return MC_LINE_BINARY;
        }
        return MC_LINE_NONE;

    case ST_DISCARD:
        if (b == 0u)
            return open_frame(l);
        if (b == '\r' || b == '\n') {
            l->state = ST_TEXT;
            return MC_LINE_BOUNDARY;
        }
        return MC_LINE_NONE;

    default: /* ST_TEXT */
        if (b == 0u)
            return open_frame(l);
        if (b == '\r' || b == '\n') {
            if (l->len == 0u)
                return MC_LINE_BOUNDARY;
            l->buf[l->len] = '\0';
            l->ready = 1;
            return MC_LINE_READY;
        }
        if (l->len < MC_LINE_MAX) {
            l->buf[l->len++] = (char)b;
            return MC_LINE_NONE;
        }
        l->len = 0;
        l->state = ST_DISCARD;
        return MC_LINE_OVERLONG;
    }
}
