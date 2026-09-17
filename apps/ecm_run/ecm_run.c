/* ==========================================================================
 * ecm_run.c — Real cyclic EtherCAT master, two process-data groups running
 * at different rates on a single RT thread (Giai doan 3 / L3 scope).
 *
 * Architecture decision: ONE thread, round-robin by tick count, NOT two
 * real OS threads. Reason: ecx_send/receive_processdata_group() for both
 * groups share the same underlying socket/port; calling them concurrently
 * from two threads risks a race at the socket layer that SOEM does not
 * lock for you. Since io_cycle_us is a whole multiple of motion_cycle_us,
 * a single base tick with a counter serving group 2 every Nth tick is both
 * safe and matches how real single-NIC EtherCAT masters are structured.
 *
 * Group assignment: slaves 1..motion_count -> group 1 (GROUP_MOTION),
 * slaves motion_count+1..slavecount -> group 2 (GROUP_IO). Assigning
 * .group is entirely the caller's responsibility -- confirmed by reading
 * ec_config.c: ecx_config_init()/ecx_set_slaves_to_default() never touch
 * slavelist[].group, so it stays at its calloc'd default (0) until this
 * code sets it, and must be set AFTER ecx_config_init() (so slavelist[]
 * is sized/populated) and BEFORE ecx_config_map_group() (so mapping
 * respects it).
 *
 * ecx_config_map_group(ctx, IOmap, 0) means "map ALL groups" -- confirmed
 * in ec_config.c via the `!group ||` check. ecx_send/receive_processdata_
 * group(ctx, 0) has NO such wildcard -- confirmed by reading ec_main.c,
 * every access there is a direct grouplist[group] index. So group 0 is
 * NEVER used for either call in this file; groups are always 1 and 2.
 *
 * IOMAP_SIZE note: SOEM 2.0.0's ecx_clearmbxstatus() memsets a size derived
 * from Obytes+Ibytes+mbxstatuslength, and mbxstatuslength's internal
 * formula does NOT subtract the group's logstartaddr offset -- confirmed
 * via AddressSanitizer (global-buffer-overflow, WRITE of size 65540 into a
 * 4096-byte buffer, for a group whose real Obytes+Ibytes was only 32 bytes).
 * This is a quirk of this SOEM build, not something fixable from here --
 * the buffer must simply be sized generously enough to absorb it.
 *
 * IMPORTANT: no fprintf/logging of any kind inside the main cyclic loop.
 * An earlier version printed stats once per second directly inside the
 * loop while running under SCHED_FIFO(80) on an isolated core -- this
 * caused REAL wkc_mismatch and overrun events that were NOT present
 * without RT scheduling, because fprintf's underlying write() can block
 * on terminal/pipe I/O, and a blocked FIFO-priority thread holds the CPU
 * while doing so. Fix: record only numbers into an in-RAM snapshot array
 * during the loop, and print everything once after the loop exits.
 *
 * Build:  see the ecm_run target in the Makefile (same pattern as l1_probe)
 * Run:    sudo ./ecm_run --iface veth_m --n 8 --motion-slaves 4
 *         Ctrl+C to stop, or --duration-sec N to stop automatically.
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>

#include "soem/soem.h"

#include <sys/mman.h>

#define GROUP_MOTION 1
#define GROUP_IO     2

#define IOMAP_SIZE   (256 * 1024)   /* see IOMAP_SIZE note in the file header */

static uint8 IOmap_motion[IOMAP_SIZE];
static uint8 IOmap_io[IOMAP_SIZE];
static ecx_contextt ctx;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* Per-group running stats, updated once per cycle serviced. */
typedef struct {
    const char  *label;
    uint8_t      group;
    long         expected_wkc;
    uint64_t     cycles;
    uint64_t     wkc_mismatch;
    uint64_t     overrun;
    int64_t      cycle_ns;         /* nominal cycle period for this group */
} group_stats_t;

/* One snapshot per second of wall-clock progress, written with NO I/O
 * inside the hot loop -- see the file header note on why this exists. */
typedef struct {
    uint64_t tick;
    uint64_t motion_cycles, motion_mismatch, motion_overrun;
    uint64_t io_cycles,     io_mismatch,     io_overrun;
} snapshot_t;

#define MAX_SNAPSHOTS 4096   /* enough for ~68 minutes at one snapshot/second */
static snapshot_t g_snapshots[MAX_SNAPSHOTS];
static int        g_snapshot_count = 0;

/* ---- timespec helpers ---- */
static void ts_add_ns(struct timespec *ts, int64_t ns)
{
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec  += 1;
    }
}

static int64_t ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (int64_t)(a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

/* Requests ALL slaves (slave=0 broadcasts) into `target`, blocks until
 * reached or timeout. Reused pattern from l2_probe.c's request_state(),
 * generalized to slave=0 since this file operates on the whole bus, not
 * a single slave under test. */
static int request_all_state(int target, int timeout_us)
{
    ctx.slavelist[0].state = target;
    ecx_writestate(&ctx, 0);
    return ecx_statecheck(&ctx, 0, target, timeout_us);
}

/* Runs one process-data exchange for `g`, updates its stats. Called once
 * per due tick for that group -- this is the unit of work shared by both
 * the "background" 1ms rate and the "every Nth tick" 8ms rate.
 * NO fprintf/logging here -- this runs inside the RT hot loop. */
static void service_group(group_stats_t *g)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ecx_send_processdata_group(&ctx, g->group);
    int wkc = ecx_receive_processdata_group(&ctx, g->group, EC_TIMEOUTRET);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t elapsed_ns = ts_diff_ns(&t1, &t0);

    g->cycles++;
    if (wkc != g->expected_wkc) g->wkc_mismatch++;
    if (elapsed_ns > g->cycle_ns) g->overrun++; /* the work itself already exceeded this group's own period */
}

static void print_stats(const group_stats_t *g)
{
    fprintf(stderr,
        "  [%s] cycles=%" PRIu64 " wkc_mismatch=%" PRIu64 " (%.4f%%) overrun=%" PRIu64 " (%.4f%%)\n",
        g->label, g->cycles,
        g->wkc_mismatch, g->cycles ? 100.0 * (double)g->wkc_mismatch / (double)g->cycles : 0.0,
        g->overrun,      g->cycles ? 100.0 * (double)g->overrun      / (double)g->cycles : 0.0);
}

/* Called ONCE after the loop exits -- prints every recorded snapshot, so
 * per-second progress is still visible in the final output without any
 * I/O having happened while the RT loop was actually running. */
static void print_all_snapshots(void)
{
    fprintf(stderr, "\n=== Per-second progress (%d snapshot(s), printed after loop exit) ===\n", g_snapshot_count);
    for (int i = 0; i < g_snapshot_count; i++) {
        const snapshot_t *s = &g_snapshots[i];
        fprintf(stderr,
            "-- tick=%" PRIu64 " -- motion: cycles=%" PRIu64 " mismatch=%" PRIu64 " overrun=%" PRIu64
            " | io: cycles=%" PRIu64 " mismatch=%" PRIu64 " overrun=%" PRIu64 "\n",
            s->tick, s->motion_cycles, s->motion_mismatch, s->motion_overrun,
            s->io_cycles, s->io_mismatch, s->io_overrun);
    }
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int n = 0;
    int motion_slaves = -1;
    long motion_cycle_us = 1000;
    long io_cycle_us     = 8000;
    long duration_sec    = 0; /* 0 = run until Ctrl+C */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--motion-slaves") == 0 && i + 1 < argc) {
            motion_slaves = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--motion-cycle-us") == 0 && i + 1 < argc) {
            motion_cycle_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--io-cycle-us") == 0 && i + 1 < argc) {
            io_cycle_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--duration-sec") == 0 && i + 1 < argc) {
            duration_sec = atol(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (!ifname || n <= 0 || motion_slaves <= 0 || motion_slaves >= n) {
        fprintf(stderr,
            "Usage: %s --iface <veth_m> --n <total_slaves> --motion-slaves <count> "
            "[--motion-cycle-us N] [--io-cycle-us N] [--duration-sec N]\n", argv[0]);
        return 1;
    }
    if (io_cycle_us % motion_cycle_us != 0) {
        fprintf(stderr,
            "io-cycle-us (%ld) must be a whole multiple of motion-cycle-us (%ld) "
            "for the round-robin tick counter to land exactly.\n", io_cycle_us, motion_cycle_us);
        return 1;
    }
    long ticks_per_io = io_cycle_us / motion_cycle_us;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        /* not fatal -- warn and continue, since this is a hardening step,
        * not a correctness requirement */
    }

    if (!ecx_init(&ctx, ifname)) {
        fprintf(stderr, "ecx_init on %s failed\n", ifname);
        return 1;
    }
    int wc = ecx_config_init(&ctx);
    if (wc <= 0) {
        fprintf(stderr, "No slaves found.\n");
        ecx_close(&ctx);
        return 1;
    }
    if (wc < n) {
        fprintf(stderr, "Warning: found %d slave(s), expected %d -- continuing with what was found.\n", wc, n);
        n = wc;
        if (motion_slaves >= n) motion_slaves = n - 1;
    }

    /* ---- Group assignment: MUST happen after config_init, before map_group ---- */
    for (int s = 1; s <= n; s++) {
        ctx.slavelist[s].group = (s <= motion_slaves) ? GROUP_MOTION : GROUP_IO;
    }

    int motion_iomap_size = ecx_config_map_group(&ctx, IOmap_motion, GROUP_MOTION);
    int io_iomap_size     = ecx_config_map_group(&ctx, IOmap_io,     GROUP_IO);
    fprintf(stderr, "ecm_run: GROUP_MOTION (%d slave, %d byte IOmap), GROUP_IO (%d slave, %d byte IOmap)\n",
            motion_slaves, motion_iomap_size, n - motion_slaves, io_iomap_size);

    group_stats_t motion = {
        .label = "GROUP_MOTION", .group = GROUP_MOTION,
        .expected_wkc = (ctx.grouplist[GROUP_MOTION].outputsWKC * 2) + ctx.grouplist[GROUP_MOTION].inputsWKC,
        .cycle_ns = motion_cycle_us * 1000L,
    };
    group_stats_t io = {
        .label = "GROUP_IO", .group = GROUP_IO,
        .expected_wkc = (ctx.grouplist[GROUP_IO].outputsWKC * 2) + ctx.grouplist[GROUP_IO].inputsWKC,
        .cycle_ns = io_cycle_us * 1000L,
    };
    fprintf(stderr, "ecm_run: expected WKC -- motion=%ld io=%ld\n", motion.expected_wkc, io.expected_wkc);

    /* ---- Bring the whole bus up to SAFEOP ---- */
    if (!request_all_state(EC_STATE_PRE_OP, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach PREOP\n"); ecx_close(&ctx); return 1;
    }
    if (!request_all_state(EC_STATE_SAFE_OP, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach SAFEOP\n"); ecx_close(&ctx); return 1;
    }

    /* ---- One real cycle per group BEFORE requesting OP -- satisfies each
     * ESC's "must have received valid outputs" precondition (see L2-04). */
    ecx_send_processdata_group(&ctx, GROUP_MOTION);
    ecx_receive_processdata_group(&ctx, GROUP_MOTION, EC_TIMEOUTRET);
    ecx_send_processdata_group(&ctx, GROUP_IO);
    ecx_receive_processdata_group(&ctx, GROUP_IO, EC_TIMEOUTRET);

    if (!request_all_state(EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach OPERATIONAL\n"); ecx_close(&ctx); return 1;
    }
    fprintf(stderr, "ecm_run: all slaves in OPERATIONAL. Starting cyclic loop (base tick = %ld us, IO every %ld ticks).\n",
            motion_cycle_us, ticks_per_io);
    fprintf(stderr, "ecm_run: no further stderr output until the loop ends -- see file header for why.\n");

    signal(SIGINT, on_sigint);

    struct timespec next, start;
    clock_gettime(CLOCK_MONOTONIC, &next);
    start = next;
    uint64_t tick = 0;

    /* Ticks-per-second, used only to decide when to record a snapshot --
     * still no I/O happens as a result, just an array write. */
    long ticks_per_snapshot = (motion_cycle_us > 0) ? (1000000L / motion_cycle_us) : 1;
    if (ticks_per_snapshot < 1) ticks_per_snapshot = 1;

    while (!g_stop) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        service_group(&motion);
        if (tick % (uint64_t)ticks_per_io == 0) {
            service_group(&io);
        }
        tick++;

        if (tick % (uint64_t)ticks_per_snapshot == 0 && g_snapshot_count < MAX_SNAPSHOTS) {
            g_snapshots[g_snapshot_count++] = (snapshot_t){
                .tick = tick,
                .motion_cycles = motion.cycles, .motion_mismatch = motion.wkc_mismatch, .motion_overrun = motion.overrun,
                .io_cycles     = io.cycles,     .io_mismatch     = io.wkc_mismatch,     .io_overrun     = io.overrun,
            };
        }

        ts_add_ns(&next, motion_cycle_us * 1000L);

        if (duration_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (ts_diff_ns(&now, &start) >= duration_sec * 1000000000L) break;
        }
    }

    print_all_snapshots();

    fprintf(stderr, "\n=== Final stats after %" PRIu64 " motion ticks ===\n", tick);
    print_stats(&motion);
    print_stats(&io);

    request_all_state(EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&ctx);
    return 0;
}