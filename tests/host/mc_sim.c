/*
 * mc_sim.c - Run the real MasterController core on a PC.
 *
 * The PC link is a TCP socket (default port 5555); open it with any tool
 * that speaks raw TCP, or with pyserial as "socket://localhost:5555".
 * Every DataController port is looped back, TX to RX, exactly like the
 * jumpers described in tools/loopback_test.py. Wire time is emulated at
 * each port's baud rate, so queueing behaves as it does on hardware.
 *
 *   make -C tests/host mc_sim
 *   tests/host/mc_sim [port]
 *   python tools/loopback_test.py socket://localhost:5555
 */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mc_app.h"
#include "mc_config.h"
#include "mc_platform.h"

#define RING 65536u

typedef struct {
    uint8_t  rx[RING];
    size_t   rx_head, rx_tail;
    int      busy;
    uint64_t done_us;
    const uint8_t *data;
    uint16_t len;
} sport_t;

static sport_t S[MC_NUM_PORTS];
static sock_t  client = INVALID_SOCKET;
static uint64_t pc_rx_credit_us;           /* paces PC input at 921600 */
static uint8_t  pc_in[RING];
static size_t   pc_in_head, pc_in_tail;

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000 / f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

static uint64_t wire_us(unsigned port, size_t bytes)
{
    return (uint64_t)bytes * 10u * 1000000u / mc_plat_port_baud(port);
}

/* ------------------------------------------------------ platform API */

uint32_t mc_plat_now_ms(void) { return (uint32_t)(now_us() / 1000u); }
int mc_plat_port_present(unsigned port) { return port < MC_NUM_PORTS && port != MC_PORT_DBG; }

const char *mc_plat_port_name(unsigned port)
{
    static const char *n[MC_NUM_PORTS] = { "SIM-PC", "SIM-DC1", "SIM-DC2", "SIM-DC3",
                                           "SIM-DC4", "SIM-DC5", "SIM-DC6", "SIM-DBG" };
    return port < MC_NUM_PORTS ? n[port] : "?";
}

uint32_t mc_plat_port_baud(unsigned port)
{
    return port == MC_PORT_PC ? MC_PC_BAUD : port == MC_PORT_DBG ? MC_DBG_BAUD : MC_DC_BAUD;
}

size_t mc_plat_rx_read(unsigned port, uint8_t *dst, size_t max)
{
    sport_t *p = &S[port];
    size_t n = 0;
    while (n < max && p->rx_tail != p->rx_head)
        dst[n++] = p->rx[p->rx_tail++ % RING];
    return n;
}

size_t mc_plat_rx_pending(unsigned port) { return S[port].rx_head - S[port].rx_tail; }

int mc_plat_tx_busy(unsigned port) { return S[port].busy; }

int mc_plat_tx_start(unsigned port, const uint8_t *data, uint16_t len)
{
    sport_t *p = &S[port];
    if (p->busy) return 0;
    p->busy = 1;
    p->data = data;
    p->len = len;
    p->done_us = now_us() + wire_us(port, len);
    return 1;
}

void mc_plat_tx_abort(unsigned port) { S[port].busy = 0; }

void mc_plat_hw_stats(unsigned port, mc_hw_stats_t *out)
{
    (void)port;
    memset(out, 0, sizeof *out);
}

const char *mc_plat_reset_cause(void) { return "SIMULATOR"; }

/* ------------------------------------------------------------ plumbing */

static void complete_tx(void)
{
    uint64_t t = now_us();
    for (unsigned i = 0; i < MC_NUM_PORTS; ++i) {
        sport_t *p = &S[i];
        if (!p->busy || t < p->done_us) continue;
        if (i == MC_PORT_PC) {
            if (client != INVALID_SOCKET)
                send(client, (const char *)p->data, p->len, 0);
        } else {
            for (uint16_t b = 0; b < p->len; ++b)       /* loopback jumper */
                p->rx[p->rx_head++ % RING] = p->data[b];
        }
        p->busy = 0;
    }
}

static void pump_pc_input(void)
{
    if (client != INVALID_SOCKET) {
        char buf[4096];
        int n = (int)recv(client, buf, sizeof buf, 0);
        if (n > 0) {
            for (int i = 0; i < n; ++i) pc_in[pc_in_head++ % RING] = (uint8_t)buf[i];
        } else if (n == 0) {
            CLOSESOCK(client);
            client = INVALID_SOCKET;
            printf("client disconnected\n");
        }
    }
    /* Release bytes to the "UART" no faster than 921600 baud would. */
    uint64_t t = now_us();
    if (!pc_rx_credit_us) pc_rx_credit_us = t;
    while (pc_in_tail != pc_in_head && pc_rx_credit_us <= t) {
        S[MC_PORT_PC].rx[S[MC_PORT_PC].rx_head++ % RING] = pc_in[pc_in_tail++ % RING];
        pc_rx_credit_us += wire_us(MC_PORT_PC, 1);
    }
    if (pc_in_tail == pc_in_head) pc_rx_credit_us = t;
}

static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 5555;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
        fprintf(stderr, "cannot listen on port %d\n", port);
        return 1;
    }
    set_nonblocking(ls);
    printf("MasterController simulator on localhost:%d (DC ports looped back)\n", port);
    fflush(stdout);

    mc_app_init();
    for (;;) {
        if (client == INVALID_SOCKET) {
            sock_t c = accept(ls, NULL, NULL);
            if (c != INVALID_SOCKET) {
                client = c;
                set_nonblocking(client);
                int nd = 1;
                setsockopt(client, IPPROTO_TCP, 1 /* TCP_NODELAY */, (const char *)&nd, sizeof nd);
                printf("client connected\n");
                fflush(stdout);
                mc_app_init();                   /* fresh boot per session */
            }
        }
        pump_pc_input();
        complete_tx();
        mc_app_poll();
#ifdef _WIN32
        Sleep(0);
#else
        usleep(50);
#endif
    }
}
