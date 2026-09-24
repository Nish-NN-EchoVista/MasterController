/* Fake implementation of mc_platform.h for host tests. */
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "mc_platform.h"

enum { FAKE_TX_AUTO = 0, FAKE_TX_HOLD, FAKE_TX_STALL };

void        fake_reset(void);
void        fake_advance(uint32_t ms);
void        fake_inject(unsigned port, const char *s);
void        fake_inject_bytes(unsigned port, const void *data, size_t n);
/* FAKE_TX_AUTO: every transmission completes immediately.
   FAKE_TX_HOLD: a transmission completes only on fake_complete().
   FAKE_TX_STALL: never completes; only mc_plat_tx_abort() ends it.        */
void        fake_tx_mode(unsigned port, int mode);
int         fake_complete(unsigned port);          /* 1 if one was pending  */
/* Everything fully transmitted on a port so far (NUL-terminated copy).   */
const char *fake_out(unsigned port);
size_t      fake_out_len(unsigned port);
void        fake_out_clear(unsigned port);
unsigned    fake_tx_starts(unsigned port);
unsigned    fake_aborts(unsigned port);
void        fake_set_hw(unsigned port, const mc_hw_stats_t *s);

#endif
