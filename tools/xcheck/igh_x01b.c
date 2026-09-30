/* ==========================================================================
 * igh_x01b.c — GD8 X-01b: an INDEPENDENT master (IgH EtherCAT Master 1.6,
 * userspace library) drives soft_bus to OP, exchanges process data every
 * cycle and runs Distributed Clocks in BUS-SHIFT mode (the application time
 * is written into the reference clock every cycle) -- the opposite of the
 * master-shift loop ecm_run uses, so soft_bus gets exercised on a code path
 * SOEM never touches (writes to 0x0910 on the reference clock).
 *
 * Pass criteria (printed at the end):
 *   - every slave reaches OP and stays there
 *   - domain working counter COMPLETE on every cycle after the first OP cycle
 *   - DC sync monitor (max |0x092C| over all slaves) reported, no alarm
 *   - no cycle overrun (wake-up later than one full cycle)
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o igh_x01b igh_x01b.c \
 *       -I/opt/etherlab/include -L/opt/etherlab/lib -lethercat \
 *       -Wl,-rpath,/opt/etherlab/lib
 * Run (needs /dev/EtherCAT0 access, SCHED_FIFO, mlockall):
 *   sudo ./igh_x01b --n 8 --seconds 600
 *
 * GD9.3 X-04 (tools/xcheck/run_x04.sh), against soft_bus --coe-ca:
 *   --complete-sdo   configure 0x1C12 = {1, 0x1600} and 0x1C13 = {1, 0x1A00}
 *                    on every slave with ecrt_slave_config_complete_sdo()
 *                    (Complete Access download in PREOP, data 01 00 00 16 /
 *                    01 00 00 1A: SI0 as U8 + pad byte, ETG.1000.6)
 *   --sdo8002 LEN    configure 0x8002:00 with LEN byte (1..400) via
 *                    ecrt_slave_config_sdo() -> IgH normal (LEN <= 4:
 *                    expedited) or segmented download in PREOP
 * ========================================================================== */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "ecrt.h"

#define VENDOR_ID     0x00000499u
#define PRODUCT_CODE  0x00000001u
#define OUT_INDEX     0x7000       /* RxPDO entry (master -> slave), SM2 */
#define IN_INDEX      0x6000       /* TxPDO entry (slave -> master), SM3 */
#define MAX_SLAVES    64
#define NSEC_PER_SEC  1000000000LL

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec;
}

static void ts_add(struct timespec *t, int64_t ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= NSEC_PER_SEC) { t->tv_nsec -= NSEC_PER_SEC; t->tv_sec++; }
}

static const char *wc_name(ec_wc_state_t s)
{
    switch (s) {
    case EC_WC_ZERO:       return "ZERO";
    case EC_WC_INCOMPLETE: return "INCOMPLETE";
    case EC_WC_COMPLETE:   return "COMPLETE";
    default:               return "?";
    }
}

int main(int argc, char **argv)
{
    int n = 8, seconds = 60, cpu = 3, prio = 80;
    int64_t cycle_ns = 1000000;             /* 1 ms */
    int32_t sync0_shift_ns = 0;
    int use_dc = 1;
    int complete_sdo = 0;                   /* GD9.3 X-04 */
    int sdo8002_len = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--n") && i + 1 < argc)              n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)   seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cycle-us") && i + 1 < argc)  cycle_ns = atoll(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc)       cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prio") && i + 1 < argc)      prio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shift-us") && i + 1 < argc)  sync0_shift_ns = atoi(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "--no-dc"))                     use_dc = 0;
        else if (!strcmp(argv[i], "--complete-sdo"))              complete_sdo = 1;
        else if (!strcmp(argv[i], "--sdo8002") && i + 1 < argc)   sdo8002_len = atoi(argv[++i]);
        else {
            fprintf(stderr, "Usage: %s [--n N] [--seconds S] [--cycle-us US] [--cpu C] "
                            "[--prio P] [--shift-us US] [--no-dc] [--complete-sdo] [--sdo8002 LEN]\n", argv[0]);
            return 1;
        }
    }
    if (n < 1 || n > MAX_SLAVES) { fprintf(stderr, "--n must be 1..%d\n", MAX_SLAVES); return 1; }
    if (sdo8002_len < 0 || sdo8002_len > 400) { fprintf(stderr, "--sdo8002 must be 1..400\n"); return 1; }
    static uint8_t blob8002[400];
    for (int k = 0; k < sdo8002_len; k++) blob8002[k] = (uint8_t)('A' + k % 26);  /* printable: `ethercat upload -t string` */
    static const uint8_t ca_1c12[4] = { 0x01, 0x00, 0x00, 0x16 };
    static const uint8_t ca_1c13[4] = { 0x01, 0x00, 0x00, 0x1A };

    /* ---- configuration (non-RT) ---- */
    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "ecrt_request_master(0) failed (is ethercatctl started?)\n"); return 1; }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) { fprintf(stderr, "create_domain failed\n"); return 1; }

    ec_slave_config_t *sc[MAX_SLAVES];
    int off_out[MAX_SLAVES], off_in[MAX_SLAVES];

    for (int i = 0; i < n; i++) {
        sc[i] = ecrt_master_slave_config(master, 0, (uint16_t)i, VENDOR_ID, PRODUCT_CODE);
        if (!sc[i]) { fprintf(stderr, "slave_config %d failed\n", i); return 1; }

        /* No ecrt_slave_config_pdos(): use the default mapping from SII.
         * A default soft_bus has no 0x1C12/0x1C13 (IgH gets an abort and
         * falls back to SII); with --coe-ca (GD9.3) IgH reads the mapping
         * over CoE instead. The PDO assignment is written only on request
         * (--complete-sdo), as raw Complete Access configuration data. */
        if (complete_sdo &&
            (ecrt_slave_config_complete_sdo(sc[i], 0x1C12, ca_1c12, sizeof(ca_1c12)) ||
             ecrt_slave_config_complete_sdo(sc[i], 0x1C13, ca_1c13, sizeof(ca_1c13)))) {
            fprintf(stderr, "slave %d: ecrt_slave_config_complete_sdo failed\n", i);
            return 1;
        }
        if (sdo8002_len > 0 &&
            ecrt_slave_config_sdo(sc[i], 0x8002, 0, blob8002, (size_t)sdo8002_len)) {
            fprintf(stderr, "slave %d: ecrt_slave_config_sdo 0x8002 failed\n", i);
            return 1;
        }
        off_out[i] = ecrt_slave_config_reg_pdo_entry(sc[i], OUT_INDEX, 1, domain, NULL);
        off_in[i]  = ecrt_slave_config_reg_pdo_entry(sc[i], IN_INDEX, 1, domain, NULL);
        if (off_out[i] < 0 || off_in[i] < 0) {
            fprintf(stderr, "slave %d: register 0x%04X:01 / 0x%04X:01 failed (%d/%d) -- "
                            "does the SII carry real entry indices?\n",
                    i, OUT_INDEX, IN_INDEX, off_out[i], off_in[i]);
            return 1;
        }

        if (use_dc) /* AssignActivate 0x0300: SYNC0 active, cyclic */
            ecrt_slave_config_dc(sc[i], 0x0300, (uint32_t)cycle_ns, sync0_shift_ns, 0, 0);
    }

    if (ecrt_master_activate(master)) { fprintf(stderr, "master_activate failed\n"); return 1; }
    uint8_t *pd = ecrt_domain_data(domain);
    if (!pd) { fprintf(stderr, "domain_data NULL\n"); return 1; }

    printf("igh_x01b: N=%d cycle=%lld us DC=%s (bus-shift, SYNC0 shift %d us) seconds=%d cpu=%d prio=%d\n",
           n, (long long)(cycle_ns / 1000), use_dc ? "on" : "off", sync0_shift_ns / 1000,
           seconds, cpu, prio);
    if (complete_sdo || sdo8002_len)
        printf("igh_x01b: config SDOs: %s%s%d byte 0x8002:00\n",
               complete_sdo ? "CA 0x1C12/0x1C13, " : "", sdo8002_len ? "" : "no ", sdo8002_len);
    for (int i = 0; i < n; i++)
        printf("  slave %d: out 0x%04X:01 @%d, in 0x%04X:01 @%d\n", i, OUT_INDEX, off_out[i], IN_INDEX, off_in[i]);

    /* ---- RT setup ---- */
    if (mlockall(MCL_CURRENT | MCL_FUTURE)) perror("mlockall");
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set)) perror("sched_setaffinity");
    struct sched_param sp = { .sched_priority = prio };
    if (sched_setscheduler(0, SCHED_FIFO, &sp)) perror("sched_setscheduler (run with sudo)");
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* ---- statistics ---- */
    uint64_t cycles = 0, cycles_all_op = 0, wc_bad_after_op = 0, overruns = 0;
    uint64_t in_changed = 0, sync_samples = 0;
    uint32_t sync_max_ns = 0, sync_last_ns = 0;
    int64_t  lat_max_ns = 0;
    int      first_op_cycle = -1;
    ec_wc_state_t last_wc = EC_WC_ZERO;
    unsigned int last_al = 0, last_resp = 0;
    uint32_t in_prev[MAX_SLAVES] = {0};

    struct timespec wake;
    clock_gettime(CLOCK_MONOTONIC, &wake);
    ts_add(&wake, cycle_ns);
    int64_t t_end = ts_ns(&wake) + (int64_t)seconds * NSEC_PER_SEC;

    while (!g_stop && ts_ns(&wake) < t_end) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t lat = ts_ns(&now) - ts_ns(&wake);
        if (lat > lat_max_ns) lat_max_ns = lat;
        if (lat > cycle_ns) overruns++;

        ecrt_master_receive(master);
        ecrt_domain_process(domain);

        ec_domain_state_t ds;
        ecrt_domain_state(domain, &ds);
        if (ds.wc_state != last_wc) {
            printf("[%8llu] domain WC %u, state %s\n", (unsigned long long)cycles,
                   ds.working_counter, wc_name(ds.wc_state));
            last_wc = ds.wc_state;
        }

        ec_master_state_t ms;
        ecrt_master_state(master, &ms);
        if (ms.al_states != last_al || ms.slaves_responding != last_resp) {
            printf("[%8llu] master: %u slave(s) responding, AL states 0x%02X, link %s\n",
                   (unsigned long long)cycles, ms.slaves_responding, ms.al_states,
                   ms.link_up ? "up" : "down");
            last_al = ms.al_states;
            last_resp = ms.slaves_responding;
        }

        /* Everyone in OP? Only then count WC failures (the ramp-up is not
         * an error). */
        int all_op = (ms.al_states == 0x08 && ms.slaves_responding == (unsigned)n);
        if (all_op) {
            cycles_all_op++;
            if (first_op_cycle < 0) {
                first_op_cycle = (int)cycles;
                printf("[%8llu] all %d slaves in OP\n", (unsigned long long)cycles, n);
            } else if (ds.wc_state != EC_WC_COMPLETE) {
                wc_bad_after_op++;
            }
        }

        /* process data: write a per-slave counter, watch the inputs move */
        for (int i = 0; i < n; i++) {
            EC_WRITE_U32(pd + off_out[i], (uint32_t)(cycles + (uint64_t)i * 0x01000000u));
            uint32_t v = EC_READ_U32(pd + off_in[i]);
            if (v != in_prev[i]) { in_changed++; in_prev[i] = v; }
        }

        /* DC, bus-shift: tell the master the application time, write it into
         * the reference clock, distribute the reference time to the others. */
        if (use_dc) {
            ecrt_master_application_time(master, (uint64_t)ts_ns(&wake));
            ecrt_master_sync_reference_clock(master);
            ecrt_master_sync_slave_clocks(master);

            /* sync monitor: max |System Time Difference 0x092C| over slaves */
            uint32_t d = ecrt_master_sync_monitor_process(master);
            if (cycles > 0 && all_op) {
                sync_last_ns = d;
                if (d > sync_max_ns) sync_max_ns = d;
                sync_samples++;
            }
            ecrt_master_sync_monitor_queue(master);
        }

        ecrt_domain_queue(domain);
        ecrt_master_send(master);

        cycles++;
        if (cycles % (uint64_t)(NSEC_PER_SEC / cycle_ns * 10) == 0) {
            printf("[%8llu] OP cycles %llu, WC bad %llu, sync diff last %u ns / max %u ns, "
                   "lat max %lld us, overruns %llu\n",
                   (unsigned long long)cycles, (unsigned long long)cycles_all_op,
                   (unsigned long long)wc_bad_after_op, sync_last_ns, sync_max_ns,
                   (long long)(lat_max_ns / 1000), (unsigned long long)overruns);
            fflush(stdout);
        }
        ts_add(&wake, cycle_ns);
    }

    /* ---- per-slave final state ---- */
    int slaves_op = 0;
    for (int i = 0; i < n; i++) {
        ec_slave_config_state_t s;
        ecrt_slave_config_state(sc[i], &s);
        if (s.operational && s.al_state == 0x08) slaves_op++;
        else printf("  slave %d: online %u, operational %u, AL 0x%02X\n",
                    i, s.online, s.operational, s.al_state);
    }

    int pass_op   = (first_op_cycle >= 0 && slaves_op == n);
    int pass_wc   = (wc_bad_after_op == 0 && cycles_all_op > 0);
    int pass_over = (overruns == 0);

    printf("\n=== X-01b summary ===\n");
    printf("cycles %llu, first all-OP at cycle %d, OP cycles %llu, slaves in OP at end %d/%d\n",
           (unsigned long long)cycles, first_op_cycle, (unsigned long long)cycles_all_op, slaves_op, n);
    printf("domain WC not COMPLETE after first OP: %llu\n", (unsigned long long)wc_bad_after_op);
    printf("input changes seen: %llu\n", (unsigned long long)in_changed);
    if (use_dc)
        printf("DC sync monitor: %llu samples, max %u ns, last %u ns\n",
               (unsigned long long)sync_samples, sync_max_ns, sync_last_ns);
    printf("wake latency max %lld us, overruns %llu\n",
           (long long)(lat_max_ns / 1000), (unsigned long long)overruns);
    printf("[%s] X-01b-1 all slaves reach and keep OP\n", pass_op ? "PASS" : "FAIL");
    printf("[%s] X-01b-2 domain WC COMPLETE every OP cycle\n", pass_wc ? "PASS" : "FAIL");
    printf("[%s] X-01b-3 no cycle overrun\n", pass_over ? "PASS" : "FAIL");
    if (use_dc)
        printf("[INFO] X-01b-4 DC bus-shift sync diff max %u ns (judge against soft_bus DC model)\n",
               sync_max_ns);

    ecrt_release_master(master);
    return (pass_op && pass_wc && pass_over) ? 0 : 1;
}
