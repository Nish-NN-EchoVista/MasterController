/*
 * mc_board.h - Glue between the CubeMX-generated main.c and the application.
 *
 * main.c calls (inside USER CODE blocks, so regeneration keeps them):
 *   mc_board_init()   once, after every MX_..._Init() and BSP init
 *   mc_board_poll()   forever, from the main loop
 *   mc_board_fatal()  from Error_Handler() and the fault handlers
 */
#ifndef MC_BOARD_H
#define MC_BOARD_H

#include <stdint.h>

/* Codes recorded in a backup register across a deliberate reset. */
typedef enum {
    MC_FAULT_NONE = 0,
    MC_FAULT_ERROR_HANDLER,
    MC_FAULT_HARDFAULT,
    MC_FAULT_MEMMANAGE,
    MC_FAULT_BUSFAULT,
    MC_FAULT_USAGEFAULT,
    MC_FAULT_NMI,
    MC_FAULT_DMA,          /* a receive DMA stream could not be restarted */
} mc_fault_t;

void mc_board_init(void);
void mc_board_poll(void);
/* Record the reason and reset immediately. Never returns. */
void mc_board_fatal(mc_fault_t why) __attribute__((noreturn));

#endif /* MC_BOARD_H */
