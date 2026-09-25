/* ==========================================================================
 * ecm_run.c — Real cyclic EtherCAT master, two process-data groups running
 * at different rates on the RT thread (Giai doan 3 / L3 scope), now
 * instrumented for Giai doan 4 (5-thread architecture, 4 measured
 * quantities, histograms, turnaround correlation via error queue).
 *
 * ---- Giai doan 4 additions on top of the Giai doan 3 file ----
 *
 * 1. Per-tick instrumentation: wake_jitter_ns, prep_send_ns,
 *    cycle_occupancy_ns are now captured and pushed into a lock-free SPSC
 *    ring (rt_sample_t / ring_spsc.h) for the telemetry thread to consume.
 *    See master_plan_v2.md §4.1 for the definition of each quantity.
 *
 * 2. TX send order is pushed into a second small SPSC ring
 *    (tx_order_ring_t / turnaround.h) at the exact moment of each
 *    sendto() (inside service_group()), so the telemetry thread can match
 *    asynchronous TX completions from MSG_ERRQUEUE back to the right tick
 *    WITHOUT ever reading or embedding anything into PDO payload — see
 *    giai_doan_4_ke_hoach.md §5.2 for why the PDO-embedding approach from
 *    the first draft was rejected (violates master_plan_v2.md §12 "no
 *    hardcoded PDO map / object index in libecmaster").
 *
 * 3. RESOLVED (22/9): read oshw/linux/nicdrv.c directly. TX is fully wired:
 *    ctx.port is an EMBEDDED ecx_portt struct (not a pointer), and
 *    ctx.port.sockhandle is the exact raw AF_PACKET fd SOEM uses
 *    internally for both send() and recv() (see ecx_outframe/
 *    ecx_recvpkt) -- polling its MSG_ERRQUEUE from the telemetry thread
 *    does not conflict with SOEM's own use of that fd, since the error
 *    queue is a separate queue from the normal socket buffer.
 *
 *    RX is a DIFFERENT story: ecx_recvpkt() reads with plain recv()
 *    (MSG_DONTWAIT, no msg_control), so even with SO_TIMESTAMPING
 *    enabled on the same fd, the kernel's per-packet timestamp is
 *    generated and then discarded, because recv() never asked for the
 *    ancillary data that carries it -- confirmed by reading the function
 *    body directly, not assumed. RX is handled by option (b): a second,
 *    independent passive AF_PACKET socket (open_passive_rx_socket()),
 *    bound to the same interface, observing the same frames with
 *    SO_TIMESTAMPING+recvmsg -- SOEM's own socket and nicdrv.c stay
 *    completely untouched. `rt_sample_t.rx_ts_ns` (the RT thread's own
 *    clock_gettime() estimate) is now DIAGNOSTIC ONLY; the real
 *    turnaround measurement comes entirely from turnaround_on_tx_
 *    complete() + turnaround_on_rx_arrival() in the telemetry thread. See
 *    giai_doan_4_ke_hoach.md §5.2 for the full reasoning, including why
 *    the passive socket must filter out PACKET_OUTGOING (its own echo of
 *    what was just sent) and only trust PACKET_HOST arrivals.
 *
 * 4. Five-thread skeleton per master_plan_v2.md §2.4: this file's main()
 *    IS the RT thread (SCHED_FIFO 80, pinned to the isolated core). Four
 *    more threads are spawned: ứng dụng and mailbox are inert stubs (no
 *    logic until Giai doan 5), giám sát is a lightweight ~100ms
 *    placeholder, and telemetry is the real consumer of both rings above.
 *    giám sát and telemetry are deliberately SEPARATE threads (not merged,
 *    as the very first Giai doan 4 draft did) -- see the Giai doan 4
 *    review note on why merging them risks head-of-line blocking once
 *    Giai doan 9 diagnostics add real SDO/mailbox calls to giám sát.
 *
 * ---- Giai doan 5 additions on top of the Giai doan 4 file ----
 *
 * 5. mailbox_thread_fn() is no longer an inert stub: it now runs
 *    ecm_mailbox_run() (libecmaster/mailbox/ecm_mailbox.c), built on
 *    SOEM's *cyclic* mailbox handler (ecx_slavembxcyclic/ecx_mbxhandler),
 *    confirmed by reading ec_main.c/ec_coe.c directly: ecx_SDOread()/
 *    ecx_SDOwrite() never touch ctx.port once a slave is in cyclic
 *    mode -- they only push/pop SOEM's own PI-mutex protected queue
 *    (osal_mutex_create() sets PTHREAD_PRIO_INHERIT, confirmed in
 *    osal.c) and block the calling thread. The actual FPWR/FPRD socket
 *    I/O for pending mailbox traffic happens exclusively on the RT
 *    thread, once per group per cycle, via the new
 *    ecm_mailbox_rt_pump_group() call inside service_group() -- placed
 *    BEFORE t1 is captured so its cost lands inside cycle_occupancy_ns
 *    (quantity #4), on purpose: L4-03 needs a histogram comparison
 *    with vs without SDO traffic in flight, not an eyeball check.
 *
 *    g_mbx is created and ecm_mailbox_enable_cyclic() is called once
 *    every slave is >= PRE_OP, before SAFEOP is requested and well
 *    before the mailbox thread is spawned further below.
 *
 *    KNOWN, DELIBERATE LIMITATION (documented, not silently ignored):
 *    SOEM's context->elist (ecx_pusherror/ecx_poperror) is a plain,
 *    unlocked ring that can be written both from the RT thread
 *    (ecx_mbxhandler -> ecx_mbxinhandler -> ecx_mbxerror/
 *    emergencyerror) and from the mailbox thread (ecx_SDOread/write ->
 *    ecx_SDOerror). This module never calls ecx_poperror()/
 *    ecx_iserror() itself to avoid adding a second concurrent reader;
 *    draining elist for decoded Abort codes is left to exactly one
 *    thread project-wide (monitor_thread_fn, once its Giai doan 9 scope
 *    lands) -- see ecm_mailbox.c's execute_job() comment.
 *
 * ---- Giai doan 6 additions on top of the Giai doan 5 file ----
 *
 * 6. Distributed Clocks, master-shift (libecmaster/core/ecm_dc.c):
 *    DC(a) ecx_configdc() + ecx_dcsync0() on every GROUP_MOTION slave
 *    (SYNC0 = motion cycle, CyclShift 0), placed right after the explicit
 *    SM watchdog writes: same category of blocking pre-RT frames, still
 *    before any thread exists and before SAFEOP (real slaves check the
 *    SYNC configuration on PREOP->SAFEOP). GROUP_IO slaves stay free-run
 *    (no SYNC0): their clocks are still disciplined by the FRMW that
 *    travels in the motion frame, but an 8 ms SYNC0 would need the IO
 *    round-robin tick aligned to the DC grid -- deliberately out of scope.
 *    DC(b) each motion tick: ecm_dc_update(ctx.DCtime, t_wake) returns an
 *    adjustment added to the next absolute deadline. wake_jitter keeps
 *    its meaning because it is measured against that adjusted deadline.
 *    Anchor = ONE FPRD 0x0910..0x0997 on the reference clock.
 *    DC telemetry is RT-thread-only state (no ring change): |e| histogram
 *    and sums read after the loop exits, plus 4 fields in the per-second
 *    snapshot -> same "no I/O in the hot loop" rule as Giai doan 3.
 *    Enabled automatically when slave 1 is DC-capable; --no-dc forces off.
 *
 * Everything below this point that is unchanged from Giai doan 4 keeps its
 * original comments; only the file header above and the new/changed code
 * sections are new for Giai doan 5/6.
 * ========================================================================== */

/* Must come before any system header — CPU_ZERO/CPU_SET/sched_setaffinity
 * are only declared by glibc's <sched.h> when _GNU_SOURCE is defined. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>

#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <arpa/inet.h>

#include "soem/soem.h"

#include <sys/mman.h>

#include "libecmaster/telemetry/ecm_groups.h"
#include "libecmaster/telemetry/ring_spsc.h"
#include "libecmaster/telemetry/turnaround.h"
#include "libecmaster/telemetry/histogram.h"
#include "libecmaster/mailbox/ecm_mailbox.h"
#include "libecmaster/core/ecm_dc.h"

#define IOMAP_SIZE   (256 * 1024)   /* see IOMAP_SIZE note in the Giai doan 3 file header */

static uint8 IOmap_motion[IOMAP_SIZE];
static uint8 IOmap_io[IOMAP_SIZE];
static ecx_contextt ctx;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ---- Giai doan 4: telemetry plumbing, global so both the RT thread and
 * the telemetry thread can reach them. The RT thread only ever PUSHES;
 * the telemetry thread only ever POPS -- this is the single-producer/
 * single-consumer contract both ring types are built around. ---- */
static ring_spsc_t     g_ring;       /* rt_sample_t, one push per tick */
static tx_order_ring_t g_tx_order;   /* one push per sendto() call (1 or 2 per tick) */

static volatile sig_atomic_t g_telemetry_stop = 0;
static volatile sig_atomic_t g_app_stop       = 0;
static volatile sig_atomic_t g_mailbox_stop   = 0;
static volatile sig_atomic_t g_monitor_stop   = 0;

/* ---- Giai doan 5: mailbox subsystem, created in main() once ctx is
 * initialized, used by mailbox_thread_fn() below and by the RT loop
 * (service_group()) via ecm_mailbox_rt_pump_group(). Not touched by
 * any other thread. ---- */
static ecm_mailbox_t *g_mbx = NULL;

/* ---- Giai doan 6: DC(b). g_dc and g_dcstat are written ONLY by the RT
 * thread once the loop starts; main() reads them only after the loop has
 * exited (same contract as the motion/io group_stats_t). ---- */
#define DC_HIST_US 1001            /* |e| bins of 1 us, last bin = overflow */
static int      g_dc_enabled = 0;
static ecm_dc_t g_dc;
static int64_t  g_dc_sum_u_all;    /* all samples, for per-second drift in snapshots */
static struct {
    uint64_t hist[DC_HIST_US];
    uint64_t n;                    /* samples after settle */
    int64_t  sum_e, sum_u;
    uint64_t wrap_n; int64_t wrap_sum_e, wrap_emax;
    uint64_t settle_ticks;
} g_dcstat;

/* Opened in main(), BEFORE any thread is created -- see the Giai doan 4
 * post-mortem note in giai_doan_4_ke_hoach.md §5.2: opening this inside
 * telemetry_thread_fn() left a startup race (RT thread could start
 * ticking, and the very first replies could already have come and gone,
 * before the telemetry thread got scheduled and finished binding this
 * socket), which permanently desynced pending_tx/pending_rx by a
 * constant number of ticks for the rest of the run -- observed as
 * turnaround ~= 5-7ms, impossibly larger than cycle_occupancy's own max
 * of ~98us. Opening it here, before pthread_create(), closes that window
 * entirely: it is guaranteed bound before the RT loop ever sends a frame. */
static int g_passive_rx_fd = -1;

typedef struct {
    ecm_hist_t       wake_jitter_hist;
    ecm_hist_t       prep_send_hist;
    ecm_hist_t       occupancy_hist;
    ecm_hist_t       turnaround_hist;
    turnaround_ctx_t turnaround;
} telemetry_state_t;

static telemetry_state_t g_telemetry;

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

/* Made file-scope (not local to main()) so telemetry_thread_fn's final
 * reconcile print can read their .cycles totals -- safe to read there
 * without synchronization because telemetry only reads them in its own
 * shutdown path, which main() only triggers (g_telemetry_stop = 1) AFTER
 * the RT loop has already exited and stopped writing to them. */
static group_stats_t motion;
static group_stats_t io;


/* One snapshot per second of wall-clock progress, written with NO I/O
 * inside the hot loop -- see the Giai doan 3 file header note on why this
 * exists. */
typedef struct {
    uint64_t tick;
    uint64_t motion_cycles, motion_mismatch, motion_overrun;
    uint64_t io_cycles,     io_mismatch,     io_overrun;
    /* Giai doan 6 */
    int      dc_state;             /* ecm_dc_state_t */
    int64_t  dc_err_ns;            /* last phase error */
    int64_t  dc_sum_u;             /* cumulative adjust, all samples */
    uint64_t dc_samples, dc_wraps, dc_unlocks;
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

static uint64_t ts_to_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ULL + (uint64_t)t->tv_nsec;
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
 * NO fprintf/logging here -- this runs inside the RT hot loop.
 *
 * Giai doan 4 additions: pushes (tick, group) into g_tx_order right after
 * sendto() so the telemetry thread can later match the async TX
 * completion; and exposes three timing values via out-params (any of
 * which may be NULL if the caller doesn't need them) so the caller can
 * build this tick's rt_sample_t without duplicating the timing logic.
 * wkc_mismatch/overrun accounting is UNCHANGED from Giai doan 3 -- t0 is
 * still captured fresh at the start of this function, exactly as before,
 * so those two stats keep their exact original meaning. */
static void service_group(group_stats_t *g, uint64_t tick,
                           int64_t *out_prep_send_ns, int64_t *out_total_ns,
                           uint64_t *out_rx_ts_ns, int *out_wkc)
{
    struct timespec t0, t_after_send, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ecx_send_processdata_group(&ctx, g->group);
    tx_order_ring_push(&g_tx_order, tick, (ecm_group_id_t)g->group);

    clock_gettime(CLOCK_MONOTONIC, &t_after_send);
    if (out_prep_send_ns) *out_prep_send_ns = ts_diff_ns(&t_after_send, &t0);

    int wkc = ecx_receive_processdata_group(&ctx, g->group, EC_TIMEOUTRET);
    if (out_wkc) *out_wkc = wkc;   /* Giai doan 6: DC(b) only trusts ctx.DCtime if the frame came back */

    /* Giai doan 5: pump any pending mailbox I/O queued for this group
     * (at most ECM_MBX_LIMIT_PER_CYCLE jobs -- see ecm_mailbox.h).
     * Placed BEFORE t1 is captured so its cost is included in
     * cycle_occupancy_ns (quantity #4) on purpose: L4-03 needs to
     * compare occupancy with vs without SDO traffic in flight by
     * number, from this exact histogram, not by eye.
     *
     * mbx_received/mbx_sent each real ecx_FPRD/ecx_FPWR this triggers
     * are genuine extra sendto() calls on the wire, exactly like the
     * one ecx_send_processdata_group() just made above -- so each one
     * needs its own tx_order_ring_push() too, in the same order they
     * actually go out (received/FPRD before sent/FPWR, matching
     * ecm_mailbox_rt_pump_group()'s own call order). Missing this was
     * confirmed (by running with real CoE traffic once soft_bus could
     * actually answer) to desync turnaround's passive-observer FIFO
     * match -- "implausible" spiked from 0 to 205 the moment mailbox
     * frames started really hitting the wire; back to 0 after this fix. */
    int mbx_received = 0, mbx_sent = 0;
    ecm_mailbox_rt_pump_group(&ctx, g->group, &mbx_received, &mbx_sent);
    for (int i = 0; i < mbx_received; i++) {
        tx_order_ring_push(&g_tx_order, tick, (ecm_group_id_t)g->group);
    }
    for (int i = 0; i < mbx_sent; i++) {
        tx_order_ring_push(&g_tx_order, tick, (ecm_group_id_t)g->group);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t elapsed_ns = ts_diff_ns(&t1, &t0);
    if (out_total_ns)  *out_total_ns  = elapsed_ns;
    if (out_rx_ts_ns)  *out_rx_ts_ns  = ts_to_ns(&t1);   /* PLACEHOLDER -- see file header point 3 */

    g->cycles++;
    if (wkc != g->expected_wkc) g->wkc_mismatch++;
    if (elapsed_ns > g->cycle_ns) g->overrun++;
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
            " | io: cycles=%" PRIu64 " mismatch=%" PRIu64 " overrun=%" PRIu64,
            s->tick, s->motion_cycles, s->motion_mismatch, s->motion_overrun,
            s->io_cycles, s->io_mismatch, s->io_overrun);
        if (g_dc_enabled) {
            /* drift over the last second from the mean adjustment (the
             * instantaneous integrator is too noisy, see ecm_dc.h) */
            double drift_ppb = 0.0;
            if (i > 0 && s->dc_samples > g_snapshots[i - 1].dc_samples) {
                uint64_t dn = s->dc_samples - g_snapshots[i - 1].dc_samples;
                int64_t  du = s->dc_sum_u   - g_snapshots[i - 1].dc_sum_u;
                drift_ppb = -(double)du / (double)dn / (double)motion.cycle_ns * 1e9;
            }
            fprintf(stderr, " | dc: %s e=%+.1fus drift=%+.0fppb wraps=%" PRIu64 " unlocks=%" PRIu64,
                    s->dc_state == ECM_DC_LOCKED ? "LOCK" : s->dc_state == ECM_DC_ACQUIRE ? "ACQ" : "--",
                    s->dc_err_ns / 1000.0, drift_ppb, s->dc_wraps, s->dc_unlocks);
        }
        fputc('\n', stderr);
    }
}

/* ==========================================================================
 * Giai doan 4: the four non-RT threads.
 *
 * IMPORTANT correctness note: pthread_create() defaults to
 * PTHREAD_INHERIT_SCHED, meaning a new thread inherits the CREATING
 * thread's scheduling policy/priority. Since main() (the RT thread) will
 * be running under SCHED_FIFO 80 by the time these are spawned, every one
 * of these threads MUST explicitly switch itself to SCHED_OTHER on entry,
 * or it would silently keep SCHED_FIFO 80 -- exactly the kind of
 * mis-priority bug that would surface much later as an unexplained
 * latency spike. isolcpus=3 (Giai doan 1) already keeps these threads off
 * the isolated core by default, so no explicit CPU affinity is set here.
 * ========================================================================== */

static void set_non_rt_thread(void)
{
    struct sched_param sp = { .sched_priority = 0 };
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) != 0) {
        perror("pthread_setschedparam(SCHED_OTHER)");
    }
}

/* Enables TX software timestamping on `fd` -- ts[0] in the SO_TIMESTAMPING
 * cmsg, not ts[2] (hardware) like hw_ts_test.c uses on enP1p1s0. This
 * matches option (c) chosen in giai_doan_4_ke_hoach.md §2: veth has no
 * PHY, so only SOF_TIMESTAMPING_*_SOFTWARE is available here. Swapping to
 * hardware timestamps later (real LAN9252 link) means adding the
 * SOF_TIMESTAMPING_TX_HARDWARE/RAW_HARDWARE flags and reading ts[2]
 * instead -- the rest of this file does not need to change. */
static int enable_tx_sw_timestamping(int fd)
{
    int flags = SOF_TIMESTAMPING_TX_SOFTWARE
              | SOF_TIMESTAMPING_SOFTWARE;
    return setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
}

/* ---- Giai doan 4, RX side (option (b), confirmed 22/9): a SEPARATE,
 * passive AF_PACKET socket bound to the same interface, used only to
 * observe genuine incoming EtherCAT frames with SO_TIMESTAMPING -- this
 * keeps nicdrv.c (which reads with plain recv(), no cmsg) completely
 * untouched. Reading oshw/linux/nicdrv.c directly confirmed ecx_recvpkt()
 * cannot supply a real RX timestamp itself, so this is the deliberate
 * workaround, not a guess. See giai_doan_4_ke_hoach.md §5.2. */
static const char *g_ifname = NULL;   /* set once in main(), read-only after that */

static int open_passive_rx_socket(const char *ifname)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(0x88A4 /* ETH_P_ECAT */));
    if (fd < 0) { perror("open_passive_rx_socket: socket"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("open_passive_rx_socket: SIOCGIFINDEX");
        close(fd); return -1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(0x88A4);
    sll.sll_ifindex  = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("open_passive_rx_socket: bind");
        close(fd); return -1;
    }

    /* RX software timestamping only -- this socket never sends, so no TX
     * flags are needed here (TX is handled separately via soem_raw_fd). */
    int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
        perror("open_passive_rx_socket: SO_TIMESTAMPING");
        close(fd); return -1;
    }
    return fd;
}

static void *telemetry_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();

    hist_init(&g_telemetry.wake_jitter_hist);
    hist_init(&g_telemetry.prep_send_hist);
    hist_init(&g_telemetry.occupancy_hist);
    hist_init(&g_telemetry.turnaround_hist);
    turnaround_init(&g_telemetry.turnaround);

    /* TX side: fully confirmed (22/9) -- ctx.port is an EMBEDDED struct,
     * so this is a dot, not an arrow. See the file header note above. */
    int soem_raw_fd = ctx.port.sockhandle;
    if (enable_tx_sw_timestamping(soem_raw_fd) != 0) {
        perror("enable_tx_sw_timestamping");
        soem_raw_fd = -1;
    }

    /* RX side: option (b), a second independent socket -- already opened
     * in main(), before any thread was created (see g_passive_rx_fd's
     * declaration comment for why that ordering matters). */
    int passive_rx_fd = g_passive_rx_fd;
    if (passive_rx_fd < 0) {
        fprintf(stderr, "telemetry: passive RX socket failed to open -- turnaround will stay incomplete on the RX side.\n");
    }

    int print_counter = 0;

    while (!g_telemetry_stop) {
        rt_sample_t s;
        while (ring_pop(&g_ring, &s) == 0) {
            hist_add(&g_telemetry.wake_jitter_hist, s.wake_jitter_ns);
            hist_add(&g_telemetry.prep_send_hist,   s.prep_send_ns);
            hist_add(&g_telemetry.occupancy_hist,   s.cycle_occupancy_ns);
            /* s.rx_ts_ns is now DIAGNOSTIC ONLY (a CPU clock_gettime()
             * estimate from the RT thread) -- the real turnaround
             * measurement below comes entirely from soem_raw_fd's error
             * queue (TX) and passive_rx_fd (RX), never from this field. */
        }

        turnaround_drain_tx_order(&g_telemetry.turnaround, &g_tx_order);

        if (soem_raw_fd >= 0) {
            struct pollfd pfd = { .fd = soem_raw_fd, .events = 0 };
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLERR)) {
                char control[512], buf[128];
                struct msghdr msg;
                struct iovec  iov = { .iov_base = buf, .iov_len = sizeof(buf) };
                memset(&msg, 0, sizeof(msg));
                msg.msg_iov = &iov; msg.msg_iovlen = 1;
                msg.msg_control = control; msg.msg_controllen = sizeof(control);

                if (recvmsg(soem_raw_fd, &msg, MSG_ERRQUEUE) < 0) break;

                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                        struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
                        uint64_t tx_ts_ns = ts_to_ns(&ts[0]);  /* [0]=software, matches enable_tx_sw_timestamping */
                        turnaround_on_tx_complete(&g_telemetry.turnaround, tx_ts_ns);
                    }
                }
            }
        }

        if (passive_rx_fd >= 0) {
            for (;;) {
                char control[512], buf[1600];
                struct sockaddr_ll from;
                struct msghdr msg;
                struct iovec  iov = { .iov_base = buf, .iov_len = sizeof(buf) };
                memset(&msg, 0, sizeof(msg));
                msg.msg_name = &from; msg.msg_namelen = sizeof(from);
                msg.msg_iov = &iov; msg.msg_iovlen = 1;
                msg.msg_control = control; msg.msg_controllen = sizeof(control);

                ssize_t r = recvmsg(passive_rx_fd, &msg, MSG_DONTWAIT);
                if (r < 0) break;   /* EAGAIN -- nothing more waiting right now */

                /* CRITICAL: this socket sees BOTH directions on the
                 * interface. Only PACKET_HOST is a genuine arrival;
                 * PACKET_OUTGOING is the echo of our own send and must be
                 * skipped, or motion's own frame would match against
                 * itself and report turnaround ~= 0. */
                if (from.sll_pkttype == PACKET_OUTGOING) continue;

                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                        struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
                        uint64_t rx_ts_ns = ts_to_ns(&ts[0]);
                        turnaround_on_rx_arrival(&g_telemetry.turnaround, rx_ts_ns, &g_telemetry.turnaround_hist);
                    }
                }
            }
        }

        if (++print_counter >= 100) {   /* ~once/second at the 10ms poll period below */
            print_counter = 0;
            hist_print(&g_telemetry.wake_jitter_hist, "wake_jitter");
            hist_print(&g_telemetry.prep_send_hist,   "prep_send");
            hist_print(&g_telemetry.occupancy_hist,   "occupancy");
            hist_print(&g_telemetry.turnaround_hist,  "turnaround");
        }

        usleep(10000);  /* 10ms poll period -- fine for a non-RT thread */
    }

    /* Final drain + print so nothing collected right before shutdown is lost. */
    rt_sample_t s;
    while (ring_pop(&g_ring, &s) == 0) {
        hist_add(&g_telemetry.wake_jitter_hist, s.wake_jitter_ns);
        hist_add(&g_telemetry.prep_send_hist,   s.prep_send_ns);
        hist_add(&g_telemetry.occupancy_hist,   s.cycle_occupancy_ns);
    }
    if (passive_rx_fd >= 0) close(passive_rx_fd);
    fprintf(stderr, "\n=== Giai doan 4 telemetry (final) ===\n");
    fprintf(stderr, "  ring drop_count=%" PRIu64 "  turnaround: matched=%" PRIu64
                    " implausible=%" PRIu64 " tx_io_discarded=%" PRIu64 " rx_io_discarded=%" PRIu64
                    " evicted_no_match=%" PRIu64 " completion_no_pending=%" PRIu64
                    " rx_no_pending=%" PRIu64 "\n",
            g_ring.drop_count, g_telemetry.turnaround.stat_matched, g_telemetry.turnaround.stat_implausible,
            g_telemetry.turnaround.stat_tx_io_discarded, g_telemetry.turnaround.stat_rx_io_discarded,
            g_telemetry.turnaround.stat_evicted_no_match, g_telemetry.turnaround.stat_completion_no_pending,
            g_telemetry.turnaround.stat_rx_no_pending);
    fprintf(stderr, "  reconcile TX side: matched+implausible+tx_io_discarded+completion_no_pending = %" PRIu64
                    "  (expect == total sendto() calls = motion.cycles + io.cycles = %" PRIu64 ")\n",
            g_telemetry.turnaround.stat_matched + g_telemetry.turnaround.stat_implausible
                + g_telemetry.turnaround.stat_tx_io_discarded + g_telemetry.turnaround.stat_completion_no_pending,
            motion.cycles + io.cycles);
    fprintf(stderr, "  reconcile RX side: matched+implausible+rx_io_discarded = %" PRIu64
                    "  vs rx_no_pending (excess RX events with nothing pending) = %" PRIu64 "\n",
            g_telemetry.turnaround.stat_matched + g_telemetry.turnaround.stat_implausible
                + g_telemetry.turnaround.stat_rx_io_discarded,
            g_telemetry.turnaround.stat_rx_no_pending);
    hist_print(&g_telemetry.wake_jitter_hist, "wake_jitter");
    hist_print(&g_telemetry.prep_send_hist,   "prep_send");
    hist_print(&g_telemetry.occupancy_hist,   "occupancy");
    hist_print(&g_telemetry.turnaround_hist,  "turnaround");

    return NULL;
}

/* Inert for now -- no CiA402 logic until later platforms. Exists so the
 * 5-thread skeleton matches master_plan_v2.md §2.4 from the start. */
static void *app_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();
    while (!g_app_stop) usleep(100000);
    return NULL;
}

/* Giai doan 5: real mailbox worker. The actual FPWR/FPRD socket I/O for
 * pending SDO/mailbox traffic still happens on the RT thread (see
 * ecm_mailbox_rt_pump_group() inside service_group() below) -- this
 * thread only ever talks to SOEM's PI-mutex-protected cyclic mailbox
 * queue via ecx_SDOread()/ecx_SDOwrite(), never the socket directly.
 * g_mbx is created in main() before this thread is spawned (see the
 * Giai doan 4 thread-ordering note above pthread_create() in main). */
static void *mailbox_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();
    ecm_mailbox_run(g_mbx, &g_mailbox_stop);
    return NULL;
}

/* ~100ms cadence per master_plan_v2.md §2.4. Nothing to poll yet (no
 * CoE/mailbox until Giai doan 5) -- deliberately its OWN thread, separate
 * from telemetry, so that Giai doan 9's future SDO/mailbox calls here
 * (which can block far longer than a 1ms/8ms cycle) can never delay
 * telemetry's ring-draining -- see the Giai doan 4 review note on this. */
static void *monitor_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();
    while (!g_monitor_stop) {
        usleep(100000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int n = 0;
    int motion_slaves = -1;
    long motion_cycle_us = 1000;
    long io_cycle_us     = 8000;
    long duration_sec    = 0; /* 0 = run until Ctrl+C */
    int  no_dc           = 0; /* Giai doan 6 */
    long dc_setpoint_pct = 30;

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
        } else if (strcmp(argv[i], "--no-dc") == 0) {
            no_dc = 1;
        } else if (strcmp(argv[i], "--dc-setpoint-pct") == 0 && i + 1 < argc) {
            dc_setpoint_pct = atol(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (!ifname || n <= 0 || motion_slaves <= 0 || motion_slaves >= n) {
        fprintf(stderr,
            "Usage: %s --iface <veth_m> --n <total_slaves> --motion-slaves <count> "
            "[--motion-cycle-us N] [--io-cycle-us N] [--duration-sec N] "
            "[--no-dc] [--dc-setpoint-pct N]\n", argv[0]);
        return 1;
    }
    if (io_cycle_us % motion_cycle_us != 0) {
        fprintf(stderr,
            "io-cycle-us (%ld) must be a whole multiple of motion-cycle-us (%ld) "
            "for the round-robin tick counter to land exactly.\n", io_cycle_us, motion_cycle_us);
        return 1;
    }
    long ticks_per_io = io_cycle_us / motion_cycle_us;

    g_ifname = ifname;   /* Giai doan 4: telemetry thread's passive RX socket needs this */

    /* Open the passive RX socket HERE, before anything else -- see
     * g_passive_rx_fd's declaration comment for why this must happen
     * before any thread (especially the RT thread) can possibly send a
     * single frame. */
    g_passive_rx_fd = open_passive_rx_socket(g_ifname);
    if (g_passive_rx_fd < 0) {
        fprintf(stderr, "ecm_run: passive RX socket failed to open -- turnaround will stay incomplete on the RX side.\n");
    }

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

    motion = (group_stats_t){
        .label = "GROUP_MOTION", .group = GROUP_MOTION,
        .expected_wkc = (ctx.grouplist[GROUP_MOTION].outputsWKC * 2) + ctx.grouplist[GROUP_MOTION].inputsWKC,
        .cycle_ns = motion_cycle_us * 1000L,
    };
    io = (group_stats_t){
        .label = "GROUP_IO", .group = GROUP_IO,
        .expected_wkc = (ctx.grouplist[GROUP_IO].outputsWKC * 2) + ctx.grouplist[GROUP_IO].inputsWKC,
        .cycle_ns = io_cycle_us * 1000L,
    };
    fprintf(stderr, "ecm_run: expected WKC -- motion=%ld io=%ld\n", motion.expected_wkc, io.expected_wkc);

    /* ---- Bring the whole bus up to SAFEOP ---- */
    if (!request_all_state(EC_STATE_PRE_OP, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach PREOP\n"); ecx_close(&ctx); return 1;
    }

    /* ---- Giai doan 5: enable SOEM's cyclic mailbox handler now that
     * every slave is >= PRE_OP, and BEFORE requesting SAFEOP -- this
     * must happen before the mailbox thread (spawned further below,
     * still well before OP) or the RT thread ever touch mailbox
     * traffic for these slaves. g_mbx itself has no state dependency,
     * but is created here, right next to enable_cyclic(), so both
     * mailbox-related setup steps stay together. ---- */
    g_mbx = ecm_mailbox_create(&ctx);
    if (!g_mbx) {
        fprintf(stderr, "ecm_mailbox_create failed\n"); ecx_close(&ctx); return 1;
    }
    int mbx_enabled = ecm_mailbox_enable_cyclic(&ctx);
    fprintf(stderr, "ecm_run: cyclic mailbox enabled on %d/%d slave(s)\n", mbx_enabled, n);

    /* Giai doan 5: workaround for a length-computation asymmetry in
     * SOEM's ecx_config_map_group() (ec_config.c), confirmed by gdb +
     * reading the source directly: Obytes and Ibytes are each computed
     * as (LogAddr - logstartaddr), but mbxstatuslength is computed as
     * (LogAddr - Obytes - Ibytes), missing that same "- logstartaddr"
     * term. For any group whose logical address space does not start
     * at 0 (this build assigns logstartaddr = group_index * 0x10000),
     * mbxstatuslength ends up inflated by exactly logstartaddr, which
     * then makes ecx_mbxinhandler() index past mbxstatuslookup[]'s
     * real bounds and segfault. Applying the missing subtraction here
     * rather than patching ec_config.c itself. No-op for any group
     * whose logstartaddr is already 0 (e.g. GROUP_MOTION if it happens
     * to be group 0). */
    ctx.grouplist[GROUP_MOTION].mbxstatuslength -= ctx.grouplist[GROUP_MOTION].logstartaddr;
    ctx.grouplist[GROUP_IO].mbxstatuslength     -= ctx.grouplist[GROUP_IO].logstartaddr;

    /* ---- Giai doan 5: SM watchdog -- written explicitly, never left at
     * whatever the slave's power-on default happens to be (roadmap
     * requirement: "SM watchdog cấu hình tường minh"). Two registers per
     * slave (Section II §2.10 / ETG.1000-4):
     *   0x0400 Watchdog Divider    -- base tick = (Divider+2) * 40ns
     *   0x0420 Watchdog Time (process data) -- actual timeout =
     *          Time * base_tick; trips if no valid output-SM write
     *          (LWR) lands within this window since the last one.
     * (0x0410, "PDI Watchdog", is a different, PDI-local heartbeat --
     * not written here, not relevant to EtherCAT process data.)
     *
     * Values mirror tools/soft_bus/esc_types.h's own register map
     * (REG_WD_DIVIDER/REG_WD_TIME_PROCDATA), which cites the same
     * datasheet section -- kept as local constants here rather than
     * including that slave-simulator header from the master, which
     * also has to run against real hardware later.
     *
     * Divider shared by every slave: 2498 -> (2498+2)*40ns = 100us base
     * tick, a common, easy-to-reason-about unit.
     *
     * Watchdog Time is chosen PER GROUP, not one value for the whole
     * bus, because GROUP_MOTION slaves get a fresh output write every
     * 1ms while GROUP_IO slaves only get one every 8ms (see the main
     * cyclic loop below) -- a single shared value would either trip
     * GROUP_IO under completely normal operation (if sized for 1ms) or
     * take far too long to react to a real master failure for
     * GROUP_MOTION (if sized for 8ms):
     *   GROUP_MOTION: 30  * 100us = 3ms  (3x its 1ms nominal cycle --
     *     comfortably above every occupancy/wake_jitter figure measured
     *     in Giai doan 4/5 so far, worst case ~232us, yet trips within
     *     3ms of an actual master hang)
     *   GROUP_IO:     240 * 100us = 24ms (3x its 8ms nominal cycle,
     *     same reasoning)
     *
     * NOTE: soft_bus accepts these writes (generic register space, no
     * special handling needed) but does not yet ACT on the watchdog
     * itself (no trip/countdown behavior simulated) -- out of scope
     * for this pass. DoD for this step is "cấu hình tường minh, test
     * được bằng tshark", not "trip behavior simulated end to end". ---- */
#define REG_SM_WD_DIVIDER     0x0400u
#define REG_SM_WD_TIME_PROCDATA 0x0420u
#define WD_DIVIDER_VALUE        2498u  /* -> 100us base tick */
#define WD_TIME_MOTION_TICKS      30u  /* -> 3ms  (3x GROUP_MOTION's 1ms cycle) */
#define WD_TIME_IO_TICKS         240u  /* -> 24ms (3x GROUP_IO's 8ms cycle) */

    int wd_fail = 0;
    for (int slave = 1; slave <= ctx.slavecount; slave++) {
        uint16_t divider = (uint16_t)WD_DIVIDER_VALUE;
        uint16_t wd_time = (uint16_t)((ctx.slavelist[slave].group == GROUP_IO)
                                       ? WD_TIME_IO_TICKS : WD_TIME_MOTION_TICKS);
        uint16_t configadr = ctx.slavelist[slave].configadr;

        if (ecx_FPWR(&ctx.port, configadr, REG_SM_WD_DIVIDER,
                      sizeof(divider), &divider, EC_TIMEOUTRET) <= 0) {
            wd_fail++;
        }
        if (ecx_FPWR(&ctx.port, configadr, REG_SM_WD_TIME_PROCDATA,
                      sizeof(wd_time), &wd_time, EC_TIMEOUTRET) <= 0) {
            wd_fail++;
        }
    }
    fprintf(stderr,
        "ecm_run: SM watchdog configured explicitly (divider=%u -> 100us tick; "
        "motion=%ums io=%ums)%s\n",
        (unsigned)WD_DIVIDER_VALUE,
        (unsigned)(WD_TIME_MOTION_TICKS / 10), (unsigned)(WD_TIME_IO_TICKS / 10),
        wd_fail ? " -- WARNING: one or more FPWR failed, see wd_fail count in source" : "");
    if (wd_fail) {
        fprintf(stderr, "ecm_run: SM watchdog FPWR failures: %d\n", wd_fail);
    }

    /* ---- Giai doan 6: Distributed Clocks, DC(a). Blocking frames, so
     * they go here with the SM watchdog writes: before any thread exists
     * (Giai doan 5 lesson: a blocking frame from another thread while the
     * RT loop runs desyncs g_tx_order), and in PREOP, before SAFEOP. ---- */
    if (!no_dc) {
        ecx_configdc(&ctx);
        uint16_t ref = ctx.grouplist[GROUP_MOTION].DCnext;

        if (!ctx.slavelist[1].hasdc) {
            fprintf(stderr, "ecm_run: DC off -- slave 1 is not DC-capable (0x0008 bit2 = 0)\n");
        } else if (!ctx.grouplist[GROUP_MOTION].hasdc || ctx.grouplist[GROUP_IO].hasdc || ref != 1) {
            /* ecx_configdc() gives the FRMW datagram only to the group that
             * owns the FIRST DC slave. If that is not GROUP_MOTION the PI
             * would get a sample only every io cycle -- refuse. */
            fprintf(stderr, "ecm_run: DC off -- FRMW not owned by GROUP_MOTION "
                    "(motion.hasdc=%d io.hasdc=%d DCnext=%u)\n",
                    ctx.grouplist[GROUP_MOTION].hasdc, ctx.grouplist[GROUP_IO].hasdc, ref);
        } else {
            int64_t cyc = motion_cycle_us * 1000L;
            int sync0_n = 0;
            for (int s = 1; s <= ctx.slavecount; s++) {
                fprintf(stderr, "ecm_run: dc slave %d hasdc=%d pdelay=%d ns%s\n", s,
                        ctx.slavelist[s].hasdc, ctx.slavelist[s].pdelay,
                        ctx.slavelist[s].group == GROUP_MOTION ? " SYNC0" : "");
                if (ctx.slavelist[s].group == GROUP_MOTION && ctx.slavelist[s].hasdc) {
                    ecx_dcsync0(&ctx, (uint16)s, TRUE, (uint32)cyc, 0);
                    sync0_n++;
                }
            }
            /* anchor: 0x0910 (system time) and 0x0990 (next SYNC0) of the
             * reference clock in ONE datagram, so both belong to one instant */
            uint8_t snap[0x88];
            memset(snap, 0, sizeof(snap));
            int w = ecx_FPRD(&ctx.port, ctx.slavelist[ref].configadr, ECT_REG_DCSYSTIME,
                             sizeof(snap), snap, EC_TIMEOUTRET);
            struct timespec t_anchor;
            clock_gettime(CLOCK_MONOTONIC, &t_anchor);
            uint64_t dc_raw = 0, s0_raw = 0;
            for (int b = 7; b >= 0; b--) {
                dc_raw = (dc_raw << 8) | snap[b];
                s0_raw = (s0_raw << 8) | snap[0x80 + b];
            }
            int32_t lead = (int32_t)((uint32_t)s0_raw - (uint32_t)dc_raw);
            if (w != 1 || lead <= 0) {
                fprintf(stderr, "ecm_run: DC off -- anchor read failed (wkc=%d lead=%d ns)\n", w, lead);
            } else {
                ecm_dc_cfg_t dcfg;
                ecm_dc_default_cfg(&dcfg, cyc, cyc * dc_setpoint_pct / 100);
                ecm_dc_init(&g_dc, &dcfg);
                ecm_dc_anchor(&g_dc, dc_raw, s0_raw, ts_to_ns(&t_anchor));
                g_dc_enabled = 1;
                fprintf(stderr, "ecm_run: DC on -- ref=slave %u, SYNC0 %lld ns on %d motion slave(s), "
                        "setpoint %ld%% after SYNC0, first SYNC0 in %.1f ms\n",
                        ref, (long long)cyc, sync0_n, dc_setpoint_pct, lead / 1e6);
            }
        }
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

    /* ---- Giai doan 4: bring up the two rings and the four non-RT threads
     * BEFORE this thread switches itself to SCHED_FIFO below. Order
     * matters: pthread_create() inherits the creating thread's scheduling
     * policy at the moment of creation (PTHREAD_INHERIT_SCHED default), so
     * spawning these while main() is still SCHED_OTHER means they start
     * SCHED_OTHER too -- each one also calls set_non_rt_thread() itself as
     * a second, explicit safeguard, in case this ordering ever changes. */
    ring_init(&g_ring);
    tx_order_ring_init(&g_tx_order);

    pthread_t telemetry_tid, app_tid, mailbox_tid, monitor_tid;
    pthread_create(&telemetry_tid, NULL, telemetry_thread_fn, NULL);
    pthread_create(&app_tid,       NULL, app_thread_fn,       NULL);
    pthread_create(&mailbox_tid,   NULL, mailbox_thread_fn,   NULL);
    pthread_create(&monitor_tid,   NULL, monitor_thread_fn,   NULL);

    /* ---- NOW switch this thread (the RT thread) to SCHED_FIFO 80,
     * pinned to the isolated core (core 3, isolcpus=3 from Giai doan 1).
     * Requires cap_sys_nice, already granted by `make setcap`. ---- */
    {
        struct sched_param sp = { .sched_priority = 80 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            perror("sched_setscheduler(SCHED_FIFO, 80)");
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
            perror("sched_setaffinity(core 3)");
        }
    }

    fprintf(stderr, "ecm_run: all slaves in OPERATIONAL. Starting cyclic loop (base tick = %ld us, IO every %ld ticks).\n",
            motion_cycle_us, ticks_per_io);
    fprintf(stderr, "ecm_run: no further stderr output from the RT thread until the loop ends -- see the Giai doan 3 file header for why.\n");

    signal(SIGINT, on_sigint);

    struct timespec next, start;
    clock_gettime(CLOCK_MONOTONIC, &next);
    start = next;
    uint64_t tick = 0;
    g_dcstat.settle_ticks = (uint64_t)(5000000L / motion_cycle_us);   /* Giai doan 6: DC stats skip first 5 s */

    /* Ticks-per-second, used only to decide when to record a snapshot --
     * still no I/O happens as a result, just an array write. */
    long ticks_per_snapshot = (motion_cycle_us > 0) ? (1000000L / motion_cycle_us) : 1;
    if (ticks_per_snapshot < 1) ticks_per_snapshot = 1;

    while (!g_stop) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        struct timespec t_wake;
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        int64_t wake_jitter_ns = ts_diff_ns(&t_wake, &next);   /* quantity #1 */

        int64_t  motion_prep_send_ns = 0, motion_total_ns = 0;
        uint64_t motion_rx_ts_ns     = 0;
        int motion_wkc = 0;
        service_group(&motion, tick, &motion_prep_send_ns, &motion_total_ns, &motion_rx_ts_ns, &motion_wkc);

        int64_t cycle_occupancy_ns = motion_total_ns;   /* quantity #4, starts with motion's own cost */
        uint8_t io_due = (tick % (uint64_t)ticks_per_io == 0) ? 1 : 0;
        uint8_t any_wkc_mismatch = 0;

        if (io_due) {
            uint64_t before_mismatch = io.wkc_mismatch;
            int64_t io_total_ns = 0;
            service_group(&io, tick, NULL, &io_total_ns, NULL, NULL);
            cycle_occupancy_ns += io_total_ns;  /* io's work this tick also counts toward total occupancy */
            if (io.wkc_mismatch != before_mismatch) any_wkc_mismatch = 1;
        }
        {
            static uint64_t motion_mismatch_before;
            if (motion.wkc_mismatch != motion_mismatch_before) any_wkc_mismatch = 1;
            motion_mismatch_before = motion.wkc_mismatch;
        }

        /* ---- Giai doan 6: DC(b). ctx.DCtime was refreshed by the motion
         * receive (only GROUP_MOTION carries the FRMW), so read it before
         * anything else can send. Pure arithmetic, no syscalls. ---- */
        int64_t dc_adjust_ns = 0;
        if (g_dc_enabled && motion_wkc > 0) {
            uint64_t wraps_before = g_dc.wraps;
            dc_adjust_ns = ecm_dc_update(&g_dc, (uint64_t)ctx.DCtime, ts_to_ns(&t_wake));
            g_dc_sum_u_all += dc_adjust_ns;
            static uint64_t since_wrap = 1000;
            if (g_dc.wraps != wraps_before) since_wrap = 0;
            if (tick >= g_dcstat.settle_ticks) {
                int64_t e = g_dc.err_ns, ae = e < 0 ? -e : e;
                uint64_t b = (uint64_t)(ae / 1000);
                if (b >= DC_HIST_US) b = DC_HIST_US - 1;
                g_dcstat.hist[b]++;
                g_dcstat.n++;
                g_dcstat.sum_e += e;
                g_dcstat.sum_u += dc_adjust_ns;
                if (since_wrap <= 3) {
                    g_dcstat.wrap_n++; g_dcstat.wrap_sum_e += e;
                    if (ae > g_dcstat.wrap_emax) g_dcstat.wrap_emax = ae;
                }
            }
            since_wrap++;
        }

        rt_sample_t s = {
            .wake_jitter_ns     = wake_jitter_ns,
            .prep_send_ns       = motion_prep_send_ns,
            .cycle_occupancy_ns = cycle_occupancy_ns,
            .rx_ts_ns           = motion_rx_ts_ns,   /* PLACEHOLDER -- see file header point 3 */
            .tick               = tick,
            .sent_io_group      = io_due,
            .any_wkc_mismatch   = any_wkc_mismatch,
        };
        ring_push(&g_ring, &s);   /* RT thread: never blocks; drop_count absorbs a full ring */

        tick++;

        if (tick % (uint64_t)ticks_per_snapshot == 0 && g_snapshot_count < MAX_SNAPSHOTS) {
            g_snapshots[g_snapshot_count++] = (snapshot_t){
                .tick = tick,
                .motion_cycles = motion.cycles, .motion_mismatch = motion.wkc_mismatch, .motion_overrun = motion.overrun,
                .io_cycles     = io.cycles,     .io_mismatch     = io.wkc_mismatch,     .io_overrun     = io.overrun,
                .dc_state = (int)g_dc.state, .dc_err_ns = g_dc.err_ns,
                .dc_sum_u = g_dc_sum_u_all, .dc_samples = g_dc.samples,
                .dc_wraps = g_dc.wraps, .dc_unlocks = g_dc.unlocks,
            };
        }

        /* Giai doan 6: |dc_adjust_ns| <= cycle/20, so the step stays
         * positive and ts_add_ns()'s carry-only normalisation is enough. */
        ts_add_ns(&next, motion_cycle_us * 1000L + dc_adjust_ns);

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

    /* ---- Giai doan 6: DC(b) summary (RT loop has exited) ---- */
    if (g_dc_enabled) {
        uint64_t nn = g_dcstat.n, want[4], acc = 0; int k = 0;
        const double pq[4] = { 0.50, 0.99, 0.999, 0.9999 };
        int64_t pv[4] = { 0, 0, 0, 0 };
        for (int j = 0; j < 4; j++) want[j] = (uint64_t)(pq[j] * (double)nn);
        for (int b = 0; b < DC_HIST_US && k < 4; b++) {
            acc += g_dcstat.hist[b];
            while (k < 4 && acc > want[k]) pv[k++] = b;
        }
        fprintf(stderr,
            "  [DC] state=%s locks=%" PRIu64 " unlocks=%" PRIu64 " clamps=%" PRIu64
            " stale=%" PRIu64 " wraps=%" PRIu64 "\n"
            "  [DC] |e| (after 5 s) p50=%lld p99=%lld p99.9=%lld p99.99=%lld us, mean e=%+.2f us\n"
            "  [DC] drift ref vs master (mean adjust) = %+.0f ppb\n"
            "  [DC] 4 samples after each wrap: n=%" PRIu64 " mean e=%+.2f us max|e|=%.1f us\n",
            g_dc.state == ECM_DC_LOCKED ? "LOCKED" : g_dc.state == ECM_DC_ACQUIRE ? "ACQUIRE" : "UNANCHORED",
            g_dc.locks, g_dc.unlocks, g_dc.clamps, g_dc.stale, g_dc.wraps,
            (long long)pv[0], (long long)pv[1], (long long)pv[2], (long long)pv[3],
            nn ? (double)g_dcstat.sum_e / (double)nn / 1000.0 : 0.0,
            nn ? -(double)g_dcstat.sum_u / (double)nn / (double)motion.cycle_ns * 1e9 : 0.0,
            g_dcstat.wrap_n,
            g_dcstat.wrap_n ? (double)g_dcstat.wrap_sum_e / (double)g_dcstat.wrap_n / 1000.0 : 0.0,
            g_dcstat.wrap_emax / 1000.0);
    } else {
        fprintf(stderr, "  [DC] disabled\n");
    }

    /* ---- Giai doan 4: stop and join the four non-RT threads before
     * tearing down the bus, so telemetry's final drain (inside
     * telemetry_thread_fn, right before it returns) sees every sample
     * this run produced. ---- */
    g_telemetry_stop = 1;
    g_app_stop       = 1;
    g_mailbox_stop   = 1;
    g_monitor_stop   = 1;
    pthread_join(telemetry_tid, NULL);
    pthread_join(app_tid,       NULL);
    pthread_join(mailbox_tid,   NULL);
    pthread_join(monitor_tid,   NULL);

    /* Giai doan 5: only after the mailbox thread has actually returned
     * from ecm_mailbox_run() (guaranteed by the join right above) --
     * destroying g_mbx any earlier could free the job queue out from
     * under a job still in flight. */
    ecm_mailbox_destroy(g_mbx);

    request_all_state(EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&ctx);
    return 0;
}