/*
 * mc_config.h - MasterController build-time configuration.
 *
 * One place for every tunable. Nothing in here depends on the HAL so the
 * routing core can be compiled and tested on a PC.
 *
 * Topology:
 *   PC (laptop GUI) <-> MasterController <-> 6 x F401 DataController <-> 2 x EVS2 each
 *
 *   Port 0      : PC link          USART2  PA2 TX / PA3 RX   921600 8N1
 *   Port 1..6   : DataController   57600 8N1 (see mc_platform.c for the pin map)
 *   Port 7      : ST-LINK VCP      USART3  diagnostics only, TX only, 115200
 */
#ifndef MC_CONFIG_H
#define MC_CONFIG_H

#define MC_FW_VERSION          "1.0.0"

/* ---- Topology ---------------------------------------------------------- */
#define MC_NUM_DC              6u          /* DataControllers                  */
#define MC_BOARDS_PER_DC       2u          /* EVS2 boards per DataController   */
#define MC_NUM_BOARDS          (MC_NUM_DC * MC_BOARDS_PER_DC)

#define MC_PORT_PC             0u
#define MC_PORT_DC(k)          ((unsigned)(k))   /* k = 1..MC_NUM_DC           */
#define MC_PORT_DBG            7u
#define MC_NUM_PORTS           8u

/* ---- Line limits -------------------------------------------------------- */
/* Longest accepted line, excluding its terminator. The F401 accepts 511
   characters per line and emits at most ~253, so 512 covers both sides.     */
#define MC_LINE_MAX            512u
/* Largest possible rewrite overhead: "[DC-6] " (7) vs "[ESV2-12]" (+1), and
   CRLF. A TX slot must hold the longest line plus that overhead.            */
#define MC_TAG_OVERHEAD        8u
#define MC_SLOT_BYTES          (MC_LINE_MAX + MC_TAG_OVERHEAD + 2u)

/* ---- Queue depths (whole-line slots) ----------------------------------- */
#define MC_PC_TXQ_SLOTS        256u        /* DC -> PC merged stream            */
#define MC_DC_TXQ_SLOTS        96u         /* PC -> each DC                     */
#define MC_DBG_TXQ_SLOTS       32u         /* diagnostics mirror                */
/* Slots held back for stop/cancel (DC queues) and for [MC] replies (PC
   queue), so neither can be locked out by bulk traffic.                     */
#define MC_TXQ_RESERVE         4u

/* ---- Binary (COBS) frames ---------------------------------------------- */
/* v1 is text only. A 0x00 byte opens a binary frame which is discarded up to
   the closing 0x00. A frame longer than this is treated as line noise.      */
#define MC_BIN_FRAME_MAX       160u

/* ---- Timing (ms) ------------------------------------------------------- */
/* Every F401 prints "PZT Temp" every 500 ms, so 3 s of silence means the
   DataController (or its cable) is gone.                                     */
#define MC_DC_SILENT_MS        3000u
#define MC_STARTUP_GRACE_MS    3000u
/* Rate limit for repeated warnings (drops, binary frames, busy).            */
#define MC_WARN_INTERVAL_MS    1000u
/* A transmit that has not completed after its wire time plus this margin is
   aborted and counted as a stall.                                            */
#define MC_TX_STALL_MARGIN_MS  100u

/* ---- Baud rates (must match the .ioc) ---------------------------------- */
#define MC_PC_BAUD             921600u
#define MC_DC_BAUD             57600u
#define MC_DBG_BAUD            115200u

#endif /* MC_CONFIG_H */
