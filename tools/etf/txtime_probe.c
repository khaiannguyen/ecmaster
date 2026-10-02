/*
 * txtime_probe.c -- Phase 8.5 step 6.2 (T-03..T-06): launch-time accuracy of
 * the i226 with and without the ETF qdisc, before touching SOEM.
 *
 * One EtherCAT-typed frame (EtherType 0x88A4, one NOP datagram carrying a
 * sequence number) per period on a raw AF_PACKET socket. Each frame asks to
 * leave at t_target = a period boundary in CLOCK_TAI (+ offset). The hardware
 * TX timestamp comes back on the socket error queue and
 *
 *     launch_err = tx_hw_ts - t_target
 *
 * The PHC must track CLOCK_TAI (phc2sys -s CLOCK_REALTIME -c IFACE -O <tai>,
 * checked by clock_check, T-01) so the hardware timestamp is in TAI.
 *
 * Modes
 *   default   SO_TXTIME (CLOCK_TAI) + SCM_TXTIME per frame; wake at
 *             t_target - lead. With `etf ... offload` the NIC holds the frame
 *             until its PHC reaches txtime (T-04); without offload the qdisc
 *             releases it at txtime - delta (T-03).
 *   -n        plain send(), no SO_TXTIME: wake at t_target and send now.
 *             Baseline of today's af_packet path. With ETF installed every
 *             frame must be dropped (T-06).
 *   -C mono   SO_TXTIME with CLOCK_MONOTONIC: ETF (clockid CLOCK_TAI) must
 *             reject every frame as INVALID_PARAM (T-06).
 *   -X us     txtime = now_tai - us (in the past): ETF must drop every frame
 *             and report it on the error queue (T-05).
 *   -w        software TX timestamps (CLOCK_REALTIME, converted to TAI) instead
 *             of hardware: for veth / CI, where there is no PHC. Measures the
 *             stack, not the wire.
 *
 * Usage
 *   txtime_probe [-i IFACE] [-s SECONDS] [-p PERIOD_US] [-l LEAD_US]
 *                [-x OFFSET_US] [-P SO_PRIORITY] [-n] [-C tai|mono]
 *                [-X PAST_US] [-w] [-c OUT.csv]
 *   defaults: -i enP1p1s0 -s 60 -p 1000 -l 200 -x 0 -P 3
 *
 * Needs CAP_NET_RAW + CAP_NET_ADMIN (SIOCSHWTSTAMP). Run under chrt -f.
 *
 * Build: gcc -O2 -Wall -Wextra -o txtime_probe txtime_probe.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif
#ifndef SO_TXTIME
#define SO_TXTIME 61
#define SCM_TXTIME SO_TXTIME
#endif
#ifndef SO_EE_ORIGIN_TXTIME
#define SO_EE_ORIGIN_TXTIME 6
#define SO_EE_CODE_TXTIME_INVALID_PARAM 1
#define SO_EE_CODE_TXTIME_MISSED 2
#endif

#define ETH_P_ECAT 0x88A4
#define NSEC_PER_SEC ((int64_t)1000000000)
#define RING 8192 /* in-flight bookkeeping, indexed by OPT_ID */

static volatile sig_atomic_t g_stop;
static void on_sigint(int s) { (void)s; g_stop = 1; }

static int64_t ts_ns(const struct timespec *t) { return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec; }
static int64_t now_ns(clockid_t c) { struct timespec t; clock_gettime(c, &t); return ts_ns(&t); }
static void ns_ts(int64_t ns, struct timespec *t) { t->tv_sec = ns / NSEC_PER_SEC; t->tv_nsec = ns % NSEC_PER_SEC; }
static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* TAI - MONO, narrowest of 3 bracketed reads (clock_check rule) */
static int64_t tai_minus_mono(void)
{
    int64_t best_w = INT64_MAX, best = 0;
    for (int i = 0; i < 3; i++) {
        int64_t a = now_ns(CLOCK_TAI), m = now_ns(CLOCK_MONOTONIC), b = now_ns(CLOCK_TAI);
        if (b - a < best_w) { best_w = b - a; best = a + (b - a) / 2 - m; }
    }
    return best;
}

/* ---- per-frame bookkeeping, indexed by the kernel's OPT_ID counter ---- */
struct slot {
    int64_t t_target;  /* TAI ns, what we asked for */
    uint32_t seq;
    uint8_t used;
};
static struct slot g_ring[RING];

struct stats {
    int64_t *launch; size_t n_launch, cap;    /* tx_hw_ts - t_target */
    int64_t *wake;   size_t n_wake;           /* actual wake - planned wake (MONO) */
    int64_t *send;   size_t n_send;           /* duration of sendmsg/send */
    uint64_t sent, send_err, ts_hw, ts_nohw, ts_unmatched;
    uint64_t txtime_missed, txtime_invalid, txtime_other, late_wake;
    int last_send_errno;
};

static void push(int64_t *a, size_t *n, size_t cap, int64_t v) { if (*n < cap) a[(*n)++] = v; }

static void print_dist(const char *name, int64_t *v, size_t n)
{
    if (!n) { printf("  %-28s no samples\n", name); return; }
    qsort(v, n, sizeof(v[0]), cmp_i64);
    printf("  %-28s min %8" PRId64 "  p50 %8" PRId64 "  p99 %8" PRId64 "  p99.9 %8" PRId64
           "  p99.99 %8" PRId64 "  max %8" PRId64 " ns (n=%zu)\n", name, v[0], v[n / 2],
           v[(size_t)(n * 0.99)], v[(size_t)(n * 0.999)], v[(size_t)(n * 0.9999)], v[n - 1], n);
}

/* Drain the error queue: hardware TX timestamps and ETF drop reports. */
static int g_sw_ts;
static int64_t g_tai_minus_real;

static void drain_errqueue(int fd, struct stats *st, FILE *csv)
{
    for (;;) {
        char ctrl[512], data[256];
        struct iovec iov = { data, sizeof(data) };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                              .msg_control = ctrl, .msg_controllen = sizeof(ctrl) };
        ssize_t r = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (r < 0)
            return; /* EAGAIN: empty */

        struct scm_timestamping *tss = NULL;
        struct sock_extended_err *ee = NULL;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING)
                tss = (struct scm_timestamping *)CMSG_DATA(c);
            else if (c->cmsg_level == SOL_PACKET && c->cmsg_type == PACKET_TX_TIMESTAMP)
                ee = (struct sock_extended_err *)CMSG_DATA(c);
        }
        if (!ee)
            continue;

        if (ee->ee_origin == SO_EE_ORIGIN_TXTIME) {
            if (ee->ee_code == SO_EE_CODE_TXTIME_MISSED) st->txtime_missed++;
            else if (ee->ee_code == SO_EE_CODE_TXTIME_INVALID_PARAM) st->txtime_invalid++;
            else st->txtime_other++;
            if (csv) {
                uint64_t tx = ((uint64_t)ee->ee_data << 32) | ee->ee_info;
                fprintf(csv, "drop,%u,%" PRIu64 ",,\n", ee->ee_code, tx);
            }
            continue;
        }
        if (ee->ee_origin != SO_EE_ORIGIN_TIMESTAMPING)
            continue;

        struct slot *s = &g_ring[ee->ee_data % RING];
        if (!s->used) { st->ts_unmatched++; continue; }
        s->used = 0;
        int k = g_sw_ts ? 0 : 2;
        if (!tss || (tss->ts[k].tv_sec == 0 && tss->ts[k].tv_nsec == 0)) { st->ts_nohw++; continue; }
        int64_t hw = ts_ns(&tss->ts[k]) + (g_sw_ts ? g_tai_minus_real : 0);
        int64_t err = hw - s->t_target;
        st->ts_hw++;
        push(st->launch, &st->n_launch, st->cap, err);
        if (csv)
            fprintf(csv, "ts,%u,%" PRId64 ",%" PRId64 ",%" PRId64 "\n", s->seq, s->t_target, hw, err);
    }
}

static int send_at(int fd, const void *buf, size_t len, int use_txtime, uint64_t txtime)
{
    char cbuf[CMSG_SPACE(sizeof(uint64_t))];
    struct iovec iov = { (void *)buf, len };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (use_txtime) {
        memset(cbuf, 0, sizeof(cbuf));
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_TXTIME;
        c->cmsg_len = CMSG_LEN(sizeof(uint64_t));
        memcpy(CMSG_DATA(c), &txtime, sizeof(uint64_t));
    }
    return (int)sendmsg(fd, &msg, 0);
}

static void usage(const char *a)
{
    fprintf(stderr,
            "usage: %s [-i IFACE] [-s SECONDS] [-p PERIOD_US] [-l LEAD_US] [-x OFFSET_US]\n"
            "          [-P SO_PRIORITY] [-n] [-C tai|mono] [-X PAST_US] [-w] [-c OUT.csv]\n"
            "defaults: -i enP1p1s0 -s 60 -p 1000 -l 200 -x 0 -P 3\n", a);
}

int main(int argc, char **argv)
{
    const char *ifname = "enP1p1s0", *csv_path = NULL;
    int seconds = 60, period_us = 1000, lead_us = 200, offset_us = 0, prio = 3, opt;
    int no_txtime = 0, past_us = 0;
    clockid_t txclock = CLOCK_TAI;

    while ((opt = getopt(argc, argv, "i:s:p:l:x:P:nC:X:wc:h")) != -1) {
        switch (opt) {
        case 'i': ifname = optarg; break;
        case 's': seconds = atoi(optarg); break;
        case 'p': period_us = atoi(optarg); break;
        case 'l': lead_us = atoi(optarg); break;
        case 'x': offset_us = atoi(optarg); break;
        case 'P': prio = atoi(optarg); break;
        case 'n': no_txtime = 1; break;
        case 'C':
            if (!strcmp(optarg, "tai")) txclock = CLOCK_TAI;
            else if (!strcmp(optarg, "mono")) txclock = CLOCK_MONOTONIC;
            else { usage(argv[0]); return 2; }
            break;
        case 'X': past_us = atoi(optarg); break;
        case 'w': g_sw_ts = 1; break;
        case 'c': csv_path = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (seconds <= 0 || period_us < 100 || lead_us < 0) { usage(argv[0]); return 2; }
    if (no_txtime) lead_us = 0;

    int ifindex = (int)if_nametoindex(ifname);
    if (!ifindex) { fprintf(stderr, "%s: no such interface\n", ifname); return 1; }

    /* protocol 0: transmit only. A socket that also received its own
     * frames (loopback, veth) would fill its receive buffer, and the kernel
     * then silently stops queueing TX timestamps on the error queue. */
    int fd = socket(AF_PACKET, SOCK_RAW, 0);
    if (fd < 0) { perror("socket(AF_PACKET)"); return 1; }
    struct sockaddr_ll sll = { .sll_family = AF_PACKET, .sll_protocol = 0,
                               .sll_ifindex = ifindex };
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind"); return 1; }

    /* source MAC */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) { perror("SIOCGIFHWADDR"); return 1; }
    uint8_t src[6];
    memcpy(src, ifr.ifr_hwaddr.sa_data, 6);

    /* hardware TX timestamping on, RX filter left as it is */
    struct hwtstamp_config hw;
    memset(&hw, 0, sizeof(hw));
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&hw;
    if (!g_sw_ts) {
        if (ioctl(fd, SIOCGHWTSTAMP, &ifr) < 0) memset(&hw, 0, sizeof(hw));
        hw.tx_type = HWTSTAMP_TX_ON;
        ifr.ifr_data = (void *)&hw;
        if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) { perror("SIOCSHWTSTAMP (need CAP_NET_ADMIN; -w for software)"); return 1; }
    }
    {
        int64_t r1 = now_ns(CLOCK_REALTIME), t = now_ns(CLOCK_TAI), r2 = now_ns(CLOCK_REALTIME);
        g_tai_minus_real = t - (r1 + (r2 - r1) / 2);
    }

    int tsflags = g_sw_ts ? (SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                             SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY)
                          : (SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                             SOF_TIMESTAMPING_OPT_ID | SOF_TIMESTAMPING_OPT_TSONLY);
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &tsflags, sizeof(tsflags)) < 0) {
        perror("SO_TIMESTAMPING"); return 1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0) {
        perror("SO_PRIORITY"); return 1;
    }
    if (!no_txtime) {
        struct sock_txtime stt = { .clockid = txclock, .flags = SOF_TXTIME_REPORT_ERRORS };
        if (setsockopt(fd, SOL_SOCKET, SO_TXTIME, &stt, sizeof(stt)) < 0) {
            perror("SO_TXTIME"); return 1;
        }
    }

    /* frame: dst 01:01:05:01:00:00 (EtherCAT), EtherCAT header + 1 NOP datagram */
    uint8_t frame[60];
    memset(frame, 0, sizeof(frame));
    const uint8_t dst[6] = { 0x01, 0x01, 0x05, 0x01, 0x00, 0x00 };
    memcpy(frame, dst, 6);
    memcpy(frame + 6, src, 6);
    frame[12] = ETH_P_ECAT >> 8; frame[13] = ETH_P_ECAT & 0xFF;
    const uint16_t dlen = 8;                              /* NOP payload: seq + spare */
    uint16_t ech = (uint16_t)((12 + dlen) | (1u << 12));  /* length, type 1 */
    frame[14] = ech & 0xFF; frame[15] = ech >> 8;
    frame[16] = 0x00;                                     /* cmd NOP */
    frame[22] = dlen & 0xFF; frame[23] = (dlen >> 8) & 0x07;

    size_t cap = (size_t)seconds * 1000000 / period_us + 16;
    struct stats st;
    memset(&st, 0, sizeof(st));
    st.cap = cap;
    st.launch = calloc(cap, sizeof(int64_t));
    st.wake = calloc(cap, sizeof(int64_t));
    st.send = calloc(cap, sizeof(int64_t));
    if (!st.launch || !st.wake || !st.send) { fprintf(stderr, "out of memory\n"); return 1; }

    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) { perror(csv_path); return 1; }
        fprintf(csv, "kind,seq_or_code,t_target_tai_ns,tx_hw_ns,launch_err_ns\n");
    }

    mlockall(MCL_CURRENT | MCL_FUTURE);
    signal(SIGINT, on_sigint);

    const char *mode = no_txtime ? "plain send (no SO_TXTIME)"
                     : txclock == CLOCK_MONOTONIC ? "SO_TXTIME CLOCK_MONOTONIC (T-06 wrong clock)"
                     : past_us ? "SO_TXTIME CLOCK_TAI, txtime in the PAST (T-05)"
                     : "SO_TXTIME CLOCK_TAI";
    printf("iface %s  prio %d  period %d us  lead %d us  offset %d us  %d s  %s timestamps\nmode: %s\n",
           ifname, prio, period_us, lead_us, offset_us, seconds, g_sw_ts ? "SOFTWARE" : "hardware", mode);

    const int64_t period = (int64_t)period_us * 1000;
    int64_t t_target = (now_ns(CLOCK_TAI) / period + 50) * period; /* start ~50 periods ahead */
    const int64_t t_end = t_target + (int64_t)seconds * NSEC_PER_SEC;
    uint32_t seq = 0, opt_id = 0;
    int64_t last_report = now_ns(CLOCK_MONOTONIC);

    while (!g_stop && t_target < t_end) {
        int64_t off = tai_minus_mono();
        int64_t tgt = t_target + (int64_t)offset_us * 1000;
        int64_t wake_mono = tgt - off - (int64_t)lead_us * 1000;
        struct timespec w;
        ns_ts(wake_mono, &w);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &w, NULL);
        int64_t woke = now_ns(CLOCK_MONOTONIC);
        push(st.wake, &st.n_wake, cap, woke - wake_mono);
        if (lead_us > 0 && woke + off > tgt) st.late_wake++; /* meaningless with -n (lead 0) */

        /* timestamps of earlier frames */
        drain_errqueue(fd, &st, csv);

        /* frame content: sequence number in the NOP data */
        memcpy(frame + 26, &seq, sizeof(seq));
        uint64_t txtime = (uint64_t)tgt;
        if (txclock == CLOCK_MONOTONIC) txtime = (uint64_t)(tgt - off);
        if (past_us) txtime = (uint64_t)(now_ns(CLOCK_TAI) - (int64_t)past_us * 1000);

        struct slot *s = &g_ring[opt_id % RING];
        s->t_target = tgt;
        s->seq = seq;
        s->used = 1;

        int64_t t0 = now_ns(CLOCK_MONOTONIC);
        int r = send_at(fd, frame, sizeof(frame), !no_txtime, txtime);
        int64_t t1 = now_ns(CLOCK_MONOTONIC);
        push(st.send, &st.n_send, cap, t1 - t0);
        if (r < 0) {
            st.send_err++;
            st.last_send_errno = errno;
            s->used = 0;
            /* A qdisc drop (ETF: NET_XMIT_DROP -> ENOBUFS) happens after
             * packet_snd has taken a timestamp key from sk_tskey, so the
             * key is consumed. Not counting it shifted every later match by
             * one frame (A1 on veth: 3107 unmatched after one late frame). */
            if (errno == ENOBUFS)
                opt_id++;
        } else {
            st.sent++;
            opt_id++;
        }
        seq++;
        t_target += period;

        if (t1 - last_report >= 10 * NSEC_PER_SEC) {
            fprintf(stderr, "  sent %" PRIu64 "  hw_ts %" PRIu64 "  send_err %" PRIu64
                    "  missed %" PRIu64 "  invalid %" PRIu64 "  late_wake %" PRIu64 "\n",
                    st.sent, st.ts_hw, st.send_err, st.txtime_missed, st.txtime_invalid, st.late_wake);
            last_report = t1;
        }
    }

    /* collect what is still in flight (offload holds frames up to delta) */
    for (int i = 0; i < 20; i++) {
        struct pollfd p = { fd, POLLERR, 0 };
        poll(&p, 1, 10);
        drain_errqueue(fd, &st, csv);
    }
    if (csv) fclose(csv);

    uint64_t attempts = st.sent + st.send_err;
    uint64_t reported = st.ts_hw + st.ts_nohw + st.txtime_missed + st.txtime_invalid + st.txtime_other;
    printf("[counts]\n");
    printf("  attempts %" PRIu64 "  sent %" PRIu64 "  send_err %" PRIu64 "%s%s\n", attempts, st.sent,
           st.send_err, st.send_err ? "  last errno " : "", st.send_err ? strerror(st.last_send_errno) : "");
    printf("  tx timestamps %" PRIu64 "  without ts %" PRIu64 "  unmatched %" PRIu64
           "  attempts without any report %" PRId64 "\n", st.ts_hw, st.ts_nohw, st.ts_unmatched,
           (int64_t)attempts - (int64_t)reported);
    printf("  ETF drops: missed %" PRIu64 "  invalid_param %" PRIu64 "  other %" PRIu64 "\n",
           st.txtime_missed, st.txtime_invalid, st.txtime_other);
    printf("  late wake (woke after t_target) %" PRIu64 "\n", st.late_wake);
    printf("[distributions]\n");
    print_dist("launch_err = tx_ts - target", st.launch, st.n_launch);
    print_dist("wake jitter (MONO)", st.wake, st.n_wake);
    print_dist("send call duration", st.send, st.n_send);
    return 0;
}
