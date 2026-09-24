/*
 * mc_router.h - Pure routing rules (no I/O, no state).
 *
 * PC -> DataControllers (one line in, zero or more lines out)
 *   "@n <cmd>"   n = 1..12   EVS2 n. Sent as "@1 <cmd>" or "@2 <cmd>" to
 *                            DataController ceil(n/2).
 *   "#k <cmd>"   k = 1..6    "<cmd>" sent verbatim to DataController k
 *                            (DC-level commands such as enable_pump, lockout++).
 *   "mc_..."                 handled by the MasterController itself.
 *   anything else            broadcast verbatim to all six DataControllers,
 *                            exactly as a single DataController would receive
 *                            it today (including "@0 ..." pump commands).
 *
 * "stop", "cancel" and "@0 stop_all" are urgent: they overtake queued lines,
 * and queued lines for the same boards are discarded first.
 *
 * DataController k -> PC
 *   "[ESV2-1] x"      -> "[ESV2-(2k-1)] x"      "[ESV2-2] x"      -> "[ESV2-2k] x"
 *   "TX to ESV2-1: x" -> "TX to ESV2-(2k-1): x" "TX to ESV2-2: x" -> "TX to ESV2-2k: x"
 *   "Board1 Temp: x"  -> "Board(2k-1) Temp: x"  "Board2 Temp: x"  -> "Board2k Temp: x"
 *   anything else     -> "[DC-k] x"
 */
#ifndef MC_ROUTER_H
#define MC_ROUTER_H

#include <stdint.h>
#include <stddef.h>

/* Queue tags: which EVS2 boards behind a DataController a line may affect. */
#define MC_TAG_BOARD1   0x01u
#define MC_TAG_BOARD2   0x02u
#define MC_TAG_BOTH     (MC_TAG_BOARD1 | MC_TAG_BOARD2)

typedef enum {
    MC_ROUTE_BROADCAST = 0,   /* payload to every DataController              */
    MC_ROUTE_BOARD,           /* prefix + payload to DataController dc         */
    MC_ROUTE_DC,              /* payload to DataController dc                  */
    MC_ROUTE_LOCAL,           /* MasterController command (payload = line)     */
    MC_ROUTE_INVALID          /* rejected; error describes why                 */
} mc_route_kind_t;

typedef struct {
    mc_route_kind_t kind;
    uint8_t     dc;           /* 1..MC_NUM_DC for BOARD and DC routes           */
    uint8_t     board;        /* 1..MC_NUM_BOARDS for BOARD routes              */
    uint8_t     tag;          /* MC_TAG_* of the boards affected                */
    uint8_t     urgent;
    const char *prefix;       /* "" or "@1 " / "@2 "                            */
    const char *payload;      /* points into the caller's line                  */
    uint16_t    payload_len;
    const char *error;        /* INVALID only                                   */
} mc_route_t;

void   mc_route_pc_line(const char *line, uint16_t len, mc_route_t *r);

/* 1 if the command (surrounding blanks ignored) is stop, cancel or @0 stop_all. */
int    mc_is_urgent(const char *cmd, size_t len);

/* Rewrite one DataController line for the PC and append CRLF.
   Returns the output length, or 0 if it does not fit in cap.               */
size_t mc_retag_dc_line(unsigned dc, const char *line, size_t len,
                        char *out, size_t cap);

#endif /* MC_ROUTER_H */
