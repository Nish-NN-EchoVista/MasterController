/* Fake implementation of mc_platform.h for host tests. */
#ifndef FAKE_PLATFORM_H
#define FAKE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include "mc_platform.h"

enum { FAKE_TX_AUTO = 0, FAKE_TX_HOLD, FAKE_TX_STALL, FAKE_TX_REFUSE };

void        fake_reset(void);
void        fake_advance(uint32_t ms);
void        fake_inject(unsigned port, const char *s);
void        fake_inject_bytes(unsigned port, const void *data, size_t n);
/* FAKE_TX_AUTO: every transmission completes immediately.
   FAKE_TX_HOLD: a transmission completes only on fake_complete().
   FAKE_TX_STALL: never completes; only mc_plat_tx_abort() ends it.
   FAKE_TX_REFUSE: mc_plat_tx_start() fails (driver stuck).               */
void        fake_tx_mode(unsigned port, int mode);
int         fake_complete(unsigned port);          /* 1 if one was pending  */
/* Everything fully transmitted on a port so far (NUL-terminated copy).   */
const char *fake_out(unsigned port);
size_t      fake_out_len(unsigned port);
void        fake_out_clear(unsigned port);
unsigned    fake_tx_starts(unsigned port);
unsigned    fake_aborts(unsigned port);
void        fake_set_hw(unsigned port, const mc_hw_stats_t *s);
/* Recovery hooks. */
unsigned    fake_reinits(unsigned port);
/* What a reinit does to the port: heal (switch TX to AUTO) or not.       */
void        fake_reinit_heals(unsigned port, int heals);
/* Make the reinit call itself report failure. */
void        fake_reinit_fails(unsigned port, int fails);
int         fake_enabled(unsigned port);
unsigned    fake_system_resets(void);
void        fake_set_reset_cause(const char *cause);

#endif
