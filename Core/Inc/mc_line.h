/*
 * mc_line.h - Bounded line assembler with binary-frame rejection.
 *
 * Text lines end at CR or LF; the CRLF pair therefore yields one line plus
 * an ignored empty line. Empty lines are never reported.
 *
 * A line longer than MC_LINE_MAX is discarded in full, up to and including
 * its terminator, so its tail can never be executed as a command.
 *
 * A 0x00 byte opens a COBS binary frame (the DataController's experimental
 * protocol). Any partial text line is dropped and the frame is swallowed up
 * to its closing 0x00. v1 of the MasterController is text only.
 */
#ifndef MC_LINE_H
#define MC_LINE_H

#include <stdint.h>
#include "mc_config.h"

typedef enum {
    MC_LINE_NONE = 0,
    MC_LINE_READY,        /* buf/len hold a complete line until the next feed */
    MC_LINE_OVERLONG,     /* a line exceeded MC_LINE_MAX and is being dropped */
    MC_LINE_BINARY        /* a binary frame (or frame-like noise) was dropped */
} mc_line_event_t;

typedef struct {
    char     buf[MC_LINE_MAX + 1u];   /* NUL-terminated when READY          */
    uint16_t len;
    uint16_t bin_len;
    uint8_t  state;
    uint8_t  ready;
} mc_line_t;

void            mc_line_init(mc_line_t *l);
mc_line_event_t mc_line_feed(mc_line_t *l, uint8_t byte);

#endif /* MC_LINE_H */
