/*
 * mc_platform.c - NUCLEO-H7A3ZI-Q implementation of mc_platform.h and
 * mc_board.h.
 *
 * Port map (TX / RX):
 *   0  PC    USART2   PA2  / PA3    921600   external USB-UART adapter
 *   1  DC1   USART10  PE3  / PE2    57600    EVS2 @1,  @2
 *   2  DC2   UART7    PF7  / PF6    57600    EVS2 @3,  @4
 *   3  DC3   UART4    PA0  / PA1    57600    EVS2 @5,  @6
 *   4  DC4   UART5    PB13 / PB12   57600    EVS2 @7,  @8
 *   5  DC5   UART9    PD15 / PD14   57600    EVS2 @9,  @10
 *   6  DC6   USART6   PC6  / PC7    57600    EVS2 @11, @12
 *   7  DBG   USART3   PD8  / PD9    115200   ST-LINK VCP, diagnostics only
 *
 * Receive: each port's RX DMA stream (configured circular by CubeMX) is
 * started directly on the peripheral, never through HAL_UART_Receive_DMA.
 * The HAL UART interrupt handler aborts DMA reception on the first framing
 * or noise error; by not enabling the UART error interrupts, errors are left
 * as flags that the main loop counts and clears while DMA keeps running.
 * The CubeMX setting "DMA disable on RX error" is also switched off here.
 * If a stream is ever found disabled it is restarted and counted.
 *
 * Transmit: HAL_UART_Transmit_IT with the 16-byte FIFO enabled. The
 * completion interrupt only clears a busy flag; queues live in mc_app.c.
 *
 * The data cache is disabled (see the .ioc), so DMA buffers need no cache
 * maintenance.
 */
#include "main.h"
#include "stm32h7xx_nucleo.h"
#include "mc_platform.h"
#include "mc_board.h"
#include "mc_app.h"
#include "mc_config.h"

#include <string.h>

extern UART_HandleTypeDef huart2, huart10, huart7, huart4, huart5, huart9, huart6;
extern UART_HandleTypeDef hcom_uart[COMn];
extern IWDG_HandleTypeDef hiwdg1;

/* Ring sizes (DMA NDTR is 16-bit, so at most 65535). A DC ring holds more
   than the watchdog timeout of input, so it cannot be overrun without the
   board resetting first. The PC ring holds ~533 ms; longer main-loop
   stalls are detected and reported (rx_overruns).                          */
#define PC_RX_BYTES   49152u   /* ~533 ms of continuous input at 921600 */
#define DC_RX_BYTES   8192u    /* ~1.4 s of continuous input at 57600   */
/* Consecutive failed DMA restarts before the board resets itself. */
#define DMA_RESTART_LIMIT 50u

#define FAULT_MAGIC   0xFA170000u
#define FAULT_MASK    0xFFFF0000u

typedef struct {
    UART_HandleTypeDef *h;
    const char         *name;
    uint32_t            baud;
    uint8_t            *rx_buf;        /* NULL for TX-only ports             */
    uint16_t            rx_size;
    uint16_t            rx_tail;
    uint32_t            rx_ring_ms;    /* time to fill the ring at line rate */
    uint32_t            rx_fail_run;   /* consecutive failed restarts        */
    volatile uint8_t    tx_busy;
    mc_hw_stats_t       hw;
} plat_port_t;

static uint8_t rx_pc[PC_RX_BYTES] __attribute__((aligned(32)));
static uint8_t rx_dc[MC_NUM_DC][DC_RX_BYTES] __attribute__((aligned(32)));

static plat_port_t P[MC_NUM_PORTS];
static uint8_t     ready;
static char        reset_cause[24];
static uint32_t    last_poll_ms;

/* ---------------------------------------------------------- reset cause */

static void capture_reset_cause(void)
{
    static const char *faults[] = {
        "", "ERROR_HANDLER", "HARDFAULT", "MEMMANAGE", "BUSFAULT", "USAGEFAULT", "NMI",
        "DMA_FAILURE" };
    const char *why = "UNKNOWN";

    __HAL_RCC_RTC_CLK_ENABLE();                /* TAMP backup register access */
    HAL_PWR_EnableBkUpAccess();
    uint32_t bkp = TAMP->BKP0R;
    TAMP->BKP0R = 0;

    if ((bkp & FAULT_MASK) == FAULT_MAGIC && (bkp & 0xFFFFu) &&
        (bkp & 0xFFFFu) < sizeof faults / sizeof faults[0])
        why = faults[bkp & 0xFFFFu];
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) why = "WATCHDOG";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) why = "WINDOW_WATCHDOG";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWR1RST)) why = "LOW_POWER";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))   why = "POWER_ON";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   why = "BROWNOUT";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   why = "SOFTWARE";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   why = "RESET_PIN";
    __HAL_RCC_CLEAR_RESET_FLAGS();

    strncpy(reset_cause, why, sizeof reset_cause - 1u);
}

/* Always resets. There is deliberately no breakpoint here: DHCSR.C_DEBUGEN
   stays set after an ST-LINK session until power is cycled, so a "halt if
   a debugger is attached" test would hang a board left running after
   flashing. To inspect a fault, set a breakpoint on this function.         */
void mc_board_fatal(mc_fault_t why)
{
    __disable_irq();
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;                        /* clock enable takes effect */
    PWR->CR1 |= PWR_CR1_DBP;
    for (unsigned i = 0; i < 1000u && !(PWR->CR1 & PWR_CR1_DBP); ++i) { }
    TAMP->BKP0R = FAULT_MAGIC | (uint32_t)why;
    __DSB();
    NVIC_SystemReset();
}

/* --------------------------------------------------------------- receive */

static HAL_StatusTypeDef dma_start(plat_port_t *p)
{
    return HAL_DMA_Start(p->h->hdmarx, (uint32_t)&p->h->Instance->RDR,
                         (uint32_t)p->rx_buf, p->rx_size);
}

/* (Re)start reception. Returns 1 on success.
   HAL_DMA_Abort leaves the handle in HAL_DMA_STATE_ERROR if the stream does
   not stop within its timeout, and HAL_DMA_Start refuses anything but
   READY, so a failed start re-initialises the handle from its saved Init
   (circular, byte, DMAMUX request) and tries once more.                    */
static int rx_start(plat_port_t *p)
{
    UART_HandleTypeDef *h = p->h;
    DMA_HandleTypeDef  *d = h->hdmarx;

    CLEAR_BIT(h->Instance->CR3, USART_CR3_DMAR);
    (void)HAL_DMA_Abort(d);                    /* no-op error if not running */
    p->rx_tail = 0;
    __HAL_UART_CLEAR_FLAG(h, UART_CLEAR_OREF | UART_CLEAR_NEF |
                             UART_CLEAR_FEF | UART_CLEAR_PEF);
    if (dma_start(p) != HAL_OK) {
        (void)HAL_DMA_DeInit(d);
        if (HAL_DMA_Init(d) != HAL_OK || dma_start(p) != HAL_OK)
            return 0;
    }
    SET_BIT(h->Instance->CR3, USART_CR3_DMAR);
    return 1;
}

static uint16_t rx_head(const plat_port_t *p)
{
    uint32_t left = __HAL_DMA_GET_COUNTER(p->h->hdmarx);
    uint32_t head = p->rx_size - left;
    return (uint16_t)(head >= p->rx_size ? 0u : head);
}

static int rx_running(const plat_port_t *p)
{
    const DMA_Stream_TypeDef *s = (const DMA_Stream_TypeDef *)p->h->hdmarx->Instance;
    return (s->CR & DMA_SxCR_EN) && (p->h->Instance->CR3 & USART_CR3_DMAR);
}

size_t mc_plat_rx_read(unsigned port, uint8_t *dst, size_t max)
{
    if (port >= MC_NUM_PORTS || !P[port].rx_buf) return 0;
    plat_port_t *p = &P[port];
    uint16_t head = rx_head(p);
    size_t n = 0;
    while (n < max && p->rx_tail != head) {
        dst[n++] = p->rx_buf[p->rx_tail];
        p->rx_tail = (uint16_t)((p->rx_tail + 1u) % p->rx_size);
    }
    return n;
}

size_t mc_plat_rx_pending(unsigned port)
{
    if (port >= MC_NUM_PORTS || !P[port].rx_buf) return 0;
    const plat_port_t *p = &P[port];
    uint16_t head = rx_head(p);
    return (size_t)((head + p->rx_size - p->rx_tail) % p->rx_size);
}

/* Count and clear line-error flags. */
static void poll_flags(plat_port_t *p)
{
    USART_TypeDef *u = p->h->Instance;
    uint32_t isr = u->ISR;
    if (isr & USART_ISR_FE)  { ++p->hw.framing; u->ICR = USART_ICR_FECF; }
    if (isr & USART_ISR_NE)  { ++p->hw.noise;   u->ICR = USART_ICR_NECF; }
    if (isr & USART_ISR_PE)  { ++p->hw.parity;  u->ICR = USART_ICR_PECF; }
    if (isr & USART_ISR_ORE) { u->ICR = USART_ICR_ORECF; }   /* OVRDIS: never */
}

/* Poll line errors; restart reception if it has stopped; flag a main-loop
   stall long enough to have overwritten unread input.                     */
static void service_port(plat_port_t *p, uint32_t gap_ms)
{
    poll_flags(p);
    if (!p->rx_buf)
        return;
    if (gap_ms >= p->rx_ring_ms)
        ++p->hw.rx_overruns;
    if (!rx_running(p)) {
        ++p->hw.rx_restarts;
        if (rx_start(p))
            p->rx_fail_run = 0;
        else if (++p->rx_fail_run >= DMA_RESTART_LIMIT)
            mc_board_fatal(MC_FAULT_DMA);       /* a reset restores all DMA */
    }
}

/* -------------------------------------------------------------- transmit */

int mc_plat_tx_busy(unsigned port)
{
    return port < MC_NUM_PORTS && P[port].h ? P[port].tx_busy : 0;
}

int mc_plat_tx_start(unsigned port, const uint8_t *data, uint16_t len)
{
    if (port >= MC_NUM_PORTS || !P[port].h || P[port].tx_busy) return 0;
    plat_port_t *p = &P[port];
    p->tx_busy = 1;
    if (HAL_UART_Transmit_IT(p->h, data, len) != HAL_OK) {
        p->tx_busy = 0;
        return 0;
    }
    return 1;
}

void mc_plat_tx_abort(unsigned port)
{
    if (port >= MC_NUM_PORTS || !P[port].h) return;
    (void)HAL_UART_AbortTransmit(P[port].h);
    P[port].tx_busy = 0;
    ++P[port].hw.tx_aborts;
}

static plat_port_t *port_of(const UART_HandleTypeDef *h)
{
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i)
        if (P[i].h == h) return &P[i];
    return NULL;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *h)
{
    plat_port_t *p = port_of(h);
    if (p) p->tx_busy = 0;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *h)
{
    /* Only reachable if an error interrupt was enabled elsewhere: count it
       and keep reception running rather than stopping it.                  */
    plat_port_t *p = port_of(h);
    if (p) ++p->hw.framing;
}

/* The ST-LINK VCP is set up by BSP_COM_Init() in polling mode; enable its
   interrupt so diagnostics can be sent without blocking. CubeMX does not
   generate this handler because USART3 belongs to the BSP.                 */
void USART3_IRQHandler(void)
{
    HAL_UART_IRQHandler(&hcom_uart[COM1]);
}

/* ------------------------------------------------------------- platform */

uint32_t mc_plat_now_ms(void) { return HAL_GetTick(); }

int mc_plat_port_present(unsigned port)
{
    return ready && port < MC_NUM_PORTS && P[port].h != NULL;
}

const char *mc_plat_port_name(unsigned port)
{
    return port < MC_NUM_PORTS && P[port].name ? P[port].name : "?";
}

uint32_t mc_plat_port_baud(unsigned port)
{
    return port < MC_NUM_PORTS ? P[port].baud : 0u;
}

/* Samples the error flags now, so a caller that reads bytes first and then
   calls this sees every error that affected those bytes.                  */
void mc_plat_hw_stats(unsigned port, mc_hw_stats_t *out)
{
    if (port < MC_NUM_PORTS && P[port].h) {
        poll_flags(&P[port]);
        *out = P[port].hw;
    } else {
        memset(out, 0, sizeof *out);
    }
}

const char *mc_plat_reset_cause(void) { return reset_cause; }

/* ---------------------------------------------------------------- board */

static void port_setup(unsigned i, UART_HandleTypeDef *h, const char *name,
                       uint8_t *rx, uint16_t rx_size)
{
    plat_port_t *p = &P[i];
    p->h = h;
    p->name = name;
    p->baud = h->Init.BaudRate;
    p->rx_buf = rx;
    p->rx_size = rx_size;
    /* Conservative: 3/4 of the time it takes to fill the ring at 10 bits/byte. */
    p->rx_ring_ms = rx_size ? (uint32_t)((uint64_t)rx_size * 10000u * 3u / 4u / p->baud) : 0u;

    /* FIFO mode: the TX interrupt refills 8 bytes at a time. */
    (void)HAL_UARTEx_SetTxFifoThreshold(h, UART_TXFIFO_THRESHOLD_1_2);
    (void)HAL_UARTEx_EnableFifoMode(h);

    /* Keep DMA running through line errors (the .ioc enables the opposite).
       CR3.DDRE and OVRDIS may only change while the UART is disabled.      */
    __HAL_UART_DISABLE(h);
    CLEAR_BIT(h->Instance->CR3, USART_CR3_DDRE | USART_CR3_EIE);
    SET_BIT(h->Instance->CR3, USART_CR3_OVRDIS);
    __HAL_UART_ENABLE(h);

    if (rx && !rx_start(p))
        mc_board_fatal(MC_FAULT_DMA);
}

void mc_board_init(void)
{
#ifdef DEBUG
    __HAL_DBGMCU_FREEZE_IWDG1();               /* breakpoints must not reset */
#endif
    capture_reset_cause();
    memset(P, 0, sizeof P);

    port_setup(MC_PORT_PC,    &huart2,  "USART2 PA2/PA3",   rx_pc,    PC_RX_BYTES);
    port_setup(MC_PORT_DC(1), &huart10, "USART10 PE3/PE2",  rx_dc[0], DC_RX_BYTES);
    port_setup(MC_PORT_DC(2), &huart7,  "UART7 PF7/PF6",    rx_dc[1], DC_RX_BYTES);
    port_setup(MC_PORT_DC(3), &huart4,  "UART4 PA0/PA1",    rx_dc[2], DC_RX_BYTES);
    port_setup(MC_PORT_DC(4), &huart5,  "UART5 PB13/PB12",  rx_dc[3], DC_RX_BYTES);
    port_setup(MC_PORT_DC(5), &huart9,  "UART9 PD15/PD14",  rx_dc[4], DC_RX_BYTES);
    port_setup(MC_PORT_DC(6), &huart6,  "USART6 PC6/PC7",   rx_dc[5], DC_RX_BYTES);
    port_setup(MC_PORT_DBG,   &hcom_uart[COM1], "USART3 ST-LINK VCP", NULL, 0);
    HAL_NVIC_SetPriority(USART3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    ready = 1;
    last_poll_ms = HAL_GetTick();
    BSP_LED_Off(LED_GREEN);
    BSP_LED_Off(LED_YELLOW);
    BSP_LED_Off(LED_RED);
    mc_app_init();
}

static void service_leds(uint32_t now)
{
    /* Green: 1 Hz heartbeat from the main loop.
       Yellow: lit for 30 ms after any routed line.
       Red: a DataController is offline or something was dropped recently. */
    if ((now / 500u) & 1u) BSP_LED_On(LED_GREEN); else BSP_LED_Off(LED_GREEN);
    if (now - mc_app_last_activity_ms() < 30u) BSP_LED_On(LED_YELLOW);
    else BSP_LED_Off(LED_YELLOW);
    if (mc_app_fault_active()) BSP_LED_On(LED_RED); else BSP_LED_Off(LED_RED);
}

void mc_board_poll(void)
{
    uint32_t start = HAL_GetTick();
    uint32_t gap = start - last_poll_ms;
    last_poll_ms = start;

    for (unsigned i = 0; i < MC_NUM_PORTS; ++i)
        if (P[i].h)
            service_port(&P[i], gap);
    mc_app_poll();
    service_leds(start);

    /* Refreshed only here: if the main loop ever stalls for ~1 s, the
       independent watchdog resets the board and the next boot reports it. */
    (void)HAL_IWDG_Refresh(&hiwdg1);
    mc_app_note_loop_time(HAL_GetTick() - start);
}

/* User button: print a full status report on the PC link and the VCP. */
void BSP_PB_Callback(Button_TypeDef button)
{
    if (button == BUTTON_USER)
        mc_app_request_status();
}
