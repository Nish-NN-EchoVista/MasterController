/*
 * mc_platform.h - Hardware interface used by the application core.
 *
 * Implemented for the NUCLEO-H7A3ZI-Q in mc_platform.c, and by a fake in
 * tests/host so the complete router can be exercised on a PC.
 *
 * Contract:
 *  - All functions are called from the main loop only.
 *  - mc_plat_rx_read() returns bytes received since the previous call, in
 *    order, never more than max. Reception never stops on line errors.
 *  - mc_plat_tx_start() begins sending exactly len bytes from data. The
 *    buffer must stay untouched until mc_plat_tx_busy() reports 0.
 */
#ifndef MC_PLATFORM_H
#define MC_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t framing;      /* FE: bad stop bit (baud mismatch, noise, unplugged) */
    uint32_t noise;        /* NE                                                 */
    uint32_t parity;       /* PE (should stay 0: no parity is configured)        */
    uint32_t rx_restarts;  /* receive DMA found stopped and restarted            */
    uint32_t tx_aborts;    /* stalled transmissions aborted                      */
} mc_hw_stats_t;

uint32_t    mc_plat_now_ms(void);
int         mc_plat_port_present(unsigned port);
const char *mc_plat_port_name(unsigned port);     /* e.g. "USART10 PE3/PE2"   */
uint32_t    mc_plat_port_baud(unsigned port);

size_t      mc_plat_rx_read(unsigned port, uint8_t *dst, size_t max);
int         mc_plat_tx_busy(unsigned port);
int         mc_plat_tx_start(unsigned port, const uint8_t *data, uint16_t len);
void        mc_plat_tx_abort(unsigned port);
void        mc_plat_hw_stats(unsigned port, mc_hw_stats_t *out);

const char *mc_plat_reset_cause(void);

#endif /* MC_PLATFORM_H */
