/* ==========================================================================
 * soft_bus_main.c — CLI + socket + main loop. All ESC logic lives in
 * esc_core.c, kept separate so it is testable offline (see test_offline.c).
 *
 * Build:  make
 * Grant:  sudo setcap cap_net_raw,cap_net_admin+ep ./soft_bus
 * Run:    ./soft_bus --iface veth_s --n 8 --pdo-size 4
 *
 * Phase 7: fault injection through a control FIFO (default /tmp/soft_bus.ctl,
 * see esc_fault.h for the commands):
 *     ./sbctl.sh mute 100          or    echo "mute 100" > /tmp/soft_bus.ctl
 * The main loop waits with ppoll() on the raw socket AND the FIFO, with a
 * timeout set by the earliest delayed reply (late/reorder injections).
 * ========================================================================== */

#define _GNU_SOURCE          /* ppoll() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
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
#include "esc_fault.h"

#define RX_BUF_SIZE  FAULT_FRAME_MAX
#define CTL_DEFAULT  "/tmp/soft_bus.ctl"

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static esc_t *g_chain = NULL;
static int    g_n = 0;
static esc_fault_bus_t g_fault;   /* ~130 KB: static, not on the stack */

static uint64_t now_raw_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_sigusr1(int sig)
{
    (void)sig;
    /* Kept for L2-05 scripts; same as the FIFO command "reject_al all".
     * (No fprintf here: not async-signal-safe.) */
    for (int i = 0; i < g_n; i++) g_chain[i].force_reject_al = 1;
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int n = 1;
    int pdo_size = 4;
    esc_dc_cfg_t dc_cfg = { 0, 800, 0, 20000 };   /* width, hop_ns, ref ppb, other ppb */
    double dc_report_s = 10.0;
    const char *ctl_path = CTL_DEFAULT;
    int app_seq_offset = -1;
    int sm_wd_react = 1;

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
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            ctl_path = argv[++i];                      /* "none" disables */
        } else if (strcmp(argv[i], "--app-seq") == 0 && i + 1 < argc) {
            app_seq_offset = atoi(argv[++i]);          /* byte offset in TxPDO */
        } else if (strcmp(argv[i], "--no-sm-wd") == 0) {
            sm_wd_react = 0;
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (!ifname || n <= 0) {
        fprintf(stderr, "Usage: %s --iface <veth_s> --n <node_count> [--pdo-size <bytes>]\n"
                "       [--dc off|32|64] [--dc-hop-ns N] [--dc-drift-ppm X] [--dc-other-ppm X]\n"
                "       [--dc-report-s S] [--ctl <fifo>|none] [--app-seq <offset>] [--no-sm-wd]\n",
                argv[0]);
        return 1;
    }
    if (pdo_size < 0 || (unsigned)pdo_size > SII_PD_MAX_BYTES) {
        fprintf(stderr, "--pdo-size must be 0..%u (SM2+SM3 must fit below 0x2000)\n",
                (unsigned)SII_PD_MAX_BYTES);
        return 1;
    }
    if (app_seq_offset >= 0 && app_seq_offset + 2 > pdo_size) {
        fprintf(stderr, "--app-seq %d needs --pdo-size >= %d\n", app_seq_offset, app_seq_offset + 2);
        return 1;
    }

    esc_t *chain = calloc((size_t)n, sizeof(esc_t));
    if (!chain) { perror("calloc"); return 1; }

    for (int i = 0; i < n; i++) {
        esc_init(&chain[i], (uint8_t)i, (uint16_t)pdo_size);
    }
    esc_chain_wire(chain, n);
    esc_dc_setup(chain, n, &dc_cfg);   /* after esc_init: ORs DC bits into 0x0008 */
    for (int i = 0; i < n; i++) chain[i].wd.react = (uint8_t)sm_wd_react;
    esc_fault_init(&g_fault, stdout, app_seq_offset);

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

    /* Control FIFO. Opened O_RDWR so there is always one writer (us): a
     * FIFO with no writer left reports POLLHUP/EOF forever after the first
     * `echo` closes it, which would spin this loop. mode 0666 so a normal
     * user can write when soft_bus runs under sudo. */
    int ctl_fd = -1;
    if (strcmp(ctl_path, "none") != 0) {
        struct stat st;
        if (stat(ctl_path, &st) == 0) {
            if (!S_ISFIFO(st.st_mode)) {
                fprintf(stderr, "%s exists and is not a FIFO\n", ctl_path);
                close(fd); free(chain); return 1;
            }
        } else if (mkfifo(ctl_path, 0666) < 0) {
            perror("mkfifo"); close(fd); free(chain); return 1;
        }
        chmod(ctl_path, 0666);
        ctl_fd = open(ctl_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (ctl_fd < 0) { perror("open ctl fifo"); close(fd); free(chain); return 1; }
        printf("soft_bus: fault control FIFO %s (commands: see esc_fault.h, or 'status')\n", ctl_path);
    }
    printf("soft_bus: SM watchdog reaction %s, app sequence counter %s",
           sm_wd_react ? "ON" : "OFF", app_seq_offset >= 0 ? "at TxPDO byte " : "OFF");
    if (app_seq_offset >= 0) printf("%d", app_seq_offset);
    printf("\n");

    uint8_t buf[RX_BUF_SIZE];
    char ctl_line[256];
    size_t ctl_len = 0;
    int link_down = 0;                 /* [Phase 7.3] ENETDOWN seen, see recvfrom() below */
    printf("soft_bus: DC=%s hop=%lld ns ref_drift=%+lld ppb other=+-%lld ppb\n",
           dc_cfg.width ? (dc_cfg.width == 64 ? "64-bit" : "32-bit") : "off",
           (long long)dc_cfg.hop_ns, (long long)dc_cfg.ref_drift_ppb,
           (long long)dc_cfg.other_drift_ppb);
    printf("soft_bus: listening on %s, Ctrl+C to stop...\n", ifname);
    uint64_t next_report = now_raw_ns() + (uint64_t)(dc_report_s * 1e9);
    int report_due = 0;

    while (!g_stop) {
        struct pollfd pfd[2] = {
            { .fd = fd,     .events = POLLIN },
            { .fd = ctl_fd, .events = POLLIN },
        };
        struct timespec tmo, *ptmo = NULL;
        uint64_t due = esc_fault_next_due(&g_fault);
        if (due) {
            uint64_t now = now_raw_ns();
            uint64_t d = due > now ? due - now : 0;
            tmo.tv_sec = (time_t)(d / 1000000000ull);
            tmo.tv_nsec = (long)(d % 1000000000ull);
            ptmo = &tmo;
        }
        int pr = ppoll(pfd, ctl_fd >= 0 ? 2 : 1, ptmo, NULL);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("ppoll");
            break;
        }

        if (ctl_fd >= 0 && (pfd[1].revents & POLLIN)) {
            char cb[256];
            ssize_t cr;
            while ((cr = read(ctl_fd, cb, sizeof(cb))) > 0) {
                for (ssize_t k = 0; k < cr; k++) {
                    if (cb[k] == '\n' || ctl_len == sizeof(ctl_line) - 1) {
                        ctl_line[ctl_len] = '\0';
                        esc_fault_command(&g_fault, chain, n, ctl_line, now_raw_ns());
                        ctl_len = 0;
                    } else {
                        ctl_line[ctl_len++] = cb[k];
                    }
                }
            }
        }

        if (pfd[0].revents & (POLLIN | POLLERR)) {
            ssize_t r = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
            if (r < 0) {
                /* [Phase 7.3, L5-03/04] `ip link set veth_s down` makes the
                 * socket report ENETDOWN. A real ESC does not die when its
                 * cable is pulled: it just stops seeing frames (and its
                 * watchdog expires). Log the edge, wait, keep going. */
                if (errno == ENETDOWN || errno == ENXIO) {
                    if (!link_down) {
                        link_down = 1;
                        fprintf(stderr, "soft_bus: link down (%s), waiting for it to come back\n", strerror(errno));
                    }
                    struct timespec pause = { 0, 1000000 };      /* 1 ms, no busy loop */
                    nanosleep(&pause, NULL);
                    continue;
                }
                if (errno != EINTR && errno != EAGAIN) { perror("recvfrom"); break; }
            } else if ((size_t)r >= ETH_HDR_LEN + EC_HDR_LEN) {
                if (link_down) {
                    link_down = 0;
                    fprintf(stderr, "soft_bus: link up again, frames arriving\n");
                }
                uint64_t t_rx = now_raw_ns();      /* as close to recvfrom() as possible */
                int np = esc_fault_frame_begin(&g_fault, chain, n, buf, (size_t)r, t_rx);
                esc_dc_frame_begin(chain, n, t_rx);
                if (np > 0) process_frame(chain, np, buf, (size_t)r);
                /* A frame no ESC saw is "no frame" for the SYNC0 ground truth. */
                esc_dc_frame_end(chain, n, np > 0 ? g_fault.f_has_pd : 0);
                esc_fault_frame_end(&g_fault, chain, n, buf, (size_t)r, t_rx);

                if (dc_cfg.width && t_rx >= next_report) {
                    report_due = 1;
                    next_report = t_rx + (uint64_t)(dc_report_s * 1e9);
                }
            }
        }

        const uint8_t *tx;
        uint16_t tx_len;
        uint64_t now = now_raw_ns();
        while (esc_fault_pop_due(&g_fault, now, &tx, &tx_len)) {
            if (sendto(fd, tx, tx_len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0)
                perror("sendto");
        }

        if (report_due) {                        /* printf AFTER sendto */
            esc_dc_report(chain, n, stdout, dc_report_s);
            fflush(stdout);
            report_due = 0;
        }
    }
    esc_dc_report_totals(chain, n, stdout);
    esc_fault_status(&g_fault, chain, n, stdout);
    if (ctl_fd >= 0) close(ctl_fd);

    printf("soft_bus: shutting down, freeing %d node(s).\n", n);
    close(fd);
    free(chain);
    return 0;
}
