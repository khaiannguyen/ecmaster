/* ==========================================================================
 * soft_bus_main.c — CLI + socket + main loop. All ESC logic lives in
 * esc_core.c, kept separate so it is testable offline (see test_offline.c).
 *
 * Build:  make
 * Grant:  sudo setcap cap_net_raw,cap_net_admin+ep ./soft_bus
 * Run:    ./soft_bus --iface veth_s --n 8 --pdo-size 4
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_core.h"
#include "esc_dc.h"

#define RX_BUF_SIZE  2048

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static esc_t *g_chain = NULL;
static int    g_n = 0;

static uint64_t now_raw_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_sigusr1(int sig)
{
    (void)sig;
    for (int i = 0; i < g_n; i++) g_chain[i].force_reject_al = 1;
    fprintf(stderr, "soft_bus: SIGUSR1 received — next AL Control request will be rejected\n");
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int n = 1;
    int pdo_size = 4;
    esc_dc_cfg_t dc_cfg = { 0, 800, 0, 20000 };   /* width, hop_ns, ref ppb, other ppb */
    double dc_report_s = 10.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pdo-size") == 0 && i + 1 < argc) {
            pdo_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dc") == 0 && i + 1 < argc) {
            const char *v = argv[++i];                 /* off | 32 | 64 */
            dc_cfg.width = (strcmp(v, "off") == 0) ? 0 : atoi(v);
        } else if (strcmp(argv[i], "--dc-hop-ns") == 0 && i + 1 < argc) {
            dc_cfg.hop_ns = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--dc-drift-ppm") == 0 && i + 1 < argc) {
            dc_cfg.ref_drift_ppb = (int64_t)(atof(argv[++i]) * 1000.0);
        } else if (strcmp(argv[i], "--dc-other-ppm") == 0 && i + 1 < argc) {
            dc_cfg.other_drift_ppb = (int64_t)(atof(argv[++i]) * 1000.0);
        } else if (strcmp(argv[i], "--dc-report-s") == 0 && i + 1 < argc) {
            dc_report_s = atof(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (!ifname || n <= 0) {
        fprintf(stderr, "Usage: %s --iface <veth_s> --n <node_count> [--pdo-size <bytes>]\n", argv[0]);
        return 1;
    }

    esc_t *chain = calloc((size_t)n, sizeof(esc_t));
    if (!chain) { perror("calloc"); return 1; }

    for (int i = 0; i < n; i++) {
        esc_init(&chain[i], (uint8_t)i, (uint16_t)pdo_size);
    }
    esc_chain_wire(chain, n);
    esc_dc_setup(chain, n, &dc_cfg);   /* after esc_init: ORs DC bits into 0x0008 */

    g_chain = chain;
    g_n = n;

    printf("soft_bus: initialized N=%d node(s), pdo_size=%d byte/node, iface=%s\n",
           n, pdo_size, ifname);
    printf("soft_bus: DL status node[0]=0x%04X, node[%d]=0x%04X (last node: port1 loop closed)\n",
           (unsigned)(chain[0].regs[REG_DL_STATUS] | (chain[0].regs[REG_DL_STATUS + 1] << 8)),
           n - 1,
           (unsigned)(chain[n - 1].regs[REG_DL_STATUS] | (chain[n - 1].regs[REG_DL_STATUS + 1] << 8)));

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ECAT));
    if (fd < 0) { perror("socket"); free(chain); return 1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); close(fd); free(chain); return 1; }
    int ifindex = ifr.ifr_ifindex;

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ECAT);
    sll.sll_ifindex = ifindex;

    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind"); close(fd); free(chain); return 1;
    }

    /* sigaction WITHOUT SA_RESTART: glibc signal() restarts recvfrom(), so
     * SIGINT never broke the blocking read and soft_bus had to be SIGKILLed
     * (losing the final dc_total report). Now recvfrom() returns EINTR and
     * the loop sees g_stop. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGUSR1, on_sigusr1);

    uint8_t buf[RX_BUF_SIZE];
    printf("soft_bus: DC=%s hop=%lld ns ref_drift=%+lld ppb other=+-%lld ppb\n",
           dc_cfg.width ? (dc_cfg.width == 64 ? "64-bit" : "32-bit") : "off",
           (long long)dc_cfg.hop_ns, (long long)dc_cfg.ref_drift_ppb,
           (long long)dc_cfg.other_drift_ppb);
    printf("soft_bus: listening on %s, Ctrl+C to stop...\n", ifname);
    uint64_t next_report = now_raw_ns() + (uint64_t)(dc_report_s * 1e9);

    while (!g_stop) {
        ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        if ((size_t)r < ETH_HDR_LEN + EC_HDR_LEN) continue;

        uint64_t t_rx = now_raw_ns();          /* as close to recvfrom() as possible */
        esc_dc_frame_begin(chain, n, t_rx);
        process_frame(chain, n, buf, (size_t)r);
        esc_dc_frame_end(chain, n, esc_dc_frame_has_pd(buf, (size_t)r));

        if (sendto(fd, buf, (size_t)r, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
            perror("sendto");
        }

        if (dc_cfg.width && t_rx >= next_report) {  /* printf AFTER sendto */
            esc_dc_report(chain, n, stdout, dc_report_s);
            fflush(stdout);
            next_report = t_rx + (uint64_t)(dc_report_s * 1e9);
        }
    }
    esc_dc_report_totals(chain, n, stdout);

    printf("soft_bus: shutting down, freeing %d node(s).\n", n);
    close(fd);
    free(chain);
    return 0;
}
