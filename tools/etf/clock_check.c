/*
 * clock_check.c -- Phase 8.5 step 6.1 (T-01, T-02): is the i226 PHC tracking
 * the kernel's CLOCK_TAI, and is CLOCK_TAI - CLOCK_MONOTONIC free of steps?
 *
 * With ETF offload the NIC launches a frame when its PHC reaches txtime,
 * while the ETF qdisc validates txtime against the kernel's CLOCK_TAI.
 * Both must agree. This tool measures:
 *
 *   1. kernel TAI offset: adjtimex().tai and a direct CLOCK_TAI - CLOCK_REALTIME
 *      read (they must agree; usually 0 on a box without ptp4l/leapsectz)
 *   2. PHC - TAI, every period, as a distribution (p50/p99/p99.9/max).
 *      method "extended": PTP_SYS_OFFSET_EXTENDED, the driver brackets the
 *        PHC register read with CLOCK_REALTIME reads (tightest window);
 *        converted to TAI with the kernel TAI offset.
 *      method "gettime": clock_gettime(TAI), clock_gettime(PHC fd),
 *        clock_gettime(TAI), midpoint. Wider window, independent path.
 *   3. TAI - MONO drift and the largest step between consecutive samples.
 *      gettime and TAI - MONO take the narrowest of 5 bracketed reads.
 *      An NTP step moves TAI but not MONO.
 *
 * Usage:
 *   clock_check [-i enP1p1s0] [-d /dev/ptpN] [-s 600] [-p 10]
 *               [-m extended|gettime] [-c out.csv]
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o clock_check clock_check.c
 *
 * Needs read access to /dev/ptpN (root, or a udev rule) and SIOCETHTOOL
 * (unprivileged is fine for GET_TS_INFO).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/ethtool.h>
#include <linux/ptp_clock.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif

#define CLOCKFD 3
#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned int)~(fd)) << 3) | CLOCKFD))

#define NSEC_PER_SEC ((int64_t)1000000000)

/* T-01 pass threshold and T-02 step threshold */
#define T01_P99_LIMIT_NS 1000
#define T02_STEP_LIMIT_NS 100000 /* 100 us between samples = a step, not a slew */

enum method { M_EXTENDED, M_GETTIME };

static int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec;
}

static int64_t now_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return ts_ns(&t);
}

static int64_t ptp_ns(const struct ptp_clock_time *t)
{
    return (int64_t)t->sec * NSEC_PER_SEC + t->nsec;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

/* PHC index of an interface via ETHTOOL_GET_TS_INFO, -1 if none */
static int phc_index_of(const char *ifname)
{
    struct ethtool_ts_info info;
    struct ifreq ifr;
    int fd, rc;

    memset(&info, 0, sizeof(info));
    memset(&ifr, 0, sizeof(ifr));
    info.cmd = ETHTOOL_GET_TS_INFO;
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void *)&info;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    rc = ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
    return rc < 0 ? -1 : info.phc_index;
}

/* kernel TAI offset in seconds, as adjtimex reports it */
static int kernel_tai_offset(int *clock_state)
{
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    *clock_state = adjtimex(&tx);
    return tx.tai;
}

/* TAI - REALTIME read directly, midpoint of REALTIME, TAI, REALTIME */
static int64_t measured_tai_minus_real(void)
{
    int64_t r1 = now_ns(CLOCK_REALTIME);
    int64_t t = now_ns(CLOCK_TAI);
    int64_t r2 = now_ns(CLOCK_REALTIME);
    return t - (r1 + (r2 - r1) / 2);
}

/* PHC - TAI via PTP_SYS_OFFSET_EXTENDED; returns 0 on success */
static int sample_extended(int ptp_fd, int64_t tai_off_ns, int64_t *off, int64_t *win)
{
    struct ptp_sys_offset_extended e;
    int64_t best_win = INT64_MAX, best_off = 0;

    memset(&e, 0, sizeof(e));
    e.n_samples = 5;
    if (ioctl(ptp_fd, PTP_SYS_OFFSET_EXTENDED, &e) < 0)
        return -1;
    for (unsigned i = 0; i < e.n_samples; i++) {
        int64_t pre = ptp_ns(&e.ts[i][0]);  /* CLOCK_REALTIME */
        int64_t phc = ptp_ns(&e.ts[i][1]);
        int64_t post = ptp_ns(&e.ts[i][2]); /* CLOCK_REALTIME */
        int64_t w = post - pre;
        if (w < best_win) {
            best_win = w;
            best_off = phc - (pre + w / 2 + tai_off_ns);
        }
    }
    *off = best_off;
    *win = best_win;
    return 0;
}

/* PHC - TAI via clock_gettime on the PHC fd, bracketed by CLOCK_TAI.
 * Best (narrowest window) of TRIES reads: a read that got preempted has a
 * wide window and a midpoint that can be off by half of it. */
#define TRIES 5
static void sample_gettime(clockid_t phc, int64_t *off, int64_t *win)
{
    int64_t best_w = INT64_MAX, best_o = 0;
    for (int i = 0; i < TRIES; i++) {
        int64_t t1 = now_ns(CLOCK_TAI);
        int64_t p = now_ns(phc);
        int64_t t2 = now_ns(CLOCK_TAI);
        if (t2 - t1 < best_w) {
            best_w = t2 - t1;
            best_o = p - (t1 + (t2 - t1) / 2);
        }
    }
    *off = best_o;
    *win = best_w;
}

/* TAI - MONO, midpoint of TAI, MONO, TAI (same rule the ETF loop will use),
 * best of TRIES by window. A single preempted read showed up as a 92 us
 * "step" in the first T-02 run while the drift over 600 s was 16 ns. */
static int64_t tai_minus_mono(void)
{
    int64_t best_w = INT64_MAX, best = 0;
    for (int i = 0; i < TRIES; i++) {
        int64_t a = now_ns(CLOCK_TAI);
        int64_t m = now_ns(CLOCK_MONOTONIC);
        int64_t b = now_ns(CLOCK_TAI);
        if (b - a < best_w) {
            best_w = b - a;
            best = a + (b - a) / 2 - m;
        }
    }
    return best;
}

static void print_dist(const char *name, int64_t *v, size_t n)
{
    if (n == 0) {
        printf("  %-22s no samples\n", name);
        return;
    }
    qsort(v, n, sizeof(v[0]), cmp_i64);
    printf("  %-22s p50 %8" PRId64 "  p99 %8" PRId64 "  p99.9 %8" PRId64
           "  max %8" PRId64 " ns   (n=%zu)\n",
           name, v[n / 2], v[(size_t)(n * 0.99)], v[(size_t)(n * 0.999)], v[n - 1], n);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-i iface] [-d /dev/ptpN] [-s seconds] [-p period_ms]\n"
            "          [-m extended|gettime] [-c out.csv]\n"
            "defaults: -i enP1p1s0 -s 600 -p 10 -m extended\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *ifname = "enP1p1s0";
    const char *ptp_path = NULL;
    const char *csv_path = NULL;
    int seconds = 600, period_ms = 10, opt;
    enum method method = M_EXTENDED;
    char ptp_buf[32];

    while ((opt = getopt(argc, argv, "i:d:s:p:m:c:h")) != -1) {
        switch (opt) {
        case 'i': ifname = optarg; break;
        case 'd': ptp_path = optarg; break;
        case 's': seconds = atoi(optarg); break;
        case 'p': period_ms = atoi(optarg); break;
        case 'm':
            if (!strcmp(optarg, "extended")) method = M_EXTENDED;
            else if (!strcmp(optarg, "gettime")) method = M_GETTIME;
            else { usage(argv[0]); return 2; }
            break;
        case 'c': csv_path = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (seconds <= 0 || period_ms <= 0) {
        usage(argv[0]);
        return 2;
    }

    if (!ptp_path) {
        int idx = phc_index_of(ifname);
        if (idx < 0) {
            fprintf(stderr, "%s: no PHC (ETHTOOL_GET_TS_INFO failed or phc_index -1)\n", ifname);
            return 1;
        }
        snprintf(ptp_buf, sizeof(ptp_buf), "/dev/ptp%d", idx);
        ptp_path = ptp_buf;
    }
    int ptp_fd = open(ptp_path, O_RDWR);
    if (ptp_fd < 0) {
        fprintf(stderr, "open %s: %s\n", ptp_path, strerror(errno));
        return 1;
    }
    clockid_t phc = FD_TO_CLOCKID(ptp_fd);
    {
        struct timespec probe;
        if (clock_gettime(phc, &probe) < 0) {
            fprintf(stderr, "%s is not a PHC: clock_gettime: %s\n", ptp_path, strerror(errno));
            return 1;
        }
    }

    /* ---- 1. kernel TAI offset ---- */
    int clock_state;
    int tai = kernel_tai_offset(&clock_state);
    int64_t tai_real = measured_tai_minus_real();
    int64_t tai_off_ns = (int64_t)tai * NSEC_PER_SEC;
    printf("iface %s  phc %s  method %s  %d s @ %d ms\n", ifname, ptp_path,
           method == M_EXTENDED ? "extended" : "gettime", seconds, period_ms);
    printf("[kernel TAI offset]\n");
    printf("  adjtimex().tai          %d s   (clock state %d%s)\n", tai, clock_state,
           clock_state == TIME_ERROR ? " = TIME_ERROR, clock not synchronised" : "");
    printf("  CLOCK_TAI-CLOCK_REALTIME %.6f s measured\n", (double)tai_real / 1e9);
    if (abs64(tai_real - tai_off_ns) > 1000000)
        printf("  !! measured offset disagrees with adjtimex by > 1 ms\n");
    printf("  -> phc2sys -O %d\n", tai);

    if (method == M_EXTENDED) {
        int64_t o, w;
        if (sample_extended(ptp_fd, tai_off_ns, &o, &w) < 0) {
            fprintf(stderr, "PTP_SYS_OFFSET_EXTENDED: %s -- falling back to gettime\n",
                    strerror(errno));
            method = M_GETTIME;
        }
    }

    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "open %s: %s\n", csv_path, strerror(errno));
            return 1;
        }
        fprintf(csv, "t_mono_ns,phc_minus_tai_ns,window_ns,tai_minus_mono_ns\n");
    }

    size_t cap = (size_t)seconds * 1000 / period_ms + 1, n = 0;
    int64_t *off_abs = calloc(cap, sizeof(int64_t));
    int64_t *win = calloc(cap, sizeof(int64_t));
    if (!off_abs || !win) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    int64_t off_min = INT64_MAX, off_max = INT64_MIN;
    long double off_sum = 0; /* int64 overflows with an unlocked PHC (1e16 ns x 3000) */
    int64_t tm_first = tai_minus_mono(), tm_prev = tm_first, tm_last = tm_first;
    int64_t tm_max_step = 0, n_steps = 0;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int64_t t_start = ts_ns(&next), last_report = t_start;

    while (n < cap) {
        next.tv_nsec += (long)period_ms * 1000000L;
        while (next.tv_nsec >= NSEC_PER_SEC) {
            next.tv_nsec -= NSEC_PER_SEC;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        int64_t o, w;
        if (method == M_EXTENDED) {
            if (sample_extended(ptp_fd, tai_off_ns, &o, &w) < 0)
                continue;
        } else {
            sample_gettime(phc, &o, &w);
        }
        int64_t tm = tai_minus_mono();
        int64_t step = abs64(tm - tm_prev);
        if (step > tm_max_step)
            tm_max_step = step;
        if (step > T02_STEP_LIMIT_NS)
            n_steps++;
        tm_prev = tm_last = tm;

        off_abs[n] = abs64(o);
        win[n] = w;
        n++;
        off_sum += (long double)o;
        if (o < off_min) off_min = o;
        if (o > off_max) off_max = o;

        int64_t t_now = ts_ns(&next);
        if (csv)
            fprintf(csv, "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
                    t_now - t_start, o, w, tm);
        if (t_now - last_report >= 10 * NSEC_PER_SEC) {
            fprintf(stderr, "  t=%4" PRId64 " s  PHC-TAI %10" PRId64 " ns  window %6" PRId64
                            " ns  TAI-MONO drift %+" PRId64 " ns\n",
                    (t_now - t_start) / NSEC_PER_SEC, o, w, tm - tm_first);
            last_report = t_now;
        }
    }
    if (csv)
        fclose(csv);

    printf("[PHC - TAI]  signed min %" PRId64 "  mean %" PRId64 "  max %" PRId64 " ns\n",
           off_min, n ? (int64_t)(off_sum / (long double)n) : 0, off_max);
    print_dist("|PHC - TAI|", off_abs, n);
    print_dist("read window", win, n);
    printf("[TAI - MONO]\n");
    printf("  drift over run          %+" PRId64 " ns\n", tm_last - tm_first);
    printf("  largest sample-to-sample step %" PRId64 " ns, steps > %d ns: %" PRId64 "\n",
           tm_max_step, T02_STEP_LIMIT_NS, n_steps);

    int64_t p99 = n ? off_abs[(size_t)(n * 0.99)] : INT64_MAX; /* sorted by print_dist */
    printf("\nT-01 |PHC-TAI| p99 %" PRId64 " ns < %d ns : %s\n", p99, T01_P99_LIMIT_NS,
           p99 < T01_P99_LIMIT_NS ? "PASS" : "FAIL");
    printf("T-02 no TAI-MONO step > %d ns        : %s\n", T02_STEP_LIMIT_NS,
           n_steps == 0 ? "PASS" : "FAIL");

    free(off_abs);
    free(win);
    close(ptp_fd);
    return (p99 < T01_P99_LIMIT_NS && n_steps == 0) ? 0 : 1;
}
