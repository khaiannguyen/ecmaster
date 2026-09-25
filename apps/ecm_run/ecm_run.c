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
#include "libecmaster/diag/ecm_diag.h"       /* Giai doan 7.2 */

/* ctx.grouplist[] has EC_MAXGROUP entries (SOEM CMake option, default 2)
 * and this file indexes it with GROUP_IO = 2. With the default, every
 * grouplist[GROUP_IO] access is out of bounds: found in the Phase 7 sandbox
 * (IO group mapped 0 bytes, expected WKC 0). Build SOEM with
 * -DEC_MAXGROUP=4 (or more). */
#if EC_MAXGROUP <= GROUP_IO
#error "SOEM built with EC_MAXGROUP <= GROUP_IO: rebuild SOEM with cmake -DEC_MAXGROUP=4"
#endif
#include "libecmaster/diag/ecm_diag_soem.h"
#include "libecmaster/policy/ecm_policy.h"    /* Giai doan 7.3 */
#include <stdatomic.h>

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

/* ---- Giai doan 7.2: diagnostics (plan §3.1/§3.2) ----
 * RT thread: sends one diagnostic frame per second and collects the reply
 * on a later tick (ecm_diag_soem_collect never waits), then hands the raw
 * read to the monitor thread. Monitor: accumulates, analyses, writes the
 * snapshot file that `ecm_diag` prints, and reports finding changes. */
static int                g_diag_enabled = 1;
static int                g_no_tx_ts     = 0;   /* --no-tx-ts, see telemetry_thread_fn */
static const char        *g_diag_path    = "/tmp/ecm_diag.txt";
static ecm_diag_io_t      g_diag_io;       /* RT thread only */
static ecm_diag_raw_t     g_diag_raw_rt;   /* RT thread only */
static ecm_diag_handoff_t g_diag_ho;       /* RT -> monitor  */
static ecm_diag_t         g_diag;          /* monitor only (and main() after join) */
#define NON_RT_STACK_BYTES (256u * 1024u)  /* see the pthread_create() block in main() */
#define DIAG_MAX_AGE_TICKS 5              /* give a diag reply 5 ticks, then count it lost */

/* ---- Giai doan 7.3: fault policy (docs/fault_policy.md) ----
 * RT thread owns g_bus (state machine) and executes g_cmdq; the monitor
 * owns g_srec (per-slave planner), drains g_ev, and runs RECOVER and
 * recovery path B. Ownership of the bus during RECOVER is handed over with
 * g_recover_req/g_recover_done (release/acquire): the RT thread sends
 * nothing between publishing a request and seeing the matching done. */
#define RX_GUARD_NS      150000            /* §2: rest of the tick after the receives */
#define RX_MIN_US        50
#define CMD_MIN_BUDGET_US 100              /* a recovery command needs this much left in the tick */
#define RECOVER_OP_TIMEOUT_US 3000000
static int              g_rx_legacy      = 0;  /* --rx-timeout-legacy: EC_TIMEOUTRET (negative control) */
static int              g_recover_enabled = 1; /* --no-recover: detect and report, never act */
static long             g_motion_cycle_us = 1000;
static ecm_bus_fsm_t    g_bus;                 /* RT only */
static uint64_t         g_tick_deadline_ns;    /* RT only: receives must finish before this */
static ecm_evring_t     g_ev;                  /* RT -> monitor */
static ecm_cmdq_t       g_cmdq;                /* monitor -> RT */
static ecm_srec_plan_t  g_srec;                /* monitor only */
static atomic_int       g_bus_state_pub;       /* RT publishes, monitor reads */
static atomic_uint      g_recover_req;         /* RT: generation of the RECOVER it waits for */
static atomic_uint      g_recover_done;        /* monitor: generation done */
static atomic_int       g_recover_ok;          /* monitor: result of that generation */
/* Telemetry exclusion window (§5.3): frames on the wire that the RT
 * thread's tx_order ring does not describe. */
static atomic_int       g_excl_depth;
static atomic_uint      g_excl_gen;
static uint64_t         g_recoveries_b, g_recoveries_full;   /* monitor only */

/* ---- Giai doan 7.4: late / duplicate / stale replies (L5-07/08/09) ----
 * Index quarantine: SOEM hands out its 16 frame indexes in rotation (~1.1
 * frames per tick here) and accepts ANY frame carrying an index that is in
 * state EC_BUF_TX. A reply that comes back after its receive timed out is
 * therefore taken as the reply of whatever NEW frame got the same index
 * ~15 ms later: WKC fine, inputs 15 ms old. After a timeout the index is
 * parked in EC_BUF_COMPLETE for QUAR_TICKS: getindex() skips it and
 * ecx_inframe() drops a reply carrying it ("strange things happened").
 * At most QUAR_MAX parked at once, so a storm cannot starve getindex(). */
#define QUAR_TICKS 50
#define QUAR_MAX   8
static int      g_quarantine = 1;               /* --no-quarantine (negative control) */
static uint64_t g_quar_until[EC_MAXBUF];        /* RT only; 0 = not parked */
static int      g_quar_n;
static uint64_t g_quar_total, g_quar_overflow;
/* Input freshness (fault_policy §3.4 / plan §3.4): offset of a 16-bit
 * counter in EVERY slave's inputs that the slave increments per cycle. It
 * is the slave's PDO contract -- given on the command line, never assumed. */
static int      g_fresh_off   = -1;             /* --fresh-offset N */
static uint32_t g_fresh_stale = 20;             /* --fresh-stale N (cycles) */
static ecm_fresh_t g_fresh[ECM_SREC_MAX_SLAVES + 1];   /* RT only, [SOEM slave] */
static uint64_t g_stale_replies;                /* motion replies rejected by the DC age gate */
static int      g_reply_check = 1;              /* --no-reply-check (negative control): count, don't act */
static uint64_t g_foreign_replies[3];           /* [group]: reply header != what was sent */

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
    ecm_wkc_stats_t wkcs;          /* Giai doan 7.2: WKC by class (NOFRAME/ZERO/PARTIAL/OVER) */
    int          last_idx;         /* Giai doan 7.4: EtherCAT index of the last frame sent */
    uint8_t      in_snap[256];     /* Giai doan 7.4: inputs before this receive, restored  */
    uint32_t     in_snap_len;      /*   when the reply turns out to be an old one           */
    uint64_t     last_send_ns;     /* Giai doan 7.4: CLOCK_MONOTONIC right after sendto()    */
    int          reply_foreign;    /* Giai doan 7.4: this cycle's reply belonged to another frame */
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

/* Same as request_all_state(EC_STATE_OPERATIONAL, ...), except it keeps the
 * process-data watchdog fed while it waits: ecx_statecheck() only polls AL
 * status over the mailbox/FPRD path, it never sends process data, and each
 * of its internal retries already costs EC_TIMEOUTRET. Observed on the real
 * Jetson 2026-09-25: with GROUP_MOTION's SM watchdog at 3ms (see WD_TIME_*
 * above), slaves 1-4 latch SAFEOP+ERR / 0x001B (Sync manager watchdog) the
 * instant OP is requested -- every run, before any fault injection -- and
 * since there is no auto-recovery yet (Giai doan 7.3), they stay latched.
 * Root cause: the gap between the last real PD cycle sent before this call
 * and the RT loop's first cyclic send (after ecx_statecheck's polling, 4x
 * pthread_create and the SCHED_FIFO switch) exceeds 3ms. Fix: send real PD
 * cycles for both groups on a ~1ms tick while polling AL status directly
 * (ecx_readstate), instead of one blocking ecx_statecheck() call. */
static int request_op_keepalive(int timeout_us)
{
    ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&ctx, 0);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        ecx_send_processdata_group(&ctx, GROUP_MOTION);
        ecx_receive_processdata_group(&ctx, GROUP_MOTION, EC_TIMEOUTRET);
        ecx_send_processdata_group(&ctx, GROUP_IO);
        ecx_receive_processdata_group(&ctx, GROUP_IO, EC_TIMEOUTRET);

        ctx.slavelist[0].state = 0;
        ecx_readstate(&ctx);
        if (ctx.slavelist[0].state == EC_STATE_OPERATIONAL) return 1;

        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_us = (now.tv_sec - t0.tv_sec) * 1000000LL
                            + (now.tv_nsec - t0.tv_nsec) / 1000LL;
        if (elapsed_us >= timeout_us) return 0;
        usleep(1000);   /* motion tick period, well under the 3ms watchdog */
    }
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
/* Giai doan 7.4 (L5-07): does the reply now in rxbuf[idx] carry the same
 * datagrams (command, address, length) as the frame sent with this index?
 * A reply to ANOTHER frame that reused the index (e.g. a late motion reply
 * taken as the IO reply) does not. Logical (LRW/LRD/LWR) and FRMW
 * addresses are not changed on the way, so all four fields must match. */
static int reply_header_matches(int idx)
{
    const uint8 *tx = ctx.port.txbuf[idx] + ETH_HEADERSIZE;
    const uint8 *rx = ctx.port.rxbuf[idx];
    int txlen = ctx.port.txbuflength[idx] - ETH_HEADERSIZE;
    if (((rx[0] | (rx[1] << 8)) & 0x07FF) != ((tx[0] | (tx[1] << 8)) & 0x07FF)) return 0;
    int off = EC_ELENGTHSIZE;
    for (int k = 0; k < 32; k++) {
        if (off + (int)(EC_HEADERSIZE - EC_ELENGTHSIZE) > txlen) return 0;
        if (rx[off] != tx[off]) return 0;                               /* command */
        if (memcmp(rx + off + 2, tx + off + 2, 4) != 0) return 0;       /* ADP + ADO */
        uint16_t lt = (uint16_t)(tx[off + 6] | (tx[off + 7] << 8));
        uint16_t lr = (uint16_t)(rx[off + 6] | (rx[off + 7] << 8));
        if ((lt & 0x07FF) != (lr & 0x07FF)) return 0;                   /* length */
        off += (int)(EC_HEADERSIZE - EC_ELENGTHSIZE) + (lt & 0x07FF) + (int)EC_WKCSIZE;
        if (!(lt & 0x8000)) return 1;                                   /* last datagram */
    }
    return 0;
}

static void service_group(group_stats_t *g, uint64_t tick,
                           int64_t *out_prep_send_ns, int64_t *out_total_ns,
                           uint64_t *out_rx_ts_ns, int *out_wkc)
{
    struct timespec t0, t_after_send, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ecx_send_processdata_group(&ctx, g->group);
    /* Giai doan 7.4: index of the frame just sent (one frame per group
     * here), for index-keyed turnaround pairing and index quarantine. */
    int sent_idx = ctx.idxstack.pushed > 0 ? ctx.idxstack.idx[ctx.idxstack.pushed - 1] : -1;
    g->last_idx = sent_idx;
    tx_order_ring_push_idx(&g_tx_order, tick, (ecm_group_id_t)g->group,
                           sent_idx >= 0 ? (uint8_t)sent_idx : (uint8_t)TX_ORDER_IDX_UNKNOWN);

    clock_gettime(CLOCK_MONOTONIC, &t_after_send);
    g->last_send_ns = ts_to_ns(&t_after_send);
    if (out_prep_send_ns) *out_prep_send_ns = ts_diff_ns(&t_after_send, &t0);

    /* Giai doan 7.3 §2: the receive must end before the tick's deadline.
     * EC_TIMEOUTRET (2000 us) is twice the 1 ms cycle: one lost frame used
     * to cost 2-4 ms and turn into a chain of overruns. */
    int rx_timeout_us = g_rx_legacy ? EC_TIMEOUTRET
        : ecm_rx_timeout_us(ts_to_ns(&t_after_send), g_tick_deadline_ns, RX_MIN_US, (int)g_motion_cycle_us);
    {   /* Giai doan 7.4: keep the inputs as they were, in case this reply
         * turns out to be an old one (see g_stale_replies) */
        uint32_t ib = ctx.grouplist[g->group].Ibytes;
        g->in_snap_len = ib <= sizeof(g->in_snap) ? ib : 0;
        if (g->in_snap_len) memcpy(g->in_snap, ctx.grouplist[g->group].inputs, g->in_snap_len);
    }
    int wkc = ecx_receive_processdata_group(&ctx, g->group, rx_timeout_us);
    /* Giai doan 7.4: right after the receive, before anything else can
     * reuse the index: is it really the reply to what we sent? */
    g->reply_foreign = 0;
    if (wkc > 0 && sent_idx >= 0 && !reply_header_matches(sent_idx)) {
        g->reply_foreign = 1;
        g_foreign_replies[g->group]++;
        if (g_reply_check) {
            if (g->in_snap_len)
                memcpy(ctx.grouplist[g->group].inputs, g->in_snap, g->in_snap_len);
            /* rejected: from here on this cycle had no reply (WKC stats,
             * wkc_mismatch, the caller's DC update and state machine) */
            wkc = EC_NOFRAME;
        }
    }
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
    ecm_wkc_account(&g->wkcs, wkc, (int)g->expected_wkc);   /* Giai doan 7.2, no syscalls */
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

/* ---- SM watchdog and DC anchor as functions (Giai doan 7.3): also used
 * when a power-cycled slave is brought back. See main() for the rationale
 * of the values. ---- */
#define REG_SM_WD_DIVIDER     0x0400u
#define REG_SM_WD_TIME_PROCDATA 0x0420u
#define WD_DIVIDER_VALUE        2498u  /* -> 100us base tick */
#define WD_TIME_MOTION_TICKS      30u  /* -> 3ms  (3x GROUP_MOTION's 1ms cycle) */
#define WD_TIME_IO_TICKS         240u  /* -> 24ms (3x GROUP_IO's 8ms cycle) */

/* Returns the number of failed writes (0..2). Blocking. */
static int write_sm_watchdog(int slave)
{
    uint16_t divider = htoes((uint16_t)WD_DIVIDER_VALUE);
    uint16_t wd_time = htoes((uint16_t)((ctx.slavelist[slave].group == GROUP_IO)
                                        ? WD_TIME_IO_TICKS : WD_TIME_MOTION_TICKS));
    uint16_t configadr = ctx.slavelist[slave].configadr;
    int fail = 0;
    if (ecx_FPWR(&ctx.port, configadr, REG_SM_WD_DIVIDER, sizeof(divider), &divider, EC_TIMEOUTRET) <= 0) fail++;
    if (ecx_FPWR(&ctx.port, configadr, REG_SM_WD_TIME_PROCDATA, sizeof(wd_time), &wd_time, EC_TIMEOUTRET) <= 0) fail++;
    return fail;
}

static uint16_t g_dc_ref;           /* reference clock slave (1-based)   */
static int64_t  g_dc_cyc_ns;        /* SYNC0 cycle                        */
static int64_t  g_dc_setpoint_ns;   /* wanted phase after SYNC0           */

/* anchor: 0x0910 (system time) and 0x0990 (next SYNC0) of the reference
 * clock in ONE datagram, so both belong to one instant. (Re)initialises
 * g_dc: the caller must be the only user of g_dc at that moment (main()
 * before the RT loop, or the monitor during RECOVER). Returns the WKC. */
static int dc_anchor(int32_t *lead_out)
{
    uint8_t snap[0x88];
    memset(snap, 0, sizeof(snap));
    int w = ecx_FPRD(&ctx.port, ctx.slavelist[g_dc_ref].configadr, ECT_REG_DCSYSTIME,
                     sizeof(snap), snap, EC_TIMEOUTRET);
    struct timespec t_anchor;
    clock_gettime(CLOCK_MONOTONIC, &t_anchor);
    uint64_t dc_raw = 0, s0_raw = 0;
    for (int b = 7; b >= 0; b--) {
        dc_raw = (dc_raw << 8) | snap[b];
        s0_raw = (s0_raw << 8) | snap[0x80 + b];
    }
    int32_t lead = (int32_t)((uint32_t)s0_raw - (uint32_t)dc_raw);
    if (lead_out) *lead_out = lead;
    if (w != 1 || lead <= 0) return w != 1 ? w : 0;
    ecm_dc_cfg_t dcfg;
    ecm_dc_default_cfg(&dcfg, g_dc_cyc_ns, g_dc_setpoint_ns);
    /* Giai doan 7.4: reply-age gate on. Valid here because every receive
     * ends by the tick deadline (< 1 cycle after the send) and the DC
     * update gets the send time (see ecm_dc.h). */
    dcfg.gate_ns = g_dc_cyc_ns;
    ecm_dc_init(&g_dc, &dcfg);
    ecm_dc_anchor(&g_dc, dc_raw, s0_raw, ts_to_ns(&t_anchor));
    return w;
}

/* ---- Giai doan 7.3: telemetry exclusion window (docs/fault_policy.md
 * §5.3). Anyone about to put frames on the wire that the RT thread's
 * tx_order ring does not describe (monitor recovery, LOST probing) opens a
 * window; the telemetry thread discards what it sees meanwhile and resyncs
 * its TX/RX matcher afterwards. Nesting is allowed (depth counter). ---- */
static void excl_begin(void)
{
    atomic_fetch_add_explicit(&g_excl_gen, 1u, memory_order_acq_rel);
    atomic_fetch_add_explicit(&g_excl_depth, 1, memory_order_acq_rel);
}

static void excl_end(void)
{
    atomic_fetch_sub_explicit(&g_excl_depth, 1, memory_order_acq_rel);
}

/* ---- Giai doan 7.4: index quarantine (RT thread) ---- */
static void quar_add(int idx, uint64_t tick)
{
    if (!g_quarantine || idx < 0 || idx >= EC_MAXBUF) return;
    pthread_mutex_lock(&ctx.port.getindex_mutex);        /* vs. getindex() on another thread */
    if (ctx.port.rxbufstat[idx] == EC_BUF_EMPTY && g_quar_until[idx] == 0) {
        if (g_quar_n >= QUAR_MAX) {                      /* release the oldest */
            int old = -1;
            for (int i = 0; i < EC_MAXBUF; i++)
                if (g_quar_until[i] && (old < 0 || g_quar_until[i] < g_quar_until[old])) old = i;
            if (old >= 0) {
                if (ctx.port.rxbufstat[old] == EC_BUF_COMPLETE) ctx.port.rxbufstat[old] = EC_BUF_EMPTY;
                g_quar_until[old] = 0;
                g_quar_n--;
                g_quar_overflow++;
            }
        }
        ctx.port.rxbufstat[idx] = EC_BUF_COMPLETE;
        g_quar_until[idx] = tick + QUAR_TICKS;
        g_quar_n++;
        g_quar_total++;
    }
    pthread_mutex_unlock(&ctx.port.getindex_mutex);
}

static void quar_expire(uint64_t tick)
{
    if (!g_quar_n) return;
    pthread_mutex_lock(&ctx.port.getindex_mutex);
    for (int i = 0; i < EC_MAXBUF; i++) {
        if (g_quar_until[i] && tick >= g_quar_until[i]) {
            if (ctx.port.rxbufstat[i] == EC_BUF_COMPLETE) ctx.port.rxbufstat[i] = EC_BUF_EMPTY;
            g_quar_until[i] = 0;
            g_quar_n--;
        }
    }
    pthread_mutex_unlock(&ctx.port.getindex_mutex);
}

/* ---- Giai doan 7.4: input freshness, one slave (RT thread, no syscalls) ---- */
static void fresh_feed(int s, uint64_t tick)
{
    const ec_slavet *sl = &ctx.slavelist[s];
    if (s > ECM_SREC_MAX_SLAVES || !sl->inputs || sl->Ibytes < (uint32_t)g_fresh_off + 2) return;
    uint32_t v = (uint32_t)sl->inputs[g_fresh_off] | ((uint32_t)sl->inputs[g_fresh_off + 1] << 8);
    ecm_fresh_ev_t fe = ecm_fresh_update(&g_fresh[s], v);
    if (fe == ECM_FRESH_STALE_START || fe == ECM_FRESH_RESUMED
        || (fe == ECM_FRESH_REGRESSION && g_fresh[s].regressions <= 5)) {
        ecm_event_t e = { .tick = tick, .type = ECM_EV_FRESH, .a = s, .b = (int32_t)fe };
        ecm_evring_push(&g_ev, &e);
    }
}

/* RT thread, non-IO ticks only: execute at most one queued recovery
 * command (path A: one FPWR of AL control), only if the tick still has
 * CMD_MIN_BUDGET_US before its deadline -- otherwise it waits for the next
 * tick. One sendto() -> one tx_order push. */
static void rt_exec_command(uint64_t tick)
{
    ecm_cmd_t c;
    if (!ecm_cmdq_peek(&g_cmdq, &c)) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int to = ecm_rx_timeout_us(ts_to_ns(&now), g_tick_deadline_ns, 0, 300);
    if (to < CMD_MIN_BUDGET_US) return;
    uint16_t v = htoes(c.value);
    int w = ecx_FPWR(&ctx.port, c.configadr, c.reg, sizeof(v), &v, to);
    tx_order_ring_push_idx(&g_tx_order, tick, (ecm_group_id_t)GROUP_MOTION, ctx.port.lastidx);
    if (w == EC_NOFRAME) quar_add(ctx.port.lastidx, tick);
    ecm_cmdq_drop_head(&g_cmdq);
    ecm_event_t e = { .tick = tick, .type = ECM_EV_CMD_DONE, .a = c.slave, .b = w };
    ecm_evring_push(&g_ev, &e);
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

/* Giai doan 7.4: 2 ms (was 10 ms). Index pairing gives up on a send after
 * TURNAROUND_MAX_AGE_TICKS; a 10 ms batch made it give up on sends whose
 * TX completion was simply still waiting in the error queue. */
#define TELEMETRY_POLL_US 2000

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
    /* Giai doan 7.2 finding (sandbox, kernel 6.18, veth): with TX
     * timestamping on SOEM's own socket and this thread draining its error
     * queue while the RT thread recv()s on the same fd, SOEM saw ~1% motion
     * NOFRAME, 30% IO PARTIAL and replies landing in the wrong index
     * buffer; all gone with timestamping off. --no-tx-ts gives the A/B on
     * the Jetson (turnaround then has no TX side). */
    if (g_no_tx_ts) {
        fprintf(stderr, "telemetry: --no-tx-ts: TX timestamping OFF, turnaround will be empty\n");
        soem_raw_fd = -1;
    } else if (enable_tx_sw_timestamping(soem_raw_fd) != 0) {
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
    unsigned excl_seen_gen = 0;                 /* Giai doan 7.3 exclusion window */
    uint64_t excl_polls = 0, excl_order = 0, excl_txc = 0, excl_rx = 0;

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

        /* Giai doan 7.3 (docs/fault_policy.md §5.3): while another thread
         * sends frames of its own (recovery) or the bus is LOST/RECOVER,
         * send order != tx_order order. Discard everything seen in such a
         * poll, count it, and restart the matcher once the window closed. */
        {
            unsigned gen = atomic_load_explicit(&g_excl_gen, memory_order_acquire);
            int depth = atomic_load_explicit(&g_excl_depth, memory_order_acquire);
            if (depth > 0 || gen != excl_seen_gen) {
                tx_order_sample_t o;
                while (tx_order_ring_pop(&g_tx_order, &o) == 0) excl_order++;
                if (soem_raw_fd >= 0) {
                    struct pollfd pfd = { .fd = soem_raw_fd, .events = 0 };
                    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLERR)) {
                        char control[512], buf[128];
                        struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
                        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                                              .msg_control = control, .msg_controllen = sizeof(control) };
                        if (recvmsg(soem_raw_fd, &msg, MSG_ERRQUEUE) < 0) break;
                        excl_txc++;
                    }
                }
                if (passive_rx_fd >= 0) {
                    char buf[1600];
                    while (recv(passive_rx_fd, buf, sizeof(buf), MSG_DONTWAIT) >= 0) excl_rx++;
                }
                excl_polls++;
                if (depth == 0) {
                    turnaround_resync(&g_telemetry.turnaround);
                    excl_seen_gen = gen;
                }
                usleep(TELEMETRY_POLL_US);
                continue;
            }
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

                ssize_t tr = recvmsg(soem_raw_fd, &msg, MSG_ERRQUEUE);
                if (tr < 0) break;
                /* Giai doan 7.4: the error queue returns the sent frame
                 * itself -> pair by its EtherCAT index, not by position */
                int tx_idx = turnaround_frame_idx((const uint8_t *)buf, (size_t)tr);

                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                        struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
                        uint64_t tx_ts_ns = ts_to_ns(&ts[0]);  /* [0]=software, matches enable_tx_sw_timestamping */
                        turnaround_on_tx_complete_idx(&g_telemetry.turnaround, tx_ts_ns, tx_idx);
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
                        /* Giai doan 7.4: pair by EtherCAT index (duplicates,
                         * late and lost replies no longer shift the pairing) */
                        turnaround_on_rx_arrival_idx(&g_telemetry.turnaround, rx_ts_ns,
                                                     turnaround_frame_idx((const uint8_t *)buf, (size_t)r),
                                                     &g_telemetry.turnaround_hist);
                    }
                }
            }
        }

        if (++print_counter >= 1000000 / TELEMETRY_POLL_US) {   /* ~once/second */
            print_counter = 0;
            hist_print(&g_telemetry.wake_jitter_hist, "wake_jitter");
            hist_print(&g_telemetry.prep_send_hist,   "prep_send");
            hist_print(&g_telemetry.occupancy_hist,   "occupancy");
            hist_print(&g_telemetry.turnaround_hist,  "turnaround");
        }

        usleep(TELEMETRY_POLL_US);   /* non-RT thread */
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
    fprintf(stderr, "  index pairing (Giai doan 7.4): tx_skipped=%" PRIu64 " tx_unmatched=%" PRIu64
                    " rx_skipped=%" PRIu64 " (sends whose reply never came) rx_unmatched=%" PRIu64
                    " (duplicate/late/foreign arrivals)\n",
            g_telemetry.turnaround.stat_tx_skipped, g_telemetry.turnaround.stat_tx_unmatched,
            g_telemetry.turnaround.stat_rx_skipped, g_telemetry.turnaround.stat_rx_unmatched);
    fprintf(stderr, "  exclusion windows (Giai doan 7.3): polls=%" PRIu64 " discarded tx_order=%" PRIu64
                    " tx_completions=%" PRIu64 " rx=%" PRIu64 "\n", excl_polls, excl_order, excl_txc, excl_rx);
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
static const char *al_code_str(uint16_t code) { return ec_ALstatuscode2string(code); }

/* Findings identity without counts/activity, so a steadily counting fault
 * is reported once, not every second. */
static size_t finding_signature(const ecm_diag_finding_t *f, int nf, char *buf, size_t cap)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < nf && len + 32 < cap; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%d:%d:%d:%u;", (int)f[i].type, f[i].a, f[i].b, f[i].c);
    return len;
}

static void diag_report_changes(const ecm_diag_finding_t *f, int nf)
{
    static char prev_sig[2048], sig[2048];
    finding_signature(f, nf, sig, sizeof(sig));
    if (strcmp(sig, prev_sig) == 0) return;
    memcpy(prev_sig, sig, sizeof(sig));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "ecm_run: diag t_mono=%.3f findings changed (%d):\n",
            (double)ts.tv_sec + ts.tv_nsec * 1e-9, nf);
    for (int i = 0; i < nf; i++) {
        char line[256];
        ecm_diag_finding_str(&f[i], al_code_str, line, sizeof(line));
        fprintf(stderr, "  %s\n", line);
    }
}

static void diag_process_pending(void)
{
    static ecm_diag_raw_t raw;
    static ecm_diag_finding_t f[128];
    static char text[65536];
    static const char *const names[2] = { "motion", "io" };
    int got = 0;
    while (ecm_diag_handoff_get(&g_diag_ho, &raw)) { ecm_diag_ingest(&g_diag, &raw); got = 1; }
    if (!got) return;
    int nf = ecm_diag_analyze(&g_diag, f, 128);
    size_t len = ecm_diag_format(&g_diag, f, nf, names, "ecm_run", al_code_str, text, sizeof(text));
    if (ecm_diag_write_file(g_diag_path, text, len) != 0) {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "ecm_run: cannot write %s\n", g_diag_path); }
    }
    diag_report_changes(f, nf);
}

/* ==========================================================================
 * Giai doan 7.3: recovery, monitor side (docs/fault_policy.md §4, §5).
 * Everything here may block for seconds; it runs on the monitor thread.
 * ========================================================================== */
static double mono_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int b = 7; b >= 0; b--) v = (v << 8) | p[b];
    return v;
}

/* A power-cycled DC slave has lost its system time offset (0x0920) and
 * propagation delay (0x0928). Restore both from what ecx_configdc()
 * measured at startup, then re-arm SYNC0. The offset is computed from ONE
 * frame reading the reference clock's and this slave's 0x0910: the frame
 * reaches the slave (pdelay_s - pdelay_ref) after the reference clock. */
static void dc_restore_slave(int s)
{
    ec_slavet *sl = &ctx.slavelist[s];
    if (!g_dc_enabled || sl->group != GROUP_MOTION || !sl->hasdc) return;
    ec_slavet *rf = &ctx.slavelist[g_dc_ref];

    int32_t dly = (int32_t)htoel((uint32_t)sl->pdelay);
    ecx_FPWR(&ctx.port, sl->configadr, ECT_REG_DCSYSDELAY, sizeof(dly), &dly, EC_TIMEOUTRET);
    uint8_t ob[8] = { 0 };
    ecx_FPRD(&ctx.port, sl->configadr, ECT_REG_DCSYSOFFSET, sizeof(ob), ob, EC_TIMEOUTRET);
    int64_t old_off = (int64_t)le64(ob);

    ecx_portt *port = &ctx.port;
    uint8_t zero[8] = { 0 };
    uint8_t idx = ecx_getindex(port);
    ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPRD, idx, rf->configadr, ECT_REG_DCSYSTIME, 8, zero);
    uint16_t off2 = ecx_adddatagram(port, &(port->txbuf[idx]), EC_CMD_FPRD, idx, FALSE,
                                    sl->configadr, ECT_REG_DCSYSTIME, 8, zero);
    int w = ecx_srconfirm(port, idx, EC_TIMEOUTRET);
    uint64_t t_ref = le64(port->rxbuf[idx] + EC_HEADERSIZE);
    uint64_t t_sl  = le64(port->rxbuf[idx] + off2);
    uint16_t wkc2  = (uint16_t)(port->rxbuf[idx][off2 + 8] | (port->rxbuf[idx][off2 + 9] << 8));
    ecx_setbufstat(port, idx, EC_BUF_EMPTY);
    if (w <= 0 || wkc2 != 1) {
        fprintf(stderr, "ecm_run: [RECOVERY] slave %d: DC time read failed (wkc=%d/%u), SYNC0 not re-armed\n",
                s, w, wkc2);
        return;
    }
    int64_t delta = (int64_t)(t_ref - t_sl) + (sl->pdelay - rf->pdelay);
    int64_t new_off = (int64_t)htoell((uint64_t)(old_off + delta));
    ecx_FPWR(&ctx.port, sl->configadr, ECT_REG_DCSYSOFFSET, sizeof(new_off), &new_off, EC_TIMEOUTRET);
    ecx_dcsync0(&ctx, (uint16)s, TRUE, (uint32)g_dc_cyc_ns, 0);
    fprintf(stderr, "ecm_run: [RECOVERY] slave %d: DC restored (delay %d ns, offset step %+lld ns), SYNC0 re-armed\n",
            s, sl->pdelay, (long long)delta);
}

/* PRE-OP -> SAFE-OP hook for ecx_reconfig_slave(): everything ecm_run
 * wrote by hand at startup that a power cycle erases. Registered only for
 * the duration of one reconfiguration. */
static int reconfig_po2so_hook(ecx_contextt *c, uint16 slave)
{
    (void)c;
    int f = write_sm_watchdog(slave);
    dc_restore_slave(slave);
    return f == 0;
}

/* Path B: address + full reconfiguration of one slave (1-based), leaves it
 * in SAFE-OP; path A then takes it to OP. Returns 1 on SAFE-OP. */
static int recover_path_b(int s)
{
    excl_begin();
    double t0 = mono_now_s();
    int addr_ok = ecx_recover_slave(&ctx, (uint16)s, EC_TIMEOUTRET3);
    int st = 0;
    if (addr_ok > 0) {
        ctx.slavelist[s].PO2SOconfig = reconfig_po2so_hook;
        st = ecx_reconfig_slave(&ctx, (uint16)s, EC_TIMEOUTRET3);
        ctx.slavelist[s].PO2SOconfig = NULL;
#ifdef ECMASTER_SOEM_MBXCNT_PATCH
        /* Giai doan 7.4: a power-cycled slave starts its mailbox Cnt at 1
         * again; forget the old one or its first response may be taken
         * for a duplicate (patches/soem-mbx-cnt.patch). */
        ctx.slavelist[s].mbxincnt = 0;
#endif
    }
    excl_end();
    g_recoveries_b++;
    fprintf(stderr, "ecm_run: [RECOVERY] slave %d: recover+reconfigure: address %s, state after = 0x%02x%s (%.0f ms)\n",
            s, addr_ok > 0 ? "ok" : "NOT restored (not the same slave, or not answering)",
            st, st == EC_STATE_SAFE_OP ? " SAFE-OP" : "", (mono_now_s() - t0) * 1e3);
    return st == EC_STATE_SAFE_OP;
}

/* RECOVER (§5.2): the RT thread has stopped sending; the bus is ours.
 * Returns 1 if at least one slave is back in OP. */
static int recover_full(void)
{
    excl_begin();
    double t0 = mono_now_s();
    uint16_t al = 0;
    int brd = ecx_BRD(&ctx.port, 0, ECT_REG_ALSTAT, sizeof(al), &al, EC_TIMEOUTRET3);
    int n = ctx.slavecount, nb = 0, nack = 0;
    for (int s = 1; s <= n; s++) {
        if (s - 1 >= brd) continue;                       /* behind a break: nothing to do from here */
        uint16_t st = 0;
        int w = ecx_FPRD(&ctx.port, ctx.slavelist[s].configadr, ECT_REG_ALSTAT, sizeof(st), &st, EC_TIMEOUTRET);
        st = etohs(st);
        uint8_t state = (uint8_t)(st & 0x0F);
        if (w <= 0 || state == EC_STATE_INIT || state == EC_STATE_PRE_OP) {
            recover_path_b(s);
            nb++;
        } else if (st & EC_STATE_ERROR) {
            uint16_t v = htoes((uint16_t)(state | EC_STATE_ACK));
            ecx_FPWR(&ctx.port, ctx.slavelist[s].configadr, ECT_REG_ALCTL, sizeof(v), &v, EC_TIMEOUTRET);
            nack++;
        }
    }
    /* Same sequence as at startup: valid outputs first, then OP with the
     * watchdog kept fed. */
    ecx_send_processdata_group(&ctx, GROUP_MOTION);
    ecx_receive_processdata_group(&ctx, GROUP_MOTION, EC_TIMEOUTRET);
    ecx_send_processdata_group(&ctx, GROUP_IO);
    ecx_receive_processdata_group(&ctx, GROUP_IO, EC_TIMEOUTRET);
    int all_op = request_op_keepalive(RECOVER_OP_TIMEOUT_US);
    ecx_readstate(&ctx);
    int nop = 0;
    for (int s = 1; s <= n; s++)
        if ((ctx.slavelist[s].state & 0x0F) == EC_STATE_OPERATIONAL) nop++;
    int32_t lead = 0;
    int dcw = g_dc_enabled ? dc_anchor(&lead) : 1;
    excl_end();
    g_recoveries_full++;
    fprintf(stderr, "ecm_run: [RECOVER] bus answered by %d slave(s); reconfigured %d, acked %d; "
            "%d/%d in OP%s%s (%.0f ms)\n", brd, nb, nack, nop, n, all_op ? "" : " (not all)",
            g_dc_enabled ? (dcw == 1 ? ", DC re-anchored" : ", DC re-anchor FAILED") : "",
            (mono_now_s() - t0) * 1e3);
    return nop > 0;
}

/* Per-slave planner, after each fresh diagnostic read (§4). */
static void recovery_step(void)
{
    int bs = atomic_load_explicit(&g_bus_state_pub, memory_order_relaxed);
    if (bs != ECM_BUS_RUN && bs != ECM_BUS_DEGRADED) return;
    uint64_t now_ns = (uint64_t)(mono_now_s() * 1e9);
    for (int i = 0; i < g_diag.n && i < g_srec.n; i++) {
        const ecm_diag_slave_t *ds = &g_diag.s[i];
        if (ds->seen_read != g_diag.reads) continue;      /* not in the last (chunked) read */
        ecm_srec_input_t in = { .in_reach = i < g_diag.brd_count, .answered = ds->answered,
                                .al_status = ds->al_status };
        int rec = 0, gu = 0;
        ecm_sact_t a = ecm_srec_decide(&g_srec, i, &in, now_ns, &rec, &gu);
        int s = i + 1;
        if (rec)
            fprintf(stderr, "ecm_run: [RECOVERY] slave %d back in OP (recovery #%llu)\n",
                    s, (unsigned long long)g_srec.s[i].recoveries);
        if (gu)
            fprintf(stderr, "ecm_run: [RECOVERY] slave %d: gave up after %u attempts -> FAILED "
                    "(AL 0x%02x code 0x%04x), needs an operator\n",
                    s, g_srec.max_attempts, ds->al_status, ds->al_code);
        if (a == ECM_SACT_NONE) continue;
        if (!ds->answered)
            fprintf(stderr, "ecm_run: [RECOVERY] slave %d: on the bus but not answering its address "
                    "(power-cycled?) -> %s (attempt %u/%u)\n",
                    s, ecm_sact_name(a), g_srec.s[i].attempts, g_srec.max_attempts);
        else
            fprintf(stderr, "ecm_run: [RECOVERY] slave %d: AL 0x%02x code 0x%04x (%s) -> %s (attempt %u/%u)\n",
                    s, ds->al_status, ds->al_code, ds->al_code ? al_code_str(ds->al_code) : "no error",
                    ecm_sact_name(a), g_srec.s[i].attempts, g_srec.max_attempts);
        ecm_cmd_t c = { .slave = (uint16_t)s, .configadr = ctx.slavelist[s].configadr, .reg = ECT_REG_ALCTL };
        switch (a) {
        case ECM_SACT_ACK:
            c.value = (uint16_t)((ds->al_status & 0x0F) | EC_STATE_ACK);
            ecm_cmdq_push(&g_cmdq, &c);
            break;
        case ECM_SACT_OP:
            c.value = EC_STATE_OPERATIONAL;
            ecm_cmdq_push(&g_cmdq, &c);
            break;
        case ECM_SACT_RECONFIG:
            recover_path_b(s);
            break;
        default:
            break;
        }
    }
}

static void print_events(void)
{
    ecm_event_t e;
    while (ecm_evring_pop(&g_ev, &e)) {
        if (e.type == ECM_EV_BUS) {
            char why[64] = "";
            if (e.to == ECM_BUS_DEGRADED || (e.to == ECM_BUS_LOST && e.from != ECM_BUS_RECOVER))
                snprintf(why, sizeof(why), " (%s %s)", e.b ? "io" : "motion",
                         ecm_wkc_class_name((ecm_wkc_class_t)e.a));
            else if (e.to == ECM_BUS_RECOVER)
                snprintf(why, sizeof(why), " (bus answers: BRD WKC %d)", e.a);
            else if (e.to == ECM_BUS_LOST)
                snprintf(why, sizeof(why), " (recovery failed, retry in %u ms)",
                         (unsigned)(g_bus.cfg.recover_backoff_ticks * g_motion_cycle_us / 1000));
            fprintf(stderr, "ecm_run: [BUS] tick=%llu t_mono=%.3f %s -> %s%s\n",
                    (unsigned long long)e.tick, mono_now_s(),
                    ecm_bus_state_name((ecm_bus_state_t)e.from), ecm_bus_state_name((ecm_bus_state_t)e.to), why);
        } else if (e.type == ECM_EV_FRESH) {
            const char *what = e.b == ECM_FRESH_STALE_START ? "inputs UNCHANGED -> stale data although WKC is correct"
                             : e.b == ECM_FRESH_RESUMED     ? "inputs changing again"
                             : "inputs went BACKWARDS -> an older reply was taken for this cycle";
            fprintf(stderr, "ecm_run: [FRESH] tick=%llu slave %d: %s\n", (unsigned long long)e.tick, e.a, what);
        } else if (e.type == ECM_EV_CMD_DONE) {
            if (e.b <= 0)
                fprintf(stderr, "ecm_run: [RECOVERY] slave %d: AL control write not acknowledged (wkc=%d)\n", e.a, e.b);
        }
    }
}

/* Giai doan 7.2/7.3. 10 ms cadence: bus events and RECOVER requests need a
 * fast reaction; the diagnostic snapshot (and the per-slave planner that
 * works on it) still runs every 100 ms. Blocking is fine: SCHED_OTHER. */
static void *monitor_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();
    unsigned it = 0;
    while (!g_monitor_stop) {
        usleep(10000);
        print_events();
        unsigned req = atomic_load_explicit(&g_recover_req, memory_order_acquire);
        if (req != atomic_load_explicit(&g_recover_done, memory_order_relaxed)) {
            int ok = g_recover_enabled ? recover_full() : 0;
            if (!g_recover_enabled)
                fprintf(stderr, "ecm_run: [RECOVER] --no-recover: bus answers again, not recovering\n");
            atomic_store_explicit(&g_recover_ok, ok, memory_order_relaxed);
            atomic_store_explicit(&g_recover_done, req, memory_order_release);
            continue;
        }
        if (++it % 10 == 0 && g_diag_enabled) {
            uint64_t before = g_diag.reads;
            diag_process_pending();
            if (g_recover_enabled && g_diag.reads != before) recovery_step();
        }
    }
    print_events();
    if (g_diag_enabled) diag_process_pending();   /* last read before exit */
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
    long n_lost          = 100; /* Giai doan 7.3 */

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
        } else if (strcmp(argv[i], "--no-tx-ts") == 0) {
            g_no_tx_ts = 1;
        } else if (strcmp(argv[i], "--no-diag") == 0) {
            g_diag_enabled = 0;
        } else if (strcmp(argv[i], "--diag-file") == 0 && i + 1 < argc) {
            g_diag_path = argv[++i];
        } else if (strcmp(argv[i], "--no-recover") == 0) {           /* Giai doan 7.3 */
            g_recover_enabled = 0;
        } else if (strcmp(argv[i], "--rx-timeout-legacy") == 0) {
            g_rx_legacy = 1;
        } else if (strcmp(argv[i], "--n-lost") == 0 && i + 1 < argc) {
            n_lost = atol(argv[++i]);
        } else if (strcmp(argv[i], "--no-quarantine") == 0) {        /* Giai doan 7.4 */
            g_quarantine = 0;
        } else if (strcmp(argv[i], "--no-reply-check") == 0) {
            g_reply_check = 0;
        } else if (strcmp(argv[i], "--fresh-offset") == 0 && i + 1 < argc) {
            g_fresh_off = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fresh-stale") == 0 && i + 1 < argc) {
            g_fresh_stale = (uint32_t)atol(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (!ifname || n <= 0 || motion_slaves <= 0 || motion_slaves >= n) {
        fprintf(stderr,
            "Usage: %s --iface <veth_m> --n <total_slaves> --motion-slaves <count> "
            "[--motion-cycle-us N] [--io-cycle-us N] [--duration-sec N] "
            "[--no-dc] [--dc-setpoint-pct N] [--no-diag] [--diag-file PATH] [--no-tx-ts]\n"
            "       [--no-recover] [--rx-timeout-legacy] [--n-lost N]\n"
            "       [--no-quarantine] [--no-reply-check] [--fresh-offset BYTE] [--fresh-stale CYCLES]\n", argv[0]);
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
    g_motion_cycle_us = motion_cycle_us;   /* Giai doan 7.3: receive budget */

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
    /* (register/value constants moved to file scope in Giai doan 7.3:
     * write_sm_watchdog() also runs after recovering a power-cycled slave) */

    int wd_fail = 0;
    for (int slave = 1; slave <= ctx.slavecount; slave++) {
        wd_fail += write_sm_watchdog(slave);
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
            g_dc_ref = ref;
            g_dc_cyc_ns = cyc;
            g_dc_setpoint_ns = cyc * dc_setpoint_pct / 100;
            int32_t lead = 0;
            int w = dc_anchor(&lead);
            if (w != 1 || lead <= 0) {
                fprintf(stderr, "ecm_run: DC off -- anchor read failed (wkc=%d lead=%d ns)\n", w, lead);
            } else {
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

    if (!request_op_keepalive(EC_TIMEOUTSTATE)) {
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
    ecm_diag_soem_io_init(&g_diag_io);          /* Giai doan 7.2 */
    ecm_diag_handoff_init(&g_diag_ho);
    ecm_diag_init(&g_diag, ctx.slavecount, EC_STATE_OPERATIONAL);
    {                                           /* Giai doan 7.3 */
        ecm_bus_cfg_t bcfg;
        ecm_bus_default_cfg(&bcfg);
        if (n_lost > 0) bcfg.n_lost = (uint32_t)n_lost;
        ecm_bus_init(&g_bus, &bcfg);
        ecm_evring_init(&g_ev);
        ecm_cmdq_init(&g_cmdq);
        ecm_srec_init(&g_srec, ctx.slavecount, 5, 1000000000ull);
        atomic_init(&g_bus_state_pub, ECM_BUS_RUN);
        atomic_init(&g_recover_req, 0u);
        atomic_init(&g_recover_done, 0u);
        atomic_init(&g_recover_ok, 0);
        atomic_init(&g_excl_depth, 0);
        atomic_init(&g_excl_gen, 0u);
        for (int s = 0; s <= ECM_SREC_MAX_SLAVES; s++) ecm_fresh_init(&g_fresh[s], g_fresh_stale, 16);
        fprintf(stderr, "ecm_run: late replies: index quarantine %s (%d ticks, max %d), reply check %s; input freshness %s\n",
                g_quarantine ? "ON" : "OFF (--no-quarantine)", QUAR_TICKS, QUAR_MAX,
                g_reply_check ? "ON" : "OFF (--no-reply-check)",
                g_fresh_off >= 0 ? "ON" : "off (no --fresh-offset)");
        if (g_fresh_off >= 0)
            fprintf(stderr, "ecm_run: freshness: 16-bit counter at input byte %d of every slave, "
                    "stale after %u unchanged cycles\n", g_fresh_off, g_fresh_stale);
#ifdef ECMASTER_SOEM_MBXCNT_PATCH
        fprintf(stderr, "ecm_run: SOEM mailbox Cnt patch: present (duplicated mailbox responses dropped)\n");
#else
        fprintf(stderr, "ecm_run: SOEM mailbox Cnt patch: ABSENT -- a duplicated mailbox response shifts every "
                "later SDO answer by one (L5-12), see patches/soem-mbx-cnt.patch\n");
#endif
        fprintf(stderr, "ecm_run: fault policy: rx timeout %s, LOST after %u motion NOFRAME, recovery %s\n",
                g_rx_legacy ? "LEGACY EC_TIMEOUTRET (negative control)" : "= tick deadline - 150 us",
                bcfg.n_lost, g_recover_enabled ? "ON" : "OFF (--no-recover)");
    }

    /* Giai doan 7.2 finding (2026-09-25): explicit, small stacks. With
     * mlockall(MCL_CURRENT | MCL_FUTURE) above, every pthread_create()
     * with the default 8 MB stack locks and zero-fills all 8 MB up front:
     * 4 threads took 8-12 ms (measured), right between reaching OP and the
     * RT loop's first tick -- no process data during that time, so the
     * 3 ms GROUP_MOTION SM watchdog expired and every motion slave latched
     * SAFEOP+ERR 0x001B at startup, every run. Invisible before soft_bus
     * modelled the watchdog (Giai doan 7.1). Deepest thread frame is
     * ~2.5 KB (gcc -fstack-usage) plus libc stdio: 256 KB is >10x margin,
     * and 4 x 256 KB locks in well under 1 ms. */
    pthread_t telemetry_tid, app_tid, mailbox_tid, monitor_tid;
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, NON_RT_STACK_BYTES);
        pthread_create(&telemetry_tid, &attr, telemetry_thread_fn, NULL);
        pthread_create(&app_tid,       &attr, app_thread_fn,       NULL);
        pthread_create(&mailbox_tid,   &attr, mailbox_thread_fn,   NULL);
        pthread_create(&monitor_tid,   &attr, monitor_thread_fn,   NULL);
        pthread_attr_destroy(&attr);
    }

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

    unsigned recover_gen = 0;             /* Giai doan 7.3: RECOVER generations requested */
    while (!g_stop) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        struct timespec t_wake;
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        int64_t wake_jitter_ns = ts_diff_ns(&t_wake, &next);   /* quantity #1 */

        /* Giai doan 7.3 §2: every receive in this tick ends by the deadline,
         * computed from the tick's TARGET time (not t_wake), so a late wake
         * shortens the budget instead of pushing the next tick. */
        g_tick_deadline_ns = ts_to_ns(&next) + (uint64_t)(motion_cycle_us * 1000L) - RX_GUARD_NS;
        ecm_bus_tick(&g_bus);
        quar_expire(tick);                          /* Giai doan 7.4 */
        ecm_event_t bus_ev;

        int64_t  motion_prep_send_ns = 0, motion_total_ns = 0;
        uint64_t motion_rx_ts_ns     = 0;
        int motion_wkc = 0;
        int64_t cycle_occupancy_ns = 0;
        uint8_t io_due = (tick % (uint64_t)ticks_per_io == 0) ? 1 : 0;
        uint8_t any_wkc_mismatch = 0;
        int64_t dc_adjust_ns = 0;

        if (g_bus.state == ECM_BUS_RUN || g_bus.state == ECM_BUS_DEGRADED) {
        /* (body below kept at its Giai doan 6 indentation so the 7.3 diff stays readable) */
        service_group(&motion, tick, &motion_prep_send_ns, &motion_total_ns, &motion_rx_ts_ns, &motion_wkc);
        cycle_occupancy_ns = motion_total_ns;   /* quantity #4, starts with motion's own cost */

        if (motion_wkc == EC_NOFRAME) quar_add(motion.last_idx, tick);   /* Giai doan 7.4 */
        int io_class = -1;
        int io_wkc = 0;
        if (io_due) {
            uint64_t before_mismatch = io.wkc_mismatch;
            int64_t io_total_ns = 0;
            service_group(&io, tick, NULL, &io_total_ns, NULL, &io_wkc);
            if (io_wkc == EC_NOFRAME) quar_add(io.last_idx, tick);
            cycle_occupancy_ns += io_total_ns;  /* io's work this tick also counts toward total occupancy */
            if (io.wkc_mismatch != before_mismatch) any_wkc_mismatch = 1;
            io_class = (int)ecm_wkc_classify(io_wkc, (int)io.expected_wkc);
        }
        {
            static uint64_t motion_mismatch_before;
            if (motion.wkc_mismatch != motion_mismatch_before) any_wkc_mismatch = 1;
            motion_mismatch_before = motion.wkc_mismatch;
        }

        /* ---- Giai doan 6: DC(b). ctx.DCtime was refreshed by the motion
         * receive (only GROUP_MOTION carries the FRMW), so read it before
         * anything else can send. Pure arithmetic, no syscalls. ---- */
        if (g_dc_enabled && motion_wkc > 0) {
            uint64_t wraps_before = g_dc.wraps;
            /* Giai doan 7.4: host time of the SEND, not of the wake: the DC
             * sample is taken when the frame passes the reference clock, and
             * the reply-age gate compares the two. On a non-RT host the
             * thread can be preempted between wake and send; the unwrap and
             * the PI (which works in the DC domain) do not care either way. */
            dc_adjust_ns = ecm_dc_update(&g_dc, (uint64_t)ctx.DCtime, motion.last_send_ns);
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

        /* ---- Giai doan 7.4 (L5-07): the DC reply-age gate says this motion
         * reply was taken at another time -> it is an old reply that SOEM
         * matched to this frame by a reused index. Put the inputs back and
         * count the cycle as "no frame". ---- */
        int motion_class = (int)ecm_wkc_classify(motion_wkc, (int)motion.expected_wkc);
        if (g_dc_enabled && motion_wkc > 0 && g_dc.last_rejected && !motion.reply_foreign) {
            g_stale_replies++;
            if (g_reply_check) {
                if (motion.in_snap_len)
                    memcpy(ctx.grouplist[GROUP_MOTION].inputs, motion.in_snap, motion.in_snap_len);
                motion_class = ECM_POL_WKC_NOFRAME;
            }
        }
        if (g_reply_check && motion.reply_foreign) motion_class = ECM_POL_WKC_NOFRAME;
        if (g_reply_check && io_due && io.reply_foreign) io_class = ECM_POL_WKC_NOFRAME;

        /* Giai doan 7.3 §3: bus state machine, pure arithmetic */
        if (ecm_bus_on_cycle(&g_bus, tick, motion_class, io_class, &bus_ev)) {
            ecm_evring_push(&g_ev, &bus_ev);
            if (bus_ev.to == ECM_BUS_LOST) excl_begin();   /* ends when the bus is back in RUN */
        }

        /* Giai doan 7.4 (L5-09): input freshness, only on replies that were
         * accepted with the full WKC (a missing slave is WKC's business). */
        if (g_fresh_off >= 0) {
            if (motion_class == ECM_POL_WKC_OK)
                for (int s = 1; s <= ctx.slavecount; s++)
                    if (ctx.slavelist[s].group == GROUP_MOTION) fresh_feed(s, tick);
            if (io_class == ECM_POL_WKC_OK)
                for (int s = 1; s <= ctx.slavecount; s++)
                    if (ctx.slavelist[s].group == GROUP_IO) fresh_feed(s, tick);
        }

        /* ---- Giai doan 7.2: diagnostics. One frame per second, never on an
         * IO tick; the reply is collected on a later tick from SOEM's rx
         * buffer (no waiting). One sendto() -> one g_tx_order push. ---- */
        if (g_diag_enabled) {
            static uint64_t diag_next_tick = 0;
            if (g_diag_io.pending) {
                int r = ecm_diag_soem_collect(&ctx, &g_diag_io, &g_diag_raw_rt,
                                              ts_to_ns(&t_wake), DIAG_MAX_AGE_TICKS);
                if (r < 0) quar_add(g_diag_io.idx, tick);    /* Giai doan 7.4: gave up on it */
                if (r != 0) {
                    g_diag_raw_rt.ngroups = 2;
                    g_diag_raw_rt.wkc[0] = motion.wkcs;
                    g_diag_raw_rt.wkc[1] = io.wkcs;
                    g_diag_raw_rt.handoff_drops = g_diag_ho.drops;
                    ecm_diag_handoff_put(&g_diag_ho, &g_diag_raw_rt);
                }
            } else if (tick >= diag_next_tick && !io_due) {
                if (ecm_diag_soem_send(&ctx, &g_diag_io, 1))
                    tx_order_ring_push_idx(&g_tx_order, tick, (ecm_group_id_t)GROUP_MOTION, g_diag_io.idx);
                diag_next_tick = tick + (uint64_t)ticks_per_snapshot;
            }
        }

        /* Giai doan 7.3 §4 path A: at most one AL control write per tick,
         * never on an IO tick, only with budget left before the deadline. */
        if (!io_due) rt_exec_command(tick);

        } else if (g_bus.state == ECM_BUS_LOST) {
            /* §3 LOST: no process data; probe the bus with one BRD every
             * probe_ticks, within this tick's budget. Inside the telemetry
             * exclusion window, so no tx_order push. */
            if (ecm_bus_probe_due(&g_bus, tick)) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                uint16 al = 0;
                int w = ecx_BRD(&ctx.port, 0, ECT_REG_ALSTAT, sizeof(al), &al,
                                ecm_rx_timeout_us(ts_to_ns(&now), g_tick_deadline_ns, RX_MIN_US,
                                                  (int)motion_cycle_us));
                if (ecm_bus_on_probe(&g_bus, tick, w, &bus_ev)) {
                    ecm_evring_push(&g_ev, &bus_ev);
                    atomic_store_explicit(&g_recover_req, ++recover_gen, memory_order_release);
                }
            }
        } else {
            /* §5.2 RECOVER: the monitor owns the bus. Send nothing; wait for
             * the matching generation. */
            if (atomic_load_explicit(&g_recover_done, memory_order_acquire) == recover_gen) {
                int ok = atomic_load_explicit(&g_recover_ok, memory_order_relaxed);
                if (ecm_bus_on_recover_done(&g_bus, tick, ok, &bus_ev)) {
                    ecm_evring_push(&g_ev, &bus_ev);
                    if (bus_ev.to == ECM_BUS_RUN) excl_end();
                }
            }
        }
        atomic_store_explicit(&g_bus_state_pub, (int)g_bus.state, memory_order_relaxed);

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

    /* Giai doan 7.4 (found in the 30 min soak, 25/9): leave OP the moment
     * process data stops. Printing the statistics and joining the threads
     * below took ~170 ms, during which the slaves were still in OP without
     * outputs: every one of them tripped its SM watchdog and latched
     * SAFEOP+ERR 0x001B just before INIT was requested. A real slave keeps
     * that error until it is acknowledged. Frames from here on are not in
     * the tx_order ring: telemetry exclusion window. */
    excl_begin();
    if (!request_all_state(EC_STATE_SAFE_OP, EC_TIMEOUTSTATE))
        fprintf(stderr, "ecm_run: shutdown: not every slave reached SAFE-OP\n");

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

    /* ---- Giai doan 7.2: final diagnostics (monitor has joined) ---- */
    if (g_diag_enabled) {
        static ecm_diag_finding_t f[128];
        int nf = ecm_diag_analyze(&g_diag, f, 128);
        fprintf(stderr, "  [DIAG] reads=%" PRIu64 " diag_frames_sent=%" PRIu64 " lost=%" PRIu64
                " foreign_replies=%" PRIu64 " handoff_drops=%" PRIu64 " findings=%d (snapshot: %s)\n",
                g_diag.reads, g_diag_io.frames_sent, g_diag_io.frames_lost, g_diag_io.foreign,
                g_diag_ho.drops, nf, g_diag_path);
        for (int i = 0; i < nf; i++) {
            char line[256];
            ecm_diag_finding_str(&f[i], al_code_str, line, sizeof(line));
            fprintf(stderr, "  [DIAG] %s\n", line);
        }
        for (int g = 0; g < 2; g++) {
            const group_stats_t *gs = g ? &io : &motion;
            fprintf(stderr, "  [WKC %s] ok=%" PRIu64 " noframe=%" PRIu64 " zero=%" PRIu64
                    " partial=%" PRIu64 " over=%" PRIu64 " max_run_bad=%u\n", gs->label,
                    gs->wkcs.count[ECM_WKC_OK], gs->wkcs.count[ECM_WKC_NOFRAME],
                    gs->wkcs.count[ECM_WKC_ZERO], gs->wkcs.count[ECM_WKC_PARTIAL],
                    gs->wkcs.count[ECM_WKC_OVER], gs->wkcs.max_run_bad);
        }
    }

    /* ---- Giai doan 7.4: late/stale replies summary ---- */
    fprintf(stderr, "  [LATE] index quarantine %s: parked=%" PRIu64 " early_release=%" PRIu64
            "; motion replies rejected by the DC age gate=%" PRIu64 " (gate resyncs=%" PRIu64 ")\n",
            g_quarantine ? "on" : "OFF", g_quar_total, g_quar_overflow, g_stale_replies,
            g_dc_enabled ? g_dc.gate_resyncs : 0);
    fprintf(stderr, "  [LATE] replies whose datagram headers differ from the frame sent (another frame's "
            "reply through a reused index): motion=%" PRIu64 " io=%" PRIu64 "%s\n",
            g_foreign_replies[GROUP_MOTION], g_foreign_replies[GROUP_IO],
            g_reply_check ? "" : "  (--no-reply-check: counted, NOT rejected)");
    if (g_fresh_off >= 0) {
        uint64_t reg = 0, eps = 0, n = 0;
        for (int s = 1; s <= ctx.slavecount && s <= ECM_SREC_MAX_SLAVES; s++) {
            const ecm_fresh_t *f = &g_fresh[s];
            reg += f->regressions; eps += f->stale_episodes; n += f->samples;
            if (f->regressions || f->stale_episodes)
                fprintf(stderr, "  [FRESH] slave %d: samples=%" PRIu64 " stale_episodes=%" PRIu64
                        " stale_cycles=%" PRIu64 " max_unchanged=%u regressions=%" PRIu64 "%s\n",
                        s, f->samples, f->stale_episodes, f->stale_cycles, f->max_run, f->regressions,
                        f->stale ? " STALE at exit" : "");
        }
        fprintf(stderr, "  [FRESH] total: samples=%" PRIu64 " stale_episodes=%" PRIu64
                " regressions=%" PRIu64 "\n", n, eps, reg);
    }

    /* ---- Giai doan 7.3: policy summary (monitor has joined) ---- */
    fprintf(stderr, "  [POLICY] final bus state %s; entered: DEGRADED=%" PRIu64 " LOST=%" PRIu64
            " RECOVER=%" PRIu64 " RUN=%" PRIu64 "; ticks: RUN=%" PRIu64 " DEGRADED=%" PRIu64
            " LOST=%" PRIu64 " RECOVER=%" PRIu64 "\n",
            ecm_bus_state_name(g_bus.state), g_bus.entered[ECM_BUS_DEGRADED], g_bus.entered[ECM_BUS_LOST],
            g_bus.entered[ECM_BUS_RECOVER], g_bus.entered[ECM_BUS_RUN] - 1,
            g_bus.ticks_in[ECM_BUS_RUN], g_bus.ticks_in[ECM_BUS_DEGRADED],
            g_bus.ticks_in[ECM_BUS_LOST], g_bus.ticks_in[ECM_BUS_RECOVER]);
    fprintf(stderr, "  [POLICY] recoveries: full-bus=%" PRIu64 " path-B=%" PRIu64 " cmd_drops=%" PRIu64
            " event_drops=%" PRIu64 "\n", g_recoveries_full, g_recoveries_b, g_cmdq.drops, g_ev.drops);
    for (int i = 0; i < g_srec.n; i++) {
        const ecm_srec_t *r = &g_srec.s[i];
        if (!r->recoveries && !r->unhealthy && !r->attempts) continue;
        fprintf(stderr, "  [POLICY] slave %d: recoveries=%" PRIu64 " ack=%" PRIu64 " op=%" PRIu64
                " reconfig=%" PRIu64 "%s%s\n", i + 1, r->recoveries, r->actions[ECM_SACT_ACK],
                r->actions[ECM_SACT_OP], r->actions[ECM_SACT_RECONFIG],
                r->unhealthy ? " UNHEALTHY at exit" : "", r->failed ? " FAILED" : "");
    }

    request_all_state(EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&ctx);
    return 0;
}