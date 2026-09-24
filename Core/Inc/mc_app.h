/*
 * mc_app.h - MasterController application: routing, queues, health.
 *
 * Call mc_app_init() once after the platform is up, then mc_app_poll() from
 * the main loop as often as possible. Nothing here blocks.
 */
#ifndef MC_APP_H
#define MC_APP_H

#include <stdint.h>

void     mc_app_init(void);
void     mc_app_poll(void);

/* Status for LEDs and diagnostics. */
uint32_t mc_app_last_activity_ms(void);   /* last line routed either way      */
int      mc_app_fault_active(void);       /* a DC offline or a recent drop    */
void     mc_app_note_loop_time(uint32_t ms);
/* Queue a full mc_status report (e.g. from the user button). */
void     mc_app_request_status(void);

#endif /* MC_APP_H */
