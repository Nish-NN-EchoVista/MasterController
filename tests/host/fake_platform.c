/* Fake implementation of mc_platform.h for host tests. */
#include "fake_platform.h"
#include "mc_config.h"
#include <string.h>

#define RX_CAP  (64u * 1024u)
#define OUT_CAP (512u * 1024u)

typedef struct {
    uint8_t  rx[RX_CAP];
    size_t   rx_head, rx_tail;
    char     out[OUT_CAP + 1u];
    size_t   out_len;
    int      mode, busy;
    const uint8_t *pending;
    uint16_t pending_len;
    unsigned starts, aborts;
    mc_hw_stats_t hw;
} fport_t;

static fport_t fp[MC_NUM_PORTS];
static uint32_t now_ms;

void fake_reset(void)
{
    memset(fp, 0, sizeof fp);
    now_ms = 1000;
}

void fake_advance(uint32_t ms) { now_ms += ms; }

void fake_inject_bytes(unsigned port, const void *data, size_t n)
{
    const uint8_t *b = data;
    for (size_t i = 0; i < n; ++i)
        fp[port].rx[fp[port].rx_head++ % RX_CAP] = b[i];
}

void fake_inject(unsigned port, const char *s) { fake_inject_bytes(port, s, strlen(s)); }

void fake_tx_mode(unsigned port, int mode) { fp[port].mode = mode; }

static void finish(fport_t *p)
{
    if (p->out_len + p->pending_len <= OUT_CAP) {
        memcpy(p->out + p->out_len, p->pending, p->pending_len);
        p->out_len += p->pending_len;
        p->out[p->out_len] = '\0';
    }
    p->busy = 0;
}

int fake_complete(unsigned port)
{
    if (!fp[port].busy) return 0;
    finish(&fp[port]);
    return 1;
}

const char *fake_out(unsigned port)   { return fp[port].out; }
size_t      fake_out_len(unsigned port) { return fp[port].out_len; }
void        fake_out_clear(unsigned port) { fp[port].out_len = 0; fp[port].out[0] = '\0'; }
unsigned    fake_tx_starts(unsigned port) { return fp[port].starts; }
unsigned    fake_aborts(unsigned port)    { return fp[port].aborts; }
void        fake_set_hw(unsigned port, const mc_hw_stats_t *s) { fp[port].hw = *s; }

/* ------------------------------------------------------ platform API */

uint32_t mc_plat_now_ms(void) { return now_ms; }
int      mc_plat_port_present(unsigned port) { return port < MC_NUM_PORTS; }

const char *mc_plat_port_name(unsigned port)
{
    static const char *names[MC_NUM_PORTS] = {
        "PC", "DC1", "DC2", "DC3", "DC4", "DC5", "DC6", "DBG" };
    return port < MC_NUM_PORTS ? names[port] : "?";
}

uint32_t mc_plat_port_baud(unsigned port)
{
    return port == MC_PORT_PC ? MC_PC_BAUD : port == MC_PORT_DBG ? MC_DBG_BAUD : MC_DC_BAUD;
}

size_t mc_plat_rx_read(unsigned port, uint8_t *dst, size_t max)
{
    fport_t *p = &fp[port];
    size_t n = 0;
    while (n < max && p->rx_tail != p->rx_head)
        dst[n++] = p->rx[p->rx_tail++ % RX_CAP];
    return n;
}

size_t mc_plat_rx_pending(unsigned port) { return fp[port].rx_head - fp[port].rx_tail; }

int mc_plat_tx_busy(unsigned port) { return fp[port].busy; }

int mc_plat_tx_start(unsigned port, const uint8_t *data, uint16_t len)
{
    fport_t *p = &fp[port];
    if (p->busy) return 0;
    p->pending = data;
    p->pending_len = len;
    p->busy = 1;
    ++p->starts;
    if (p->mode == FAKE_TX_AUTO)
        finish(p);
    return 1;
}

void mc_plat_tx_abort(unsigned port)
{
    fp[port].busy = 0;
    ++fp[port].aborts;
}

void mc_plat_hw_stats(unsigned port, mc_hw_stats_t *out) { *out = fp[port].hw; }

const char *mc_plat_reset_cause(void) { return "TEST"; }
