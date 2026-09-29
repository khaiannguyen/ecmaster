/*
 * soem_txtime_smoke.c -- GD8 8.5 step 6.3: SOEM + soem-txtime.patch end to end,
 * before touching libecmaster/ecm_run.
 *
 *   config (BRD/APWR/FPRD/SII/mailbox ... all "asap" frames) -> SAFE-OP -> OP
 *   -> N cycles, each LRW with txtime = next period boundary (TAI) + lead offset.
 *
 * Every frame goes through ETF when the interface has an ETF qdisc: a config
 * frame without a valid txtime would be dropped and ecx_config_init() would
 * find 0 slaves -- that is the point of the test.
 *
 * Usage: soem_txtime_smoke IFACE [-t] [-n cycles] [-p period_us] [-l lead_us]
 *                             [-a asap_us] [-b ns_per_byte] [-P so_priority]
 *   -t   enable txtime (without it: plain send(), the af_packet baseline)
 *
 * Build (from the ecmaster repo, SOEM built with the patch):
 *   gcc -O2 -Wall -Wextra -o tools/etf/soem_txtime_smoke tools/etf/soem_txtime_smoke.c \
 *       -I$HOME/projects/SOEM/include -I$HOME/projects/SOEM/build/include \
 *       -I$HOME/projects/SOEM/oshw/linux -I$HOME/projects/SOEM/osal/linux -I$HOME/projects/SOEM/osal \
 *       $HOME/projects/SOEM/build/libsoem.a -lpthread -lrt
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "soem/soem.h"

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif

static ecx_contextt ctx;
static uint8 IOmap[4096];

static int64_t now_ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int64_t tai_minus_mono(void)
{
    int64_t a = now_ns(CLOCK_TAI), m = now_ns(CLOCK_MONOTONIC), b = now_ns(CLOCK_TAI);
    return a + (b - a) / 2 - m;
}

int main(int argc, char **argv)
{
    int use_txtime = 0, cycles = 10000, period_us = 1000, lead_us = 700, asap_us = 150;
    int ns_per_byte = 8, prio = 3, opt;

    if (argc < 2) {
        fprintf(stderr, "usage: %s IFACE [-t] [-n cycles] [-p period_us] [-l lead_us] "
                        "[-a asap_us] [-b ns_per_byte] [-P prio]\n", argv[0]);
        return 2;
    }
    const char *ifname = argv[1];
    optind = 2;
    while ((opt = getopt(argc, argv, "tn:p:l:a:b:P:")) != -1) {
        switch (opt) {
        case 't': use_txtime = 1; break;
        case 'n': cycles = atoi(optarg); break;
        case 'p': period_us = atoi(optarg); break;
        case 'l': lead_us = atoi(optarg); break;
        case 'a': asap_us = atoi(optarg); break;
        case 'b': ns_per_byte = atoi(optarg); break;
        case 'P': prio = atoi(optarg); break;
        default: return 2;
        }
    }

    mlockall(MCL_CURRENT | MCL_FUTURE);
    if (!ecx_init(&ctx, ifname)) {
        fprintf(stderr, "ecx_init(%s) failed\n", ifname);
        return 1;
    }
    if (use_txtime) {
        if (!ecx_txtime_enable(&ctx.port, prio, (int64_t)asap_us * 1000, (uint32_t)ns_per_byte)) {
            perror("ecx_txtime_enable");
            return 1;
        }
    }
    printf("iface %s  txtime %s  prio %d  asap %d us  lead %d us  period %d us\n", ifname,
           use_txtime ? "ON" : "off", prio, asap_us, lead_us, period_us);

    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    int n = ecx_config_init(&ctx);
    printf("config_init: %d slave(s) in %.1f ms\n", n, (now_ns(CLOCK_MONOTONIC) - t0) / 1e6);
    if (n <= 0) {
        printf("RESULT: FAIL (no slave: config frames dropped?)  late %u  send_err %u\n",
               ctx.port.txtime_late, ctx.port.txtime_send_err);
        return 1;
    }
    ecx_config_map_group(&ctx, IOmap, 0);
    int expected = ctx.grouplist[0].outputsWKC * 2 + ctx.grouplist[0].inputsWKC;
    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

    /* a few plain cycles so the slaves see valid outputs, then OP */
    for (int i = 0; i < 20; i++) {
        ecx_send_processdata(&ctx);
        ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
    }
    ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&ctx, 0);
    for (int i = 0; i < 200 && ecx_statecheck(&ctx, 0, EC_STATE_OPERATIONAL, 5000) != EC_STATE_OPERATIONAL; i++) {
        ecx_send_processdata(&ctx);
        ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
    }
    int op = ctx.slavelist[0].state == EC_STATE_OPERATIONAL;
    printf("state: %s  expected WKC %d\n", op ? "OP" : "not OP", expected);

    /* cyclic: wake at t_target - lead (MONO), launch at t_target (TAI) */
    const int64_t period = (int64_t)period_us * 1000;
    int64_t t_target = (now_ns(CLOCK_TAI) / period + 10) * period;
    int wkc_ok = 0, wkc_bad = 0, noframe = 0;
    uint32_t late0 = ctx.port.txtime_late, err0 = ctx.port.txtime_send_err;
    for (int i = 0; i < cycles; i++) {
        int64_t off = tai_minus_mono();
        struct timespec w;
        int64_t wake = t_target - off - (int64_t)lead_us * 1000;
        w.tv_sec = wake / 1000000000LL;
        w.tv_nsec = wake % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &w, NULL);
        if (use_txtime)
            ecx_txtime_set_next(&ctx.port, t_target);
        ecx_send_processdata(&ctx);
        int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
        if (wkc == EC_NOFRAME) noframe++;
        else if (wkc == expected) wkc_ok++;
        else wkc_bad++;
        t_target += period;
    }
    printf("cycles %d: WKC ok %d  wrong %d  noframe %d  | txtime late %u  send_err %u\n", cycles,
           wkc_ok, wkc_bad, noframe, ctx.port.txtime_late - late0, ctx.port.txtime_send_err - err0);

    ctx.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&ctx, 0);
    ecx_close(&ctx);
    int pass = op && wkc_ok == cycles;
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
