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

#define RX_BUF_SIZE  2048

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int n = 1;
    int pdo_size = 4;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pdo-size") == 0 && i + 1 < argc) {
            pdo_size = atoi(argv[++i]);
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

    signal(SIGINT, on_sigint);

    uint8_t buf[RX_BUF_SIZE];
    printf("soft_bus: listening on %s, Ctrl+C to stop...\n", ifname);

    while (!g_stop) {
        ssize_t r = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        if ((size_t)r < ETH_HDR_LEN + EC_HDR_LEN) continue;

        process_frame(chain, n, buf, (size_t)r);

        if (sendto(fd, buf, (size_t)r, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
            perror("sendto");
        }
    }

    printf("soft_bus: shutting down, freeing %d node(s).\n", n);
    close(fd);
    free(chain);
    return 0;
}
