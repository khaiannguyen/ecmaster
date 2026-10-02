/* ==========================================================================
 * ecm_run.c — Real cyclic EtherCAT master, two process-data groups running
 * at different rates on the RT thread (Phase 3 / L3 scope), now
 * instrumented for Phase 4 (5-thread architecture, 4 measured
 * quantities, histograms, turnaround correlation via error queue).
 *
 * ---- Phase 4 additions on top of the Phase 3 file ----
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
 *    more threads are spawned: application and mailbox are inert stubs (no
 *    logic until Phase 5), monitor is a lightweight ~100ms
 *    placeholder, and telemetry is the real consumer of both rings above.
 *    monitor and telemetry are deliberately SEPARATE threads (not merged,
 *    as the very first Phase 4 draft did) -- see the Phase 4
 *    review note on why merging them risks head-of-line blocking once
 *    Phase 9 diagnostics add real SDO/mailbox calls to the monitor.
 *
 * ---- Phase 5 additions on top of the Phase 4 file ----
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
 *    ecx_iserror() itself to avoid adding a second concurrent reader.
 *    Phase 9.7: the ONE reader is the RT thread (elist_drain(), right after
 *    the process data of a RUN/DEGRADED tick). It is also the writer of
 *    every EMCY (they arrive through ecx_mbxinhandler in the same thread),
 *    so EMCY never race. It turns each entry into an event for the
 *    monitor (EMCY -> diag snapshot, SDO abort and other SOEM errors ->
 *    log). The remaining race is the mailbox thread pushing an SDO abort
 *    while the RT thread pops; SOEM's list has no lock, so such an entry
 *    can in rare cases be lost or read twice -- it is a report, never
 *    used for a decision (the SDO caller gets its own return code).
 *
 * ---- Phase 6 additions on top of the Phase 5 file ----
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
 *    Phase 9.5: with --eni the DC configuration is the ENI's, per slave
 *    (dc_arm_slave): which slaves get SYNC0, SYNC1 (AssignActivate 0x0700,
 *    CycleTime1), ShiftTime, and the reference clock -- which must be the
 *    first DC-capable slave on the bus, the one SOEM's ecx_configdc() uses.
 *    Anything else (AssignActivate other than 0x0300/0x0700, sync0 != the
 *    motion cycle, a DC slave that is not DC capable, ENI ref elsewhere) is
 *    refused before SAFE-OP. Without --eni nothing changes.
 *    DC(b) each motion tick: ecm_dc_update(ctx.DCtime, t_wake) returns an
 *    adjustment added to the next absolute deadline. wake_jitter keeps
 *    its meaning because it is measured against that adjusted deadline.
 *    Anchor = ONE FPRD 0x0910..0x0997 on the reference clock.
 *    DC telemetry is RT-thread-only state (no ring change): |e| histogram
 *    and sums read after the loop exits, plus 4 fields in the per-second
 *    snapshot -> same "no I/O in the hot loop" rule as Phase 3.
 *    Enabled automatically when slave 1 is DC-capable; --no-dc forces off.
 *
 * Everything below this point that is unchanged from Phase 4 keeps its
 * original comments; only the file header above and the new/changed code
 * sections are new for Phase 5/6.
 * ========================================================================== */

/* Must come before any system header — CPU_ZERO/CPU_SET/sched_setaffinity
 * are only declared by glibc's <sched.h> when _GNU_SOURCE is defined. */
#define _GNU_SOURCE

#include <math.h>      /* Phase 10.4: test producer sine */
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
#include <linux/ethtool.h>   /* Phase 9.2: link speed -> ETF ns/byte */
#include <linux/errqueue.h>   /* Phase 8.5: SO_EE_ORIGIN_TXTIME */
#include <arpa/inet.h>

#include "soem/soem.h"

#include <sys/mman.h>

#include "libecmaster/telemetry/ecm_groups.h"
#include "libecmaster/telemetry/ring_spsc.h"
#include "libecmaster/telemetry/turnaround.h"
#include "libecmaster/telemetry/histogram.h"
#include "libecmaster/mailbox/ecm_mailbox.h"
#include "libecmaster/core/ecm_dc.h"
#include "libecmaster/diag/ecm_diag.h"     
#include "libecmaster/config/ecm_eni.h"
#include "libecmaster/config/ecm_eni_soem.h"
#include "libecmaster/pdo/ecm_pdo.h"         /* Phase 9.10 */
#include "libecmaster/pdo/ecm_pdo_soem.h"
#include "libecm_cia402/ecm_cia402_cfg.h"   /* Phase 10.3 */
#include "libecmaster/xchg/ecm_xchg.h"         /* Phase 10.4 */
#include "libecm_cia402/ecm_cia402_axis.h"     /* Phase 10.5 */

/* ctx.grouplist[] has EC_MAXGROUP entries (SOEM CMake option, default 2)
 * and this file indexes it with GROUP_IO = 2. With the default, every
 * grouplist[GROUP_IO] access is out of bounds: found in the Phase 7 sandbox
 * (IO group mapped 0 bytes, expected WKC 0). Build SOEM with
 * -DEC_MAXGROUP=4 (or more). */
#if EC_MAXGROUP <= GROUP_IO
#error "SOEM built with EC_MAXGROUP <= GROUP_IO: rebuild SOEM with cmake -DEC_MAXGROUP=4"
#endif
#include "libecmaster/diag/ecm_diag_soem.h"
#include "libecmaster/policy/ecm_policy.h"    /* Phase 7.3 */
#include <stdatomic.h>

#define IOMAP_SIZE   (256 * 1024)   /* see IOMAP_SIZE note in the Phase 3 file header */

static uint8 IOmap_motion[IOMAP_SIZE];
static uint8 IOmap_io[IOMAP_SIZE];
static ecx_contextt ctx;

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ---- Phase 4: telemetry plumbing, global so both the RT thread and
 * the telemetry thread can reach them. The RT thread only ever PUSHES;
 * the telemetry thread only ever POPS -- this is the single-producer/
 * single-consumer contract both ring types are built around. ---- */
static ring_spsc_t     g_ring;       /* rt_sample_t, one push per tick */
static tx_order_ring_t g_tx_order;   /* one push per sendto() call (1 or 2 per tick) */

static volatile sig_atomic_t g_telemetry_stop = 0;
static volatile sig_atomic_t g_app_stop       = 0;
static volatile sig_atomic_t g_mailbox_stop   = 0;
static volatile sig_atomic_t g_monitor_stop   = 0;

/* ---- Phase 5: mailbox subsystem, created in main() once ctx is
 * initialized, used by mailbox_thread_fn() below and by the RT loop
 * (service_group()) via ecm_mailbox_rt_pump_group(). Not touched by
 * any other thread. ---- */
static ecm_mailbox_t *g_mbx = NULL;

/* ---- Phase 6: DC(b). g_dc and g_dcstat are written ONLY by the RT
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

static const char *g_eni_path;
static ecm_eni_t   g_eni;         /* large; file scope, never on the stack */
static int         g_eni_on;
static int         g_eni_allow_unknown_reg;   /* Phase 9.6: --eni-allow-unknown-regcmd */
static int         g_sdo_timeout_ms;          /* Phase 9.6: --sdo-timeout-ms, 0 = EC_TIMEOUTRXM */

/* Phase 9.10: process data by (slave, index, sub). The table comes from the ENI
 * (.enicfg "pdo" records) or from a scan of the bus (--pdo-scan); with both,
 * they must agree. --pdo-set / --pdo-get exercise ecm_pdo_bind() end to end:
 * the RT thread writes the set values into the outputs every tick (it owns
 * the IOmap), the gets are printed at exit. */
#define PDO_MAX_REFS 16
static int              g_pdo_scan;
static int              g_pdo_dump;                 /* --pdo-dump: print the table in use */
static ecm_pdo_table_t  g_pdo_eni, g_pdo_bus;
static const ecm_pdo_table_t *g_pdo;           /* the one in use, or NULL */
static ecm_pdo_loc_t    g_pdo_loc[ECM_PDO_MAX_SLAVES + 1];
static const char      *g_pdo_set_spec, *g_pdo_get_spec;
static int              g_pdo_nset, g_pdo_nget;
static ecm_pdo_handle_t g_pdo_set_h[PDO_MAX_REFS], g_pdo_get_h[PDO_MAX_REFS];
static uint64_t         g_pdo_set_v[PDO_MAX_REFS];
static char             g_pdo_get_name[PDO_MAX_REFS][32];
/* Phase 10.1: binding into a slave of another vendor needs --pdo-scan (N-03:
 * a mapping that differs from the ESI but has the same size is invisible
 * to every size check). Own vendors: --pdo-own-vendor; --pdo-trust-eni is
 * the explicit, logged way to bind from the ENI alone anyway. */
#define PDO_MAX_OWN 8
static uint32_t         g_pdo_own[PDO_MAX_OWN] = { ECM_PDO_OWN_VENDOR_DEFAULT };
static int              g_pdo_nown = 1;
static int              g_pdo_trust_eni;

/* Phase 10.3: CiA402 axes (libecm_cia402). Configured and checked at PREOP:
 * every object the requested modes need is bound (ecm_pdo_bind), the drive's
 * 0x6502 must list every mode, and an axis on another vendor's slave needs
 * the scanned PDO table (Phase 10.1 rule). Nothing cyclic yet (10.4/10.5). */
static ecm_axis_cfg_t   g_axes[ECM_AXIS_MAX];
static ecm_axis_bind_t  g_axis_b[ECM_AXIS_MAX];
static int              g_naxes;
static const char      *g_axis_list, *g_axis_modes_s = "csp", *g_axes_cfg;
static int              g_axis_no6502;

/* Phase 10.4: cyclic hook in the RT thread + app <-> RT exchange
 * (docs/app_rt_exchange.md). The hook runs once per tick, after the motion
 * receive and DC(b), before the next send; it gets the motion IOmap (the RT
 * thread stays its only owner). Nothing registered -> one predictable
 * branch, no datagram changes (Q-01). --hook empty measures the bare cost
 * (Q-02); --hook xchg runs ecm_xchg_rt() on --xchg-out / --xchg-in slots,
 * fed by a test producer in the application thread (--xchg-sine, Q-03/Q-04). */
typedef struct {
    uint8_t *iomap;            /* GROUP_MOTION IOmap                          */
    uint64_t tick;
    uint64_t t_send_ns;        /* send (af_packet) or launch (etf) time        */
    int      in_valid;         /* this tick's motion reply was accepted (WKC)  */
    int      bus_lost;         /* no process data this tick                    */
} ecm_hook_args_t;
typedef void (*ecm_hook_fn)(const ecm_hook_args_t *a, void *ctx);
static ecm_hook_fn      g_hook;
static void            *g_hook_ctx;
static const char      *g_hook_name;
static ecm_hist_t       g_hook_hist;
static uint64_t         g_hook_calls;
static ecm_xchg_t       g_xchg;              /* ~600 KB, static, locked by mlockall */
static const char      *g_xchg_out_spec, *g_xchg_in_spec;
static double           g_xchg_amp = 10000.0, g_xchg_hz = 1.0;
static int              g_xchg_sine, g_xchg_lead = 4;
static long             g_xchg_starve_s = -1, g_xchg_starve_ms;
static int              g_xchg_echo_off;     /* test only: shift the echo's expected tick (negative control) */
static struct {                               /* written by the app thread only */
    uint64_t pushed, full, gaps, checked, mismatch, torn, starve_from, starve_to;
    struct { uint64_t tick, got, want, used, under; } mm[4];   /* first mismatches */
} g_xapp;

/* Phase 10.5: the CiA402 layer's hook (libecm_cia402) and its test driver:
 * --cia402-script "T CMD ARGS; ..." run by the application thread, T in
 * seconds after the first published tick. Commands: enable|disable|
 * quickstop|reset AXIS|all, mode AXIS csp|csv, sine AXIS AMP HZ (CSP,
 * around the actual position at that moment), vel AXIS V (CSV), pos AXIS P
 * (CSP, absolute target P every tick: Phase 10.6 S4 test), stop AXIS
 * (no more setpoints). */
static ecm_cia402_t     g_cia;              /* static, locked by mlockall */
static const char      *g_cia_script;
static uint32_t         g_cia_step_ms = 500;
static int              g_cia_lead = 4;
static struct {                              /* app thread only */
    int64_t  track_max[ECM_AXIS_MAX];
    uint64_t track_n[ECM_AXIS_MAX], pushed, full, events;
} g_capp;

static void hook_empty(const ecm_hook_args_t *a, void *ctx) { (void)a; (void)ctx; }
static int64_t          g_cia_max_pos = 100000, g_cia_max_vel = 0;   /* Phase 10.6 S4, per cycle */
static int              g_cia_bus_lost;     /* RT only: last bus_lost given to the hook */
static struct {                              /* Phase 10.6 S6, RT only until the loop ends */
    int      state;                          /* 0 running, 1 walking down, 2 down, 3 timeout */
    uint64_t t0, ticks, max_ticks;
} g_cstop;

static void hook_cia402(const ecm_hook_args_t *a, void *ctx)
{
    g_cia_bus_lost = a->bus_lost;
    ecm_cia402_rt((ecm_cia402_t *)ctx, a->iomap, a->tick, a->t_send_ns, a->in_valid, a->bus_lost);
}

/* Phase 10.6 S6: the loop was asked to end (SIGINT, SIGTERM, --duration-sec).
 * With the CiA402 hook every axis is walked down first (Disable operation
 * 0x07 -> Shutdown 0x06 -> Disable voltage 0x00) before the slaves leave OP;
 * returns 1 while the loop must keep cycling. RT thread, no I/O. */
static int cia402_keep_cycling(uint64_t tick)
{
    if (g_hook != hook_cia402) return 0;
    switch (g_cstop.state) {
    case 0:
        ecm_cia402_shutdown(&g_cia);
        g_cstop.state = 1;
        g_cstop.t0 = tick;
        /* fall through */
    case 1:
        if (ecm_cia402_all_down(&g_cia, g_cia_bus_lost)) { g_cstop.state = 2; g_cstop.ticks = tick - g_cstop.t0; return 0; }
        if (tick - g_cstop.t0 >= g_cstop.max_ticks) { g_cstop.state = 3; g_cstop.ticks = tick - g_cstop.t0; return 0; }
        return 1;
    default:
        return 0;
    }
}
static void hook_xchg(const ecm_hook_args_t *a, void *ctx)
{
    ecm_xchg_rt((ecm_xchg_t *)ctx, a->iomap, a->tick, a->t_send_ns, a->in_valid, a->bus_lost);
}

/* the test producer's setpoint for slot s at tick t (also its echo check) */
static int64_t xchg_sine_value(int s, uint64_t t)
{
    double ph = 2.0 * M_PI * g_xchg_hz * (double)t * (double)g_xchg.cycle_ns * 1e-9;
    return (int64_t)llround(g_xchg_amp * sin(ph)) + 1000 * s;
}

/* Phase 9.6: how long to wait for a bus state. SOEM's EC_TIMEOUTSTATE (2 s)
 * unless the ENI gives a slave a longer one (the Timeout of its AL Control
 * write = the ESI's state machine timeout; IS620N: SAFE-OP->OP 9 s).
 * Never shorter than before, so a bus that came up still comes up. */
static int state_timeout_us(int st)
{
    if (!g_eni_on)
        return EC_TIMEOUTSTATE;
    long us = (long)ecm_eni_state_timeout_ms(&g_eni, st) * 1000L;
    return us > EC_TIMEOUTSTATE ? (int)us : EC_TIMEOUTSTATE;
}

/* Opened in main(), BEFORE any thread is created -- see the Phase 4
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

/* ---- Phase 7.2: diagnostics (plan §3.1/§3.2) ----
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

/* ---- Phase 7.3: fault policy (docs/fault_policy.md) ----
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
static uint64_t         g_emcy_ev_drops;       /* Phase 9.7: RT only, EMCY events the ring refused */
static atomic_ullong    g_emcy_ev_drops_pub;   /* Phase 9.7: RT publishes, monitor reads */
static int              g_emcy_print_max = 20; /* Phase 9.7: EMCY log lines per slave, then counted only */
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

/* ---- Phase 7.4: late / duplicate / stale replies (L5-07/08/09) ----
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
static int      g_fresh_off   = -1;             /* --fresh-offset N: every slave, 16 bit (Phase 7.4 alias) */
/* Phase 9.8: freshness per slave. The application counter is part of each
 * slave's own PDO contract, so where it is (and whether there is one) is
 * per slave: --fresh "all=0,2=off,5=4:8". g_fresh_on[s] = 0 means the
 * master cannot tell "WKC correct but data old" for that slave. */
static const char *g_fresh_spec;                 /* --fresh LIST */
static uint8_t  g_fresh_on[ECM_SREC_MAX_SLAVES + 1];
static uint16_t g_fresh_byte[ECM_SREC_MAX_SLAVES + 1];
static uint8_t  g_fresh_bits[ECM_SREC_MAX_SLAVES + 1];
static int      g_fresh_any;                     /* at least one slave checked */
static uint32_t g_fresh_stale = 20;             /* --fresh-stale N (cycles) */
static ecm_fresh_t g_fresh[ECM_SREC_MAX_SLAVES + 1];   /* RT only, [SOEM slave] */
static uint64_t g_stale_replies;                /* motion replies rejected by the DC age gate */
static int      g_reply_check = 1;              /* --no-reply-check (negative control): count, don't act */
static uint64_t g_foreign_replies[3];           /* [group]: reply header != what was sent */

/* ---- Phase 9.1: group of every slave, from the command line ----
 * Until Phase 8 slaves 1..k were GROUP_MOTION and k+1..n GROUP_IO, with
 * 0 < k < n: a bus of ONE slave (P1/P2/P4: one LAN9252) or of motion
 * slaves only (own slave + a commercial servo) could not run. Now:
 *   default               every slave GROUP_MOTION, GROUP_IO empty
 *   --io-slaves LIST      those positions GROUP_IO ("5-8", "2,4", "none")
 *   --motion-slaves k     kept as before: 1..k motion, the rest IO
 * GROUP_MOTION must not be empty: it carries the tick, the FRMW for DC(b)
 * and the diagnostics. An empty GROUP_IO is never sent or received: SOEM
 * sends nothing for a group of length 0 but its receive then returns
 * EC_NOFRAME, which the fault policy would count as a lost IO frame. */
static uint8_t  g_is_io[EC_MAXSLAVE + 1];       /* [SOEM slave], 1 = GROUP_IO */
static int      g_io_active;                    /* GROUP_IO has at least one slave */

/* "none" | comma list of positions or ranges, e.g. "5-8" or "2,4,6-7" */
static int parse_slave_list(const char *s, uint8_t *mark, int max, char *err, size_t errsz)
{
    memset(mark, 0, (size_t)max + 1);
    if (strcmp(s, "none") == 0) return 0;
    const char *p = s;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p) { snprintf(err, errsz, "'%s': expected a slave position at '%s'", s, p); return -1; }
        p = end;
        if (*p == '-') {
            p++;
            b = strtol(p, &end, 10);
            if (end == p) { snprintf(err, errsz, "'%s': expected the end of a range at '%s'", s, p); return -1; }
            p = end;
        }
        if (a < 1 || b < a || b > max) {
            snprintf(err, errsz, "'%s': position %ld-%ld outside 1..%d", s, a, b, max);
            return -1;
        }
        for (long i = a; i <= b; i++) mark[i] = 1;
        if (*p == ',') p++;
        else if (*p) { snprintf(err, errsz, "'%s': unexpected '%c'", s, *p); return -1; }
    }
    return 0;
}

/* One process-data exchange of every non-empty group, blocking, outside
 * the RT loop (before OP, keeping the watchdog fed, recovery). Order kept
 * from Phase 3: motion send/receive, then IO send/receive. */
static void pd_exchange_all(int timeout_us)
{
    ecx_send_processdata_group(&ctx, GROUP_MOTION);
    ecx_receive_processdata_group(&ctx, GROUP_MOTION, timeout_us);
    if (g_io_active) {
        ecx_send_processdata_group(&ctx, GROUP_IO);
        ecx_receive_processdata_group(&ctx, GROUP_IO, timeout_us);
    }
}

/* ---- Phase 8.5 (R-02): --link etf (patches/soem-txtime.patch) ----
 * `next` (CLOCK_MONOTONIC) stays the tick's target. With --link etf the RT
 * thread wakes g_etf_lead_us earlier and gives the motion frame
 * SCM_TXTIME = next + (TAI - MONO); the NIC launches it at `next` (ETF
 * offload). Every other frame leaves at once on SO_PRIORITY 0, a queue
 * without ETF (SOEM v2, needs mqprio; a root ETF drops them). lead < ETF
 * delta: the sender dequeues the frame itself, no qdisc hrtimer involved. */
#ifndef SO_EE_ORIGIN_TXTIME
#define SO_EE_ORIGIN_TXTIME 6
#define SO_EE_CODE_TXTIME_INVALID_PARAM 1
#define SO_EE_CODE_TXTIME_MISSED 2
#endif
#define TAI_STEP_NS 100000                       /* TAI - MONO moved more than this in one tick */
static int         g_link_etf        = 0;
static long        g_etf_lead_us     = 200;
static long        g_etf_asap_us     = 150;
static int         g_etf_prio        = 3;
static int         g_etf_ns_per_byte = 0;        /* 0 = from the link speed (Phase 9.2): 8 at 1 Gbit/s, 80 at 100 Mbit/s */
static uint64_t    g_tai_steps;                  /* RT only */
static int64_t     g_tai_step_max;               /* RT only, ns */
static atomic_ulong g_etf_missed, g_etf_invalid, g_etf_other;   /* telemetry thread */

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
    ecm_wkc_stats_t wkcs;          /* Phase 7.2: WKC by class (NOFRAME/ZERO/PARTIAL/OVER) */
    int          last_idx;         /* Phase 7.4: EtherCAT index of the last frame sent */
    uint8_t      in_snap[256];     /* Phase 7.4: inputs before this receive, restored  */
    uint32_t     in_snap_len;      /*   when the reply turns out to be an old one           */
    uint64_t     last_send_ns;     /* Phase 7.4: CLOCK_MONOTONIC right after sendto()    */
    int          reply_foreign;    /* Phase 7.4: this cycle's reply belonged to another frame */
} group_stats_t;

/* Made file-scope (not local to main()) so telemetry_thread_fn's final
 * reconcile print can read their .cycles totals -- safe to read there
 * without synchronization because telemetry only reads them in its own
 * shutdown path, which main() only triggers (g_telemetry_stop = 1) AFTER
 * the RT loop has already exited and stopped writing to them. */
static group_stats_t motion;
static group_stats_t io;


/* One snapshot per second of wall-clock progress, written with NO I/O
 * inside the hot loop -- see the Phase 3 file header note on why this
 * exists. */
typedef struct {
    uint64_t tick;
    uint64_t motion_cycles, motion_mismatch, motion_overrun;
    uint64_t io_cycles,     io_mismatch,     io_overrun;
    /* Phase 6 */
    int      dc_state;             /* ecm_dc_state_t */
    int64_t  dc_err_ns;            /* last phase error */
    int64_t  dc_sum_u;             /* cumulative adjust, all samples */
    uint64_t dc_samples, dc_wraps, dc_unlocks;
} snapshot_t;

/* Phase 10.1: 32768 = ~9.1 h at one snapshot/second, so an 8 h soak can still
 * say WHEN a mismatch happened (4096 = 68 min stopped recording after the
 * first hour). ~104 byte each: ~3.4 MB of BSS, locked by mlockall like the
 * rest; written once per second, never on the cyclic path. */
#define MAX_SNAPSHOTS 32768
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

/* Phase 8.5 */
static void ns_to_ts(uint64_t ns, struct timespec *t)
{
    t->tv_sec  = (time_t)(ns / 1000000000ULL);
    t->tv_nsec = (long)(ns % 1000000000ULL);
}

/* TAI - MONO: midpoint of TAI, MONO, TAI, narrowest of 2 (clock_check rule) */
static int64_t tai_minus_mono_ns(void)
{
    int64_t best = 0, best_w = INT64_MAX;
    for (int i = 0; i < 2; i++) {
        struct timespec a, m, b;
        clock_gettime(CLOCK_TAI, &a);
        clock_gettime(CLOCK_MONOTONIC, &m);
        clock_gettime(CLOCK_TAI, &b);
        int64_t w = ts_diff_ns(&b, &a);
        if (w < best_w) { best_w = w; best = (int64_t)ts_to_ns(&a) + w / 2 - (int64_t)ts_to_ns(&m); }
    }
    return best;
}

/* Startup only: is there an ETF qdisc with offload on this interface? */
/* Phase 9.2: link speed in Mbit/s from the driver (ETHTOOL_GSET), -1 if unknown
 * (veth reports SPEED_UNKNOWN). Used to pick the ETF ns/byte: at 100 Mbit/s
 * a byte takes 80 ns, ten times the 1 Gbit/s value the frames were spaced
 * with before, so back-to-back txtimes would overlap on the wire. */
static __attribute__((unused)) int link_speed_mbps(const char *ifname)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct ethtool_cmd ec = { .cmd = ETHTOOL_GSET };
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    ifr.ifr_data = (char *)&ec;
    int rc = ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
    if (rc < 0) return -1;
    uint32_t sp = ethtool_cmd_speed(&ec);
    return (sp == 0 || sp == (uint32_t)SPEED_UNKNOWN) ? -1 : (int)sp;
}

static int etf_qdisc_state(const char *ifname)
{
    char cmd[128], line[512];
    int etf = 0, off = 0;
    snprintf(cmd, sizeof(cmd), "tc qdisc show dev %s 2>/dev/null", ifname);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof(line), p)) {
        if (strstr(line, "qdisc etf")) { etf = 1; if (strstr(line, "offload on")) off = 1; }
    }
    pclose(p);
    return etf ? (off ? 2 : 1) : 0;
}

/* Requests ALL slaves (slave=0 broadcasts) into `target`, blocks until
 * reached or timeout. Reused pattern from l2_probe.c's request_state(),
 * generalized to slave=0 since this file operates on the whole bus, not
 * a single slave under test. */
static int request_all_state(int target, int timeout_us)
{
    ctx.slavelist[0].state = target;
    ecx_writestate(&ctx, 0);
    /* Phase 9.7 fix: ecx_statecheck() returns the state it READ (BRD: the OR
     * of every slave's), not success. Up to Phase 9.6 this function returned
     * it as is, so a bus with one slave stuck in PREOP (0x02 | 0x04 = 0x06)
     * counted as "reached SAFEOP" and ecm_run went on to request OP. */
    return ecx_statecheck(&ctx, 0, target, timeout_us) == target;
}

/* Same as request_all_state(EC_STATE_OPERATIONAL, ...), except it keeps the
 * process-data watchdog fed while it waits: ecx_statecheck() only polls AL
 * status over the mailbox/FPRD path, it never sends process data, and each
 * of its internal retries already costs EC_TIMEOUTRET. Observed on the real
 * Jetson 2026-09-25: with GROUP_MOTION's SM watchdog at 3ms (see WD_TIME_*
 * above), slaves 1-4 latch SAFEOP+ERR / 0x001B (Sync manager watchdog) the
 * instant OP is requested -- every run, before any fault injection -- and
 * since there is no auto-recovery yet (Phase 7.3), they stay latched.
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
        pd_exchange_all(EC_TIMEOUTRET);   /* Phase 9.1: skips an empty GROUP_IO */

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
 * Phase 4 additions: pushes (tick, group) into g_tx_order right after
 * sendto() so the telemetry thread can later match the async TX
 * completion; and exposes three timing values via out-params (any of
 * which may be NULL if the caller doesn't need them) so the caller can
 * build this tick's rt_sample_t without duplicating the timing logic.
 * wkc_mismatch/overrun accounting is UNCHANGED from Phase 3 -- t0 is
 * still captured fresh at the start of this function, exactly as before,
 * so those two stats keep their exact original meaning. */
/* Phase 7.4 (L5-07): does the reply now in rxbuf[idx] carry the same
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
    /* Phase 7.4: index of the frame just sent (one frame per group
     * here), for index-keyed turnaround pairing and index quarantine. */
    int sent_idx = ctx.idxstack.pushed > 0 ? ctx.idxstack.idx[ctx.idxstack.pushed - 1] : -1;
    g->last_idx = sent_idx;
    tx_order_ring_push_idx(&g_tx_order, tick, (ecm_group_id_t)g->group,
                           sent_idx >= 0 ? (uint8_t)sent_idx : (uint8_t)TX_ORDER_IDX_UNKNOWN);

    clock_gettime(CLOCK_MONOTONIC, &t_after_send);
    g->last_send_ns = ts_to_ns(&t_after_send);
    if (out_prep_send_ns) *out_prep_send_ns = ts_diff_ns(&t_after_send, &t0);

    /* Phase 7.3 §2: the receive must end before the tick's deadline.
     * EC_TIMEOUTRET (2000 us) is twice the 1 ms cycle: one lost frame used
     * to cost 2-4 ms and turn into a chain of overruns. */
    int rx_timeout_us = g_rx_legacy ? EC_TIMEOUTRET
        : ecm_rx_timeout_us(ts_to_ns(&t_after_send), g_tick_deadline_ns, RX_MIN_US, (int)g_motion_cycle_us);
    {   /* Phase 7.4: keep the inputs as they were, in case this reply
         * turns out to be an old one (see g_stale_replies) */
        uint32_t ib = ctx.grouplist[g->group].Ibytes;
        g->in_snap_len = ib <= sizeof(g->in_snap) ? ib : 0;
        if (g->in_snap_len) memcpy(g->in_snap, ctx.grouplist[g->group].inputs, g->in_snap_len);
    }
    int wkc = ecx_receive_processdata_group(&ctx, g->group, rx_timeout_us);
    /* Phase 7.4: right after the receive, before anything else can
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
    if (out_wkc) *out_wkc = wkc;   /* Phase 6: DC(b) only trusts ctx.DCtime if the frame came back */

    /* Phase 5: pump any pending mailbox I/O queued for this group
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
    ecm_wkc_account(&g->wkcs, wkc, (int)g->expected_wkc);   /* Phase 7.2, no syscalls */
    if (elapsed_ns > g->cycle_ns) g->overrun++;
}

/* Phase 9.7: the only reader of SOEM's error list while the threads run (see
 * the file header, point on elist). RT-safe: no syscalls, copies only, and
 * the cheap ecaterror check keeps an empty list at one load per tick. */
static void elist_drain(uint64_t tick)
{
    if (!ctx.ecaterror) return;
    ec_errort er;
    while (ecx_poperror(&ctx, &er)) {
        ecm_event_t e = { .tick = tick, .from = (uint8_t)(er.Slave > 255 ? 255 : er.Slave) };
        switch (er.Etype) {
        case EC_ERR_TYPE_EMERGENCY:
            e.type = ECM_EV_EMCY;
            e.a = (int32_t)((uint32_t)er.ErrorCode | (uint32_t)er.ErrorReg << 16 | (uint32_t)er.b1 << 24);
            e.b = (int32_t)((uint32_t)er.w1 | (uint32_t)er.w2 << 16);
            break;
        case EC_ERR_TYPE_SDO_ERROR:
            e.type = ECM_EV_SDO_ABORT;
            e.a = er.AbortCode;
            e.b = (int32_t)((uint32_t)er.Index << 8 | er.SubIdx);
            break;
        default:
            e.type = ECM_EV_SOEM_ERR;
            e.a = (int32_t)er.Etype;
            e.b = (int32_t)er.ErrorCode;
            break;
        }
        if (!ecm_evring_push(&g_ev, &e) && e.type == ECM_EV_EMCY) {
            g_emcy_ev_drops++;
            atomic_store_explicit(&g_emcy_ev_drops_pub, g_emcy_ev_drops, memory_order_relaxed);
        }
    }
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

/* ---- SM watchdog and DC anchor as functions (Phase 7.3): also used
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

/* Phase 9.5: arm SYNC0 (or SYNC0 + SYNC1) of one slave. Without --eni:
 * SYNC0 = motion cycle, shift 0, as since Phase 6. With --eni: the slave's own
 * AssignActivate (0x0300 SYNC0, 0x0700 SYNC0 + SYNC1), CycleTime1 (0x09A4,
 * SYNC1 relative to SYNC0) and ShiftTime. Startup and RECOVERY both come
 * here, so a power-cycled slave gets back exactly what the ENI asked. */
static void dc_arm_slave(int s)
{
    int32 shift = g_eni_on ? g_eni.slave[s - 1].shift_ns : 0;
    if (g_eni_on && (g_eni.slave[s - 1].assign & 0x0400))
        ecx_dcsync01(&ctx, (uint16)s, TRUE, (uint32)g_dc_cyc_ns, g_eni.slave[s - 1].sync1_ns, shift);
    else
        ecx_dcsync0(&ctx, (uint16)s, TRUE, (uint32)g_dc_cyc_ns, shift);
}
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
    /* Phase 7.4: reply-age gate on. Valid here because every receive
     * ends by the tick deadline (< 1 cycle after the send) and the DC
     * update gets the send time (see ecm_dc.h). */
    dcfg.gate_ns = g_dc_cyc_ns;
    ecm_dc_init(&g_dc, &dcfg);
    ecm_dc_anchor(&g_dc, dc_raw, s0_raw, ts_to_ns(&t_anchor));
    return w;
}

/* ---- Phase 7.3: telemetry exclusion window (docs/fault_policy.md
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

/* ---- Phase 7.4: index quarantine (RT thread) ---- */
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

/* ---- Phase 7.4: input freshness, one slave (RT thread, no syscalls) ---- */
/* Phase 9.8: "all=0,2=off,5=4:8" -> per slave on/byte/bits. Entries are
 * applied left to right, a later one overrides; ranges "3-6=..." allowed.
 * Value: "off", or BYTE[:BITS] with BITS 8, 16 (default) or 32. */
static int fresh_parse(const char *spec, int n, char *err, size_t errsz)
{
    char buf[512];
    if (strlen(spec) >= sizeof(buf)) { snprintf(err, errsz, "too long"); return -1; }
    strcpy(buf, spec);
    for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) { snprintf(err, errsz, "'%s': expected SLAVE=VALUE", t); return -1; }
        *eq = '\0';
        int lo, hi;
        if (strcmp(t, "all") == 0) { lo = 1; hi = n; }
        else {
            char *end;
            lo = (int)strtol(t, &end, 10); hi = lo;
            if (*end == '-') hi = (int)strtol(end + 1, &end, 10);
            if (*end || lo < 1 || hi < lo || hi > n || hi > ECM_SREC_MAX_SLAVES) {
                snprintf(err, errsz, "'%s': slave(s) must be within 1..%d", t, n); return -1;
            }
        }
        const char *v = eq + 1;
        int on = 1, byte = 0, bits = 16;
        if (strcmp(v, "off") == 0) on = 0;
        else {
            char *end;
            byte = (int)strtol(v, &end, 10);
            if (end == v || byte < 0 || byte > 4095) { snprintf(err, errsz, "'%s': expected off or BYTE[:8|16|32]", v); return -1; }
            if (*end == ':') bits = (int)strtol(end + 1, &end, 10);
            if (*end || (bits != 8 && bits != 16 && bits != 32)) {
                snprintf(err, errsz, "'%s': expected off or BYTE[:8|16|32]", v); return -1;
            }
        }
        for (int s = lo; s <= hi; s++) {
            g_fresh_on[s] = (uint8_t)on; g_fresh_byte[s] = (uint16_t)byte; g_fresh_bits[s] = (uint8_t)bits;
        }
    }
    return 0;
}

static void fresh_feed(int s, uint64_t tick)
{
    const ec_slavet *sl = &ctx.slavelist[s];
    if (s > ECM_SREC_MAX_SLAVES || !g_fresh_on[s] || !sl->inputs) return;
    const uint8_t *p = sl->inputs + g_fresh_byte[s];   /* range checked at startup */
    uint32_t v = p[0];
    if (g_fresh_bits[s] >= 16) v |= (uint32_t)p[1] << 8;
    if (g_fresh_bits[s] == 32) v |= (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
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
 * Phase 4: the four non-RT threads.
 *
 * IMPORTANT correctness note: pthread_create() defaults to
 * PTHREAD_INHERIT_SCHED, meaning a new thread inherits the CREATING
 * thread's scheduling policy/priority. Since main() (the RT thread) will
 * be running under SCHED_FIFO 80 by the time these are spawned, every one
 * of these threads MUST explicitly switch itself to SCHED_OTHER on entry,
 * or it would silently keep SCHED_FIFO 80 -- exactly the kind of
 * mis-priority bug that would surface much later as an unexplained
 * latency spike. isolcpus=3 (Phase 1) already keeps these threads off
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

/* ---- Phase 4, RX side (option (b), confirmed 22/9): a SEPARATE,
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

/* Phase 7.4: 2 ms (was 10 ms). Index pairing gives up on a send after
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
    /* Phase 7.2 finding (sandbox, kernel 6.18, veth): with TX
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
    unsigned excl_seen_gen = 0;                 /* Phase 7.3 exclusion window */
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

        /* Phase 7.3 (docs/fault_policy.md §5.3): while another thread
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

        /* Phase 8.5: --no-tx-ts but --link etf: ETF drop reports still
         * land on SOEM's error queue. Drain them (a non-empty error queue
         * keeps POLLERR set and turns SOEM's ppoll() into a busy loop). */
        if (soem_raw_fd < 0 && g_link_etf) {
            int fd = ctx.port.sockhandle;
            struct pollfd pfd = { .fd = fd, .events = 0 };
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLERR)) {
                char control[256], buf[64];
                struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
                struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                                      .msg_control = control, .msg_controllen = sizeof(control) };
                if (recvmsg(fd, &msg, MSG_ERRQUEUE) < 0) break;
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                    if (cm->cmsg_level != SOL_PACKET || cm->cmsg_type != PACKET_TX_TIMESTAMP) continue;
                    const struct sock_extended_err *ee = (const struct sock_extended_err *)CMSG_DATA(cm);
                    if (ee->ee_origin != SO_EE_ORIGIN_TXTIME) continue;
                    if (ee->ee_code == SO_EE_CODE_TXTIME_MISSED) atomic_fetch_add(&g_etf_missed, 1);
                    else if (ee->ee_code == SO_EE_CODE_TXTIME_INVALID_PARAM) atomic_fetch_add(&g_etf_invalid, 1);
                    else atomic_fetch_add(&g_etf_other, 1);
                }
            }
        }

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
                /* Phase 7.4: the error queue returns the sent frame
                 * itself -> pair by its EtherCAT index, not by position */
                int tx_idx = turnaround_frame_idx((const uint8_t *)buf, (size_t)tr);

                int etf_err = 0;   /* Phase 8.5: an ETF drop report, not a TX completion */
                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_PACKET && cmsg->cmsg_type == PACKET_TX_TIMESTAMP) {
                        const struct sock_extended_err *ee = (const struct sock_extended_err *)CMSG_DATA(cmsg);
                        if (ee->ee_origin == SO_EE_ORIGIN_TXTIME) {
                            etf_err = 1;
                            if (ee->ee_code == SO_EE_CODE_TXTIME_MISSED) atomic_fetch_add(&g_etf_missed, 1);
                            else if (ee->ee_code == SO_EE_CODE_TXTIME_INVALID_PARAM) atomic_fetch_add(&g_etf_invalid, 1);
                            else atomic_fetch_add(&g_etf_other, 1);
                        }
                    }
                }
                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg && !etf_err; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
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
                        /* Phase 7.4: pair by EtherCAT index (duplicates,
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
    fprintf(stderr, "\n=== Phase 4 telemetry (final) ===\n");
    fprintf(stderr, "  ring drop_count=%" PRIu64 "  turnaround: matched=%" PRIu64
                    " implausible=%" PRIu64 " tx_io_discarded=%" PRIu64 " rx_io_discarded=%" PRIu64
                    " evicted_no_match=%" PRIu64 " completion_no_pending=%" PRIu64
                    " rx_no_pending=%" PRIu64 "\n",
            g_ring.drop_count, g_telemetry.turnaround.stat_matched, g_telemetry.turnaround.stat_implausible,
            g_telemetry.turnaround.stat_tx_io_discarded, g_telemetry.turnaround.stat_rx_io_discarded,
            g_telemetry.turnaround.stat_evicted_no_match, g_telemetry.turnaround.stat_completion_no_pending,
            g_telemetry.turnaround.stat_rx_no_pending);
    fprintf(stderr, "  index pairing (Phase 7.4): tx_skipped=%" PRIu64 " tx_unmatched=%" PRIu64
                    " rx_skipped=%" PRIu64 " (sends whose reply never came) rx_unmatched=%" PRIu64
                    " (duplicate/late/foreign arrivals)\n",
            g_telemetry.turnaround.stat_tx_skipped, g_telemetry.turnaround.stat_tx_unmatched,
            g_telemetry.turnaround.stat_rx_skipped, g_telemetry.turnaround.stat_rx_unmatched);
    fprintf(stderr, "  exclusion windows (Phase 7.3): polls=%" PRIu64 " discarded tx_order=%" PRIu64
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
/* ---- Phase 10.5 test driver of the CiA402 layer (application thread) ---- */
typedef struct { double t; char op[12]; int axis; double a, b; } cia_step_t;
static int cia_parse(const char *src, cia_step_t *st, int max)
{
    char buf[2048], *save = NULL;
    int n = 0;
    snprintf(buf, sizeof(buf), "%s", src);
    for (char *p = strtok_r(buf, ";", &save); p && n < max; p = strtok_r(NULL, ";", &save)) {
        char ax[16] = "";
        cia_step_t s = { 0 };
        int k = sscanf(p, " %lf %11s %15s %lf %lf", &s.t, s.op, ax, &s.a, &s.b);
        if (k < 3) return -1;
        s.axis = !strcmp(ax, "all") ? -1 : (int)strtol(ax, NULL, 0);
        if (!strcmp(s.op, "mode")) {               /* mode AXIS csp|csv: 3rd token is the mode */
            char md[8] = "";
            if (sscanf(p, " %*f %*s %*s %7s", md) != 1) return -1;
            s.a = !strcmp(md, "csv") ? ECM_OPMODE_CSV : ECM_OPMODE_CSP;
        }
        st[n++] = s;
    }
    return n;
}

static void *cia402_app(void)
{
    cia_step_t steps[64];
    int ns = g_cia_script ? cia_parse(g_cia_script, steps, 64) : 0, si = 0;
    if (ns < 0) { fprintf(stderr, "[CIA402-APP] cannot parse --cia402-script\n"); ns = 0; }
    enum { P_NONE, P_SINE, P_VEL, P_POS } kind[ECM_AXIS_MAX] = { 0 };
    double amp[ECM_AXIS_MAX] = { 0 }, hz[ECM_AXIS_MAX] = { 0 }, vel[ECM_AXIS_MAX] = { 0 };
    int64_t base[ECM_AXIS_MAX] = { 0 };
    uint64_t t_start[ECM_AXIS_MAX] = { 0 }, next[ECM_AXIS_MAX] = { 0 }, last_rec[ECM_AXIS_MAX] = { 0 };
    static int64_t sp[ECM_AXIS_MAX][1024];
    static uint8_t sp_ok[ECM_AXIS_MAX][1024];
    ecm_cia402_state_t prev[ECM_AXIS_MAX];
    memset(prev, 0, sizeof(prev));
    uint64_t t0 = 0;
    uint32_t seq = 0;
    while (!g_app_stop) {
        struct timespec d = { 0, 1000000 };
        clock_nanosleep(CLOCK_MONOTONIC, 0, &d, NULL);
        uint64_t k;
        if (ecm_cia402_read_clock(&g_cia, &k, NULL) || k == 0) continue;
        if (!t0) t0 = k;
        double now = (double)(k - t0) * (double)g_cia.cycle_ns * 1e-9;
        for (; si < ns && steps[si].t <= now; si++) {
            cia_step_t *s = &steps[si];
            for (int a = 0; a < g_cia.naxes; a++) {
                if (s->axis >= 0 && s->axis != a) continue;
                ecm_cia402_state_t cs;
                ecm_cia402_read(&g_cia, a, &cs);
                int op = 0;
                if (!strcmp(s->op, "enable")) op = ECM_CIA_OP_ENABLE;
                else if (!strcmp(s->op, "disable")) op = ECM_CIA_OP_DISABLE;
                else if (!strcmp(s->op, "quickstop")) op = ECM_CIA_OP_QUICKSTOP;
                else if (!strcmp(s->op, "reset")) op = ECM_CIA_OP_FAULT_RESET;
                else if (!strcmp(s->op, "mode")) op = ECM_CIA_OP_SET_MODE;
                else if (!strcmp(s->op, "sine")) { kind[a] = P_SINE; amp[a] = s->a; hz[a] = s->b; base[a] = cs.apos; t_start[a] = k; next[a] = 0; }
                else if (!strcmp(s->op, "vel"))  { kind[a] = P_VEL; vel[a] = s->a; next[a] = 0; }
                else if (!strcmp(s->op, "pos"))  { kind[a] = P_POS; vel[a] = s->a; next[a] = 0; }   /* 10.6: absolute CSP target */
                else if (!strcmp(s->op, "stop")) kind[a] = P_NONE;
                if (op) ecm_cia402_cmd(&g_cia, a, op, (int64_t)s->a, ++seq, 0);
                fprintf(stderr, "[CIA402-APP] t=%.3f tick=%" PRIu64 " axis %d: %s\n", now, k, a, s->op);
            }
        }
        for (int a = 0; a < g_cia.naxes; a++) {
            /* setpoints, g_cia_lead ticks ahead */
            if (kind[a] != P_NONE) {
                if (next[a] < k + 2) next[a] = k + 2;
                for (; next[a] <= k + (uint64_t)g_cia_lead; next[a]++) {
                    int64_t v = kind[a] == P_VEL || kind[a] == P_POS ? (int64_t)llround(vel[a])
                              : base[a] + (int64_t)llround(amp[a] * sin(2.0 * M_PI * hz[a] *
                                    (double)(next[a] - t_start[a]) * (double)g_cia.cycle_ns * 1e-9));
                    if (ecm_cia402_setpoint(&g_cia, a, next[a], v)) { g_capp.full++; break; }
                    g_capp.pushed++;
                    sp[a][next[a] & 1023] = v;
                    sp_ok[a][next[a] & 1023] = kind[a] == P_SINE;
                }
            }
            /* state: transitions, errors, tracking of CSP: actual(k) vs setpoint(k-1) */
            ecm_cia402_state_t s;
            if (ecm_cia402_read(&g_cia, a, &s)) continue;
            if (s.ds != prev[a].ds || s.err != prev[a].err || s.mode_disp != prev[a].mode_disp) {
                fprintf(stderr, "[CIA402] t=%.3f tick=%" PRIu64 " axis %d %s -> %s (sw 0x%04X cw 0x%04X mode %d)%s%s%s\n",
                        now, s.tick, a, ecm_cia402_ds_str((ecm_ds_t)prev[a].ds), ecm_cia402_ds_str((ecm_ds_t)s.ds),
                        s.sw, s.cw, s.mode_disp, s.err ? " ERROR " : "", s.err ? ecm_cia402_err_str(s.err) : "",
                        s.err == ECM_AXERR_TIMEOUT ? ecm_cia402_ds_str((ecm_ds_t)s.err_ds) : "");
                g_capp.events++;
            }
            if (s.tick == last_rec[a] + 1 && kind[a] == P_SINE && s.ds == ECM_DS_OE && s.mode_disp == ECM_OPMODE_CSP &&
                sp_ok[a][(s.tick - 1) & 1023] && s.tick > t_start[a] + 8) {
                int64_t e = (int64_t)s.apos - sp[a][(s.tick - 1) & 1023];
                if (e < 0) e = -e;
                if (e > g_capp.track_max[a]) g_capp.track_max[a] = e;
                g_capp.track_n[a]++;
            }
            last_rec[a] = s.tick;
            prev[a] = s;
        }
    }
    return NULL;
}

static void *app_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();
    if (g_hook == hook_cia402) return cia402_app();
    if (!g_xchg_sine) {
        while (!g_app_stop) usleep(100000);
        return NULL;
    }
    /* Phase 10.4 test producer: a sine for every output slot, g_xchg_lead
     * ticks ahead of the published tick, checked back through the slot's
     * state record (echo). Non-RT: wakes every ms, may be late -- that is
     * exactly what the late / underrun counters are for. */
    uint64_t next = 0, last_under = 0, last_rec = 0, t0 = 0;
    int nout = 0, s0 = -1;
    for (int s = 0; s < g_xchg.nslots; s++)
        if (g_xchg.slot[s].is_out) { nout++; if (s0 < 0) s0 = s; }
    while (!g_app_stop) {
        struct timespec d = { 0, 1000000 };
        clock_nanosleep(CLOCK_MONOTONIC, 0, &d, NULL);
        uint64_t k, cw[ECM_XST_WORDS];
        if (ecm_xchg_read_clock(&g_xchg, &k, cw)) { g_xapp.torn++; continue; }
        if (k == 0) continue;
        if (!t0) t0 = k;
        if (g_xchg_starve_s >= 0) {
            uint64_t a = t0 + (uint64_t)g_xchg_starve_s * 1000000000ull / g_xchg.cycle_ns;
            uint64_t b = a + (uint64_t)g_xchg_starve_ms * 1000000ull / g_xchg.cycle_ns;
            g_xapp.starve_from = a; g_xapp.starve_to = b;
            if (k >= a && k < b) continue;                  /* Q-04: stop feeding */
        }
        if (next < k + 2 && g_xchg_lead >= 2) {             /* fell behind: skip ahead */
            if (next) g_xapp.gaps++;
            next = k + 2;
        } else if (!next) {
            next = k + (uint64_t)g_xchg_lead;               /* lead 0/1: negative control */
        }
        for (; next <= k + (uint64_t)g_xchg_lead; next++)
            for (int s = 0; s < g_xchg.nslots; s++) {
                if (!g_xchg.slot[s].is_out) continue;
                if (ecm_xchg_setpoint(&g_xchg, s, next, xchg_sine_value(s, next))) g_xapp.full++;
                else g_xapp.pushed++;
            }
        if (s0 >= 0) {                                      /* echo check of the first output slot */
            uint64_t r, w[ECM_XST_WORDS];
            if (ecm_xchg_read_slot(&g_xchg, s0, &r, w)) { g_xapp.torn++; continue; }
            /* only a NEW record (the RT thread may not have run since the
             * last read: the same held value would be checked twice) whose
             * hook did not underrun (underrun unchanged since the record
             * before it) carries sine(tick + 1) */
            if (r == last_rec) continue;
            int fresh = r == last_rec + 1;
            last_rec = r;
            if (fresh && w[ECM_XS_VALID] && w[ECM_XS_UNDERRUN] == last_under && w[ECM_XS_USED]) {
                uint64_t mask = g_xchg.slot[s0].h.bits >= 64 ? ~0ull : (1ull << g_xchg.slot[s0].h.bits) - 1;
                g_xapp.checked++;
                uint64_t want = (uint64_t)xchg_sine_value(s0, r + 1 + (uint64_t)(int64_t)g_xchg_echo_off) & mask;
                if ((w[ECM_XS_VALUE] & mask) != want) {
                    if (g_xapp.mismatch < 4)
                        g_xapp.mm[g_xapp.mismatch] = (typeof(g_xapp.mm[0])){ r, w[ECM_XS_VALUE] & mask, want,
                                                                             w[ECM_XS_USED], w[ECM_XS_UNDERRUN] };
                    g_xapp.mismatch++;
                }
            }
            last_under = w[ECM_XS_UNDERRUN];
        }
    }
    (void)nout;
    return NULL;
}

/* Phase 5: real mailbox worker. The actual FPWR/FPRD socket I/O for
 * pending SDO/mailbox traffic still happens on the RT thread (see
 * ecm_mailbox_rt_pump_group() inside service_group() below) -- this
 * thread only ever talks to SOEM's PI-mutex-protected cyclic mailbox
 * queue via ecx_SDOread()/ecx_SDOwrite(), never the socket directly.
 * g_mbx is created in main() before this thread is spawned (see the
 * Phase 4 thread-ordering note above pthread_create() in main). */
static void *mailbox_thread_fn(void *arg)
{
    (void)arg;
    set_non_rt_thread();
    ecm_mailbox_run(g_mbx, &g_mailbox_stop);
    return NULL;
}

/* ~100ms cadence per master_plan_v2.md §2.4. Nothing to poll yet (no
 * CoE/mailbox until Phase 5) -- deliberately its OWN thread, separate
 * from telemetry, so that Phase 9's future SDO/mailbox calls here
 * (which can block far longer than a 1ms/8ms cycle) can never delay
 * telemetry's ring-draining -- see the Phase 4 review note on this. */
static const char *al_code_str(uint16_t code) { return ec_ALstatuscode2string(code); }

/* Phase 9.7: a requested state was not reached at startup -> say which slaves,
 * with their AL status code and its class, instead of one line. */
static void report_state_failure(int target, const char *name)
{
    int ncfg = 0;
    for (int s = 1; s <= ctx.slavecount; s++) {
        uint8_t b[6] = { 0 };       /* 0x0130 AL status, 0x0134 AL status code: one read */
        if (ecx_FPRD(&ctx.port, ctx.slavelist[s].configadr, ECT_REG_ALSTAT, sizeof(b), b, EC_TIMEOUTRET) <= 0) {
            fprintf(stderr, "ecm_run:   slave %d (%s): no answer\n", s, ctx.slavelist[s].name);
            continue;
        }
        uint16_t st = (uint16_t)(b[0] | b[1] << 8);
        if ((st & 0x0F) == target && !(st & EC_STATE_ERROR)) continue;
        uint16_t code = (uint16_t)(b[4] | b[5] << 8);
        ecm_al_class_t cl = (st & EC_STATE_ERROR) ? ecm_al_code_class(code) : ECM_AL_OK;
        if (cl == ECM_AL_CONFIG) ncfg++;
        fprintf(stderr, "ecm_run:   slave %d (%s): AL 0x%02X%s, code 0x%04X (%s)%s\n", s, ctx.slavelist[s].name,
                st, (st & EC_STATE_ERROR) ? " +ERR" : "", code, al_code_str(code),
                cl == ECM_AL_CONFIG ? " -- CONFIGURATION error" : cl == ECM_AL_TRANSIENT ? " -- transient" : "");
    }
    if (ncfg)
        fprintf(stderr, "ecm_run: %d slave(s) refuse their configuration for %s: retrying would send the same "
                "configuration again; fix the ENI/ESI or the slave\n", ncfg, name);
}

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
 * Phase 7.3: recovery, monitor side (docs/fault_policy.md §4, §5).
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
    /* Phase 8.4: with --eni, SYNC0 slaves are the ENI's DC slaves */
    int had_sync0 = g_eni_on ? g_eni.slave[s - 1].dc : (sl->group == GROUP_MOTION);
    if (!g_dc_enabled || !had_sync0 || !sl->hasdc) return;
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
    dc_arm_slave(s);
    fprintf(stderr, "ecm_run: [RECOVERY] slave %d: DC restored (delay %d ns, offset step %+lld ns), SYNC0 re-armed\n",
            s, sl->pdelay, (long long)delta);
}

/* PRE-OP -> SAFE-OP hook for ecx_reconfig_slave(): everything ecm_run
 * wrote by hand at startup that a power cycle erases. Registered only for
 * the duration of one reconfiguration. */
static int reconfig_po2so_hook(ecx_contextt *c, uint16 slave)
{
    int f = write_sm_watchdog(slave);
    /* Phase 8.4: a power-cycled slave lost the ENI's PS CoE writes too */
    int eni_ok = g_eni_on ? ecm_eni_soem_po2so(c, slave) : 1;
    dc_restore_slave(slave);
    return f == 0 && eni_ok;
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
        /* Phase 7.4: a power-cycled slave starts its mailbox Cnt at 1
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
    pd_exchange_all(EC_TIMEOUTRET);
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
                                .al_status = ds->al_status, .al_code = ds->al_code };
        int rec = 0, gu = 0;
        ecm_sact_t a = ecm_srec_decide(&g_srec, i, &in, now_ns, &rec, &gu);
        int s = i + 1;
        if (rec)
            fprintf(stderr, "ecm_run: [RECOVERY] slave %d back in OP (recovery #%llu)\n",
                    s, (unsigned long long)g_srec.s[i].recoveries);
        if (gu && g_srec.s[i].failed_config)      /* Phase 9.7 */
            fprintf(stderr, "ecm_run: [RECOVERY] slave %d: AL 0x%02x code 0x%04x (%s) is a CONFIGURATION "
                    "error -> FAILED(config) at once, no retry (the same configuration would be refused "
                    "again; fix the ENI/ESI or the slave), needs an operator\n",
                    s, ds->al_status, ds->al_code, al_code_str(ds->al_code));
        else if (gu)
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
    g_diag.emcy_lost = atomic_load_explicit(&g_emcy_ev_drops_pub, memory_order_relaxed);   /* Phase 9.7 */
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
        } else if (e.type == ECM_EV_EMCY) {   /* Phase 9.7 */
            ecm_emcy_t m = { .t_ns = (uint64_t)(mono_now_s() * 1e9), .tick = e.tick,
                             .code = (uint16_t)((uint32_t)e.a & 0xFFFF), .reg = (uint8_t)((uint32_t)e.a >> 16),
                             .data = { (uint8_t)((uint32_t)e.a >> 24), (uint8_t)e.b, (uint8_t)((uint32_t)e.b >> 8),
                                       (uint8_t)((uint32_t)e.b >> 16), (uint8_t)((uint32_t)e.b >> 24) } };
            ecm_diag_add_emcy(&g_diag, e.from, &m);
            uint64_t k = (e.from >= 1 && e.from <= g_diag.n) ? g_diag.s[e.from - 1].emcy_count : 0;
            if (k <= (uint64_t)g_emcy_print_max)
                fprintf(stderr, "ecm_run: [EMCY] tick=%llu slave %u: code 0x%04X (%s) reg 0x%02X "
                        "data %02X %02X %02X %02X %02X%s\n", (unsigned long long)e.tick, e.from, m.code,
                        ecm_emcy_class_name(m.code), m.reg, m.data[0], m.data[1], m.data[2], m.data[3], m.data[4],
                        k == (uint64_t)g_emcy_print_max ? " -- further EMCY of this slave only counted "
                        "(diag snapshot)" : "");
        } else if (e.type == ECM_EV_SDO_ABORT) {
            fprintf(stderr, "ecm_run: [SDO] tick=%llu slave %u 0x%04X:%02X abort 0x%08X (%s)\n",
                    (unsigned long long)e.tick, e.from, (unsigned)((uint32_t)e.b >> 8), (unsigned)(e.b & 0xFF),
                    (unsigned)e.a, ec_sdoerror2string((uint32)e.a));
        } else if (e.type == ECM_EV_SOEM_ERR) {
            fprintf(stderr, "ecm_run: [SOEM] tick=%llu slave %u: error type %d code 0x%04X\n",
                    (unsigned long long)e.tick, e.from, e.a, (unsigned)e.b);
        }
    }
}

/* Phase 7.2/7.3. 10 ms cadence: bus events and RECOVER requests need a
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
    int motion_slaves = -1;          /* --motion-slaves k (alias, Phase 3..Phase 8) */
    const char *io_list = NULL;      /* --io-slaves LIST (Phase 9.1) */
    long motion_cycle_us = 1000;
    long io_cycle_us     = 8000;
    long duration_sec    = 0; /* 0 = run until Ctrl+C */
    int  no_dc           = 0; /* Phase 6 */
    long dc_setpoint_pct = 30;
    long n_lost          = 100; /* Phase 7.3 */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--motion-slaves") == 0 && i + 1 < argc) {
            motion_slaves = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--io-slaves") == 0 && i + 1 < argc) {   /* Phase 9.1 */
            io_list = argv[++i];
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
        } else if (strcmp(argv[i], "--no-recover") == 0) {           /* Phase 7.3 */
            g_recover_enabled = 0;
        } else if (strcmp(argv[i], "--rx-timeout-legacy") == 0) {
            g_rx_legacy = 1;
        } else if (strcmp(argv[i], "--n-lost") == 0 && i + 1 < argc) {
            n_lost = atol(argv[++i]);
        } else if (strcmp(argv[i], "--no-quarantine") == 0) {        /* Phase 7.4 */
            g_quarantine = 0;
        } else if (strcmp(argv[i], "--no-reply-check") == 0) {
            g_reply_check = 0;
        } else if (strcmp(argv[i], "--fresh-offset") == 0 && i + 1 < argc) {
            g_fresh_off = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pdo-scan") == 0) {                 /* Phase 9.10 */
            g_pdo_scan = 1;
        } else if (strcmp(argv[i], "--pdo-dump") == 0) {
            g_pdo_dump = 1;
        } else if (strcmp(argv[i], "--cia402-script") == 0 && i + 1 < argc) {   /* Phase 10.5 */
            g_cia_script = argv[++i];
        } else if (strcmp(argv[i], "--axis-step-timeout-ms") == 0 && i + 1 < argc) {
            g_cia_step_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--axis-max-step") == 0 && i + 1 < argc) {   /* Phase 10.6 S4 */
            char *e;
            g_cia_max_pos = strtoll(argv[++i], &e, 0);
            g_cia_max_vel = *e == ':' ? strtoll(e + 1, &e, 0) : 0;
            if (*e || g_cia_max_pos < 0 || g_cia_max_vel < 0) { fprintf(stderr, "ecm_run: --axis-max-step POS[:VEL] (0 = off)\n"); return 1; }
        } else if (strcmp(argv[i], "--hook") == 0 && i + 1 < argc) {      /* Phase 10.4 */
            g_hook_name = argv[++i];
        } else if (strcmp(argv[i], "--xchg-out") == 0 && i + 1 < argc) {
            g_xchg_out_spec = argv[++i];
        } else if (strcmp(argv[i], "--xchg-in") == 0 && i + 1 < argc) {
            g_xchg_in_spec = argv[++i];
        } else if (strcmp(argv[i], "--xchg-sine") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf:%lf", &g_xchg_amp, &g_xchg_hz) != 2) {
                fprintf(stderr, "ecm_run: --xchg-sine AMPLITUDE:HZ\n"); return 1;
            }
            g_xchg_sine = 1;
        } else if (strcmp(argv[i], "--xchg-lead") == 0 && i + 1 < argc) {
            g_xchg_lead = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--xchg-echo-off") == 0 && i + 1 < argc) {
            g_xchg_echo_off = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--xchg-starve") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld:%ld", &g_xchg_starve_s, &g_xchg_starve_ms) != 2) {
                fprintf(stderr, "ecm_run: --xchg-starve SECOND:MS\n"); return 1;
            }
        } else if (strcmp(argv[i], "--axis") == 0 && i + 1 < argc) {      /* Phase 10.3 */
            g_axis_list = argv[++i];
        } else if (strcmp(argv[i], "--axis-modes") == 0 && i + 1 < argc) {
            g_axis_modes_s = argv[++i];
        } else if (strcmp(argv[i], "--axes-cfg") == 0 && i + 1 < argc) {
            g_axes_cfg = argv[++i];
        } else if (strcmp(argv[i], "--axis-no-6502") == 0) {
            g_axis_no6502 = 1;
        } else if (strcmp(argv[i], "--pdo-trust-eni") == 0) {            /* Phase 10.1 */
            g_pdo_trust_eni = 1;
        } else if (strcmp(argv[i], "--pdo-own-vendor") == 0 && i + 1 < argc) {
            char vb[256], *save = NULL;
            snprintf(vb, sizeof(vb), "%s", argv[++i]);
            g_pdo_nown = 0;
            for (char *t = strtok_r(vb, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                char *end;
                unsigned long v = strtoul(t, &end, 0);
                if (end == t || *end || g_pdo_nown >= PDO_MAX_OWN) {
                    fprintf(stderr, "ecm_run: --pdo-own-vendor %s: expected 0xVENDOR[,0xVENDOR...] (max %d)\n", argv[i], PDO_MAX_OWN);
                    return 1;
                }
                g_pdo_own[g_pdo_nown++] = (uint32_t)v;
            }
        } else if (strcmp(argv[i], "--pdo-set") == 0 && i + 1 < argc) {
            g_pdo_set_spec = argv[++i];
        } else if (strcmp(argv[i], "--pdo-get") == 0 && i + 1 < argc) {
            g_pdo_get_spec = argv[++i];
        } else if (strcmp(argv[i], "--fresh") == 0 && i + 1 < argc) {   /* Phase 9.8 */
            g_fresh_spec = argv[++i];
        } else if (strcmp(argv[i], "--fresh-stale") == 0 && i + 1 < argc) {
            g_fresh_stale = (uint32_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--link") == 0 && i + 1 < argc) {  /* Phase 8.5 */
            const char *l = argv[++i];
            if (strcmp(l, "etf") == 0) g_link_etf = 1;
            else if (strcmp(l, "af_packet") == 0) g_link_etf = 0;
            else { fprintf(stderr, "--link: af_packet or etf, not '%s'\n", l); return 1; }
        } else if (strcmp(argv[i], "--etf-lead-us") == 0 && i + 1 < argc) {
            g_etf_lead_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--etf-asap-us") == 0 && i + 1 < argc) {
            g_etf_asap_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--etf-prio") == 0 && i + 1 < argc) {
            g_etf_prio = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--etf-ns-per-byte") == 0 && i + 1 < argc) {
            g_etf_ns_per_byte = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--eni") == 0 && i + 1 < argc) {   /* Phase 8.4 */
            g_eni_path = argv[++i];
        } else if (strcmp(argv[i], "--eni-allow-unknown-regcmd") == 0) {  /* Phase 9.6, never in CI */
            g_eni_allow_unknown_reg = 1;
        } else if (strcmp(argv[i], "--sdo-timeout-ms") == 0 && i + 1 < argc) { /* Phase 9.6 */
            g_sdo_timeout_ms = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (g_eni_path) {
        char err[512];
        if (ecm_eni_load(g_eni_path, &g_eni, err, sizeof(err)) != 0) {
            fprintf(stderr, "ecm_run: --eni: %s\n", err);
            return 1;
        }
        if (ecm_eni_soem_supported(&g_eni, g_eni_allow_unknown_reg) != 0) {
            fprintf(stderr, "ecm_run: --eni: ENI uses features ecm_run cannot execute, refusing\n");
            return 1;
        }
        if (g_sdo_timeout_ms > 0)
            ecm_eni_soem_set_sdo_timeout_us(g_sdo_timeout_ms * 1000);
        if (n <= 0) {
            n = g_eni.nslaves;
        } else if (n != g_eni.nslaves) {
            fprintf(stderr, "ecm_run: --n %d but ENI %s describes %d slaves\n", n, g_eni.source, g_eni.nslaves);
            return 1;
        }
        g_eni_on = 1;
        fprintf(stderr, "ecm_run: ENI %s (from %s): %d slaves, %d CoE InitCmd(s), cycle %u us, DC ref %d\n",
                g_eni_path, g_eni.source, g_eni.nslaves, g_eni.ncoe, g_eni.cycle_us, ecm_eni_refclock(&g_eni));
        fprintf(stderr, "ecm_run: ENI state timeouts PREOP %d ms, SAFEOP %d ms, OP %d ms; "
                "%d register InitCmd(s) checked\n",
                state_timeout_us(ECM_ENI_ST_PREOP) / 1000, state_timeout_us(ECM_ENI_ST_SAFEOP) / 1000,
                state_timeout_us(ECM_ENI_ST_OP) / 1000, g_eni.nreg);
    }
    /* Phase 9.1: group of every position. --motion-slaves k keeps its
     * Phase 3..Phase 8 meaning (1..k motion, k+1..n IO, 0 < k < n) so every existing
     * script and golden capture runs unchanged. */
    int groups_ok = ifname && n > 0 && n <= EC_MAXSLAVE;
    if (groups_ok && motion_slaves >= 0 && io_list) {
        fprintf(stderr, "ecm_run: --motion-slaves and --io-slaves exclude each other\n");
        groups_ok = 0;
    } else if (groups_ok && motion_slaves >= 0) {
        if (motion_slaves <= 0 || motion_slaves >= n) {
            fprintf(stderr, "ecm_run: --motion-slaves %d needs 0 < k < n (%d); for a bus without "
                    "IO slaves leave both options out (every slave is then a motion slave)\n", motion_slaves, n);
            groups_ok = 0;
        } else {
            memset(g_is_io, 0, sizeof(g_is_io));
            for (int s = motion_slaves + 1; s <= n; s++) g_is_io[s] = 1;
        }
    } else if (groups_ok && io_list) {
        char err[160];
        if (parse_slave_list(io_list, g_is_io, n, err, sizeof(err)) != 0) {
            fprintf(stderr, "ecm_run: --io-slaves %s\n", err);
            groups_ok = 0;
        }
    }
    if (groups_ok) {
        int nio = 0;
        for (int s = 1; s <= n; s++) nio += g_is_io[s];
        if (nio == n) {
            fprintf(stderr, "ecm_run: every slave would be in GROUP_IO; GROUP_MOTION carries the tick, "
                    "DC and diagnostics and needs at least one slave\n");
            groups_ok = 0;
        }
    }
    if (!groups_ok) {
        fprintf(stderr,
            "Usage: %s --iface <veth_m> --n <total_slaves> [--io-slaves LIST | --motion-slaves <count>] [--eni FILE.enicfg] "
            "[--motion-cycle-us N] [--io-cycle-us N] [--duration-sec N] "
            "[--no-dc] [--dc-setpoint-pct N] [--no-diag] [--diag-file PATH] [--no-tx-ts]\n"
            "       [--no-recover] [--rx-timeout-legacy] [--n-lost N]\n"
            "       [--no-quarantine] [--no-reply-check] [--fresh-offset BYTE] [--fresh-stale CYCLES]\n"
            "       [--fresh all=BYTE[:BITS],N=off,N-M=BYTE[:BITS],...]   (Phase 9.8, per slave)\n"
            "       [--pdo-scan] [--pdo-dump] [--pdo-set S:IDX:SUB=VAL,...] [--pdo-get S:IDX:SUB,...]   (Phase 9.10)\n"
            "       [--pdo-own-vendor 0xV[,0xV...]] [--pdo-trust-eni]   (Phase 10.1: binds into other vendors need --pdo-scan)\n"
            "       [--axis S[:N],... [--axis-modes csp,csv,pp,pv,hm,cst] | --axes-cfg FILE] [--axis-no-6502]   (Phase 10.3)\n"
            "       [--hook empty|xchg] [--xchg-out S:IDX:SUB,...] [--xchg-in S:IDX:SUB,...]\n"
            "       [--xchg-sine AMP:HZ] [--xchg-lead TICKS] [--xchg-starve SECOND:MS]   (Phase 10.4, test hooks)\n"
            "       [--hook cia402 (needs --axis) [--axis-step-timeout-ms N] [--axis-max-step POS[:VEL] (Phase 10.6, default 100000:0, 0 = off)] [--cia402-script \"T CMD ...; ...\"]]   (Phase 10.5)\n"
            "       [--link af_packet|etf] [--etf-lead-us N] [--etf-asap-us N] [--etf-prio N] [--etf-ns-per-byte N]\n", argv[0]);
        return 1;
    }
    if (io_cycle_us % motion_cycle_us != 0) {
        fprintf(stderr,
            "io-cycle-us (%ld) must be a whole multiple of motion-cycle-us (%ld) "
            "for the round-robin tick counter to land exactly.\n", io_cycle_us, motion_cycle_us);
        return 1;
    }
    long ticks_per_io = io_cycle_us / motion_cycle_us;

    /* Phase 10.4: hook selection (before the bus is touched) */
    if (g_hook_name) {
        if (!strcmp(g_hook_name, "empty"))      { g_hook = hook_empty; g_hook_ctx = NULL; }
        else if (!strcmp(g_hook_name, "xchg"))  { g_hook = hook_xchg;  g_hook_ctx = &g_xchg; }
        else if (!strcmp(g_hook_name, "cia402")) { g_hook = hook_cia402; g_hook_ctx = &g_cia; }
        else { fprintf(stderr, "ecm_run: --hook empty|xchg|cia402\n"); return 1; }
        hist_init(&g_hook_hist);
    }
    if ((g_xchg_out_spec || g_xchg_in_spec || g_xchg_sine) && g_hook != hook_xchg) {
        fprintf(stderr, "ecm_run: --xchg-* need --hook xchg\n"); return 1;
    }
    if (g_hook == hook_xchg && !g_xchg_out_spec && !g_xchg_in_spec) {
        fprintf(stderr, "ecm_run: --hook xchg needs --xchg-out and/or --xchg-in slots\n"); return 1;
    }
    if (g_xchg_lead < 0 || g_xchg_lead > 400) { fprintf(stderr, "ecm_run: --xchg-lead 0..400\n"); return 1; }
    ecm_xchg_init(&g_xchg, (uint64_t)motion_cycle_us * 1000u);
    if (g_hook == hook_cia402 && !g_axis_list && !g_axes_cfg) {   /* axes are parsed a few lines below */
        fprintf(stderr, "ecm_run: --hook cia402 needs --axis or --axes-cfg\n"); return 1;
    }
    if (g_cia_script && g_hook != hook_cia402) { fprintf(stderr, "ecm_run: --cia402-script needs --hook cia402\n"); return 1; }
    ecm_cia402_init(&g_cia, (uint64_t)motion_cycle_us * 1000u, g_cia_step_ms);

    /* Phase 10.3: axes from the command line or a file, checked before the
     * bus is touched (syntax); bound and checked against the drive at PREOP */
    if (g_axis_list || g_axes_cfg) {
        char aerr[512];
        if (g_axis_list && g_axes_cfg) {
            fprintf(stderr, "ecm_run: --axis and --axes-cfg exclude each other\n");
            return 1;
        }
        if (g_axes_cfg) {
            if (ecm_axis_load_cfg(g_axes_cfg, g_axes, &g_naxes, ECM_AXIS_MAX, aerr, sizeof(aerr))) {
                fprintf(stderr, "ecm_run: --axes-cfg: %s\n", aerr);
                return 1;
            }
        } else {
            uint32_t m;
            if (ecm_axis_parse_modes(g_axis_modes_s, &m, aerr, sizeof(aerr)) ||
                ecm_axis_parse_list(g_axis_list, m, g_axes, &g_naxes, ECM_AXIS_MAX, aerr, sizeof(aerr))) {
                fprintf(stderr, "ecm_run: --axis: %s\n", aerr);
                return 1;
            }
        }
    }

    g_ifname = ifname;   /* Phase 4: telemetry thread's passive RX socket needs this */
    g_motion_cycle_us = motion_cycle_us;   /* Phase 7.3: receive budget */

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
    /* Phase 8.5: --link etf. Before the first frame: with an ETF qdisc
     * every frame without a valid txtime is dropped, config included. */
    if (g_link_etf) {
#ifdef ECMASTER_SOEM_TXTIME_PATCH
        if (g_etf_lead_us <= 0 || g_etf_lead_us >= motion_cycle_us / 2 || g_etf_asap_us <= 0
            || g_etf_asap_us >= g_etf_lead_us) {   /* 8.5 v2: asap < lead */
            fprintf(stderr, "ecm_run: --link etf: need 0 < asap < lead < cycle/2 (lead %ld, asap %ld us)\n",
                    g_etf_lead_us, g_etf_asap_us);
            ecx_close(&ctx); return 1;
        }
        int sp = link_speed_mbps(ifname);
        if (g_etf_ns_per_byte <= 0) {
            /* Never below the 1 Gbit/s value 8 used until Phase 9.2 (veth reports
             * 10 Gbit/s; the i226 at 2.5 Gbit/s never carries EtherCAT). */
            g_etf_ns_per_byte = (sp > 0 && sp < 1000) ? (8000 + sp - 1) / sp : 8;
            fprintf(stderr, "ecm_run: link etf: %s speed %d Mbit/s -> %d ns/byte%s\n", ifname, sp,
                    g_etf_ns_per_byte, sp > 0 ? "" : " (speed unknown, 1 Gbit/s value kept)");
        } else if (sp > 0 && g_etf_ns_per_byte * sp < 8000) {
            fprintf(stderr, "ecm_run: WARNING --etf-ns-per-byte %d is too small for %d Mbit/s "
                            "(needs >= %d): txtimes of back-to-back frames overlap\n",
                    g_etf_ns_per_byte, sp, (8000 + sp - 1) / sp);
        }
        if (!ecx_txtime_enable(&ctx.port, g_etf_prio, (int64_t)g_etf_asap_us * 1000,
                               (uint32_t)g_etf_ns_per_byte)) {
            perror("ecm_run: --link etf: SO_TXTIME");
            ecx_close(&ctx); return 1;
        }
        int q = etf_qdisc_state(ifname);
        fprintf(stderr, "ecm_run: link etf: lead %ld us, asap %ld us, SO_PRIORITY %d, %d ns/byte; qdisc on %s: %s\n",
                g_etf_lead_us, g_etf_asap_us, g_etf_prio, g_etf_ns_per_byte, ifname,
                q == 2 ? "etf offload" : q == 1 ? "etf WITHOUT offload (software launch)" :
                q == 0 ? "NO etf -- txtime ignored, frames leave at wake time (lead early)" : "unknown");
#else
        fprintf(stderr, "ecm_run: --link etf needs SOEM with patches/soem-txtime.patch\n");
        ecx_close(&ctx); return 1;
#endif
    }
    int wc = ecx_config_init(&ctx);
    if (wc <= 0) {
        fprintf(stderr, "No slaves found.\n");
        ecx_close(&ctx);
        return 1;
    }
    if (g_eni_on) {
        if (ecm_eni_soem_check_identity(&ctx, &g_eni) != 0) {
            fprintf(stderr, "ecm_run: bus does not match the ENI, refusing to configure\n");
            ecx_close(&ctx);
            return 1;
        }
    } else if (wc < n) {
        fprintf(stderr, "Warning: found %d slave(s), expected %d -- continuing with what was found.\n", wc, n);
        n = wc;
        /* Phase 3..Phase 8 behaviour of --motion-slaves: keep at least one IO slave */
        if (motion_slaves >= n && n > 1) {
            motion_slaves = n - 1;
            for (int s = 1; s <= n; s++) g_is_io[s] = (uint8_t)(s > motion_slaves);
        }
        int nio = 0;
        for (int s = 1; s <= n; s++) nio += g_is_io[s];
        if (nio == n) {
            fprintf(stderr, "ecm_run: the %d slave(s) found are all IO slaves, GROUP_MOTION would be empty\n", n);
            ecx_close(&ctx);
            return 1;
        }
    }

    /* ---- Group assignment: MUST happen after config_init, before map_group ---- */
    int n_motion = 0, n_io = 0;
    for (int s = 1; s <= n; s++) {
        ctx.slavelist[s].group = g_is_io[s] ? GROUP_IO : GROUP_MOTION;
        if (g_is_io[s]) n_io++; else n_motion++;
    }
    g_io_active = n_io > 0;

    if (g_eni_on) {
        /* IP InitCmds need the mailbox, i.e. PRE-OP (ecx_config_init requested it) */
        if (ecx_statecheck(&ctx, 0, EC_STATE_PRE_OP, state_timeout_us(ECM_ENI_ST_PREOP)) != EC_STATE_PRE_OP) {
            fprintf(stderr, "ecm_run: --eni: bus not in PRE-OP before InitCmds\n");
            ecx_close(&ctx); return 1;
        }
        if (ecm_eni_soem_run_transition(&ctx, &g_eni, ECM_ENI_T_IP, 0) != 0) {
            fprintf(stderr, "ecm_run: --eni: IP InitCmd failed, refusing\n");
            ecx_close(&ctx); return 1;
        }
        ecm_eni_soem_arm_po2so(&ctx, &g_eni);    /* PS InitCmds run inside map_group */
    }

    int motion_iomap_size = ecx_config_map_group(&ctx, IOmap_motion, GROUP_MOTION);
    int io_iomap_size     = ecx_config_map_group(&ctx, IOmap_io,     GROUP_IO);

    if (g_eni_on) {
        for (int s = 1; s <= ctx.slavecount; s++)
            ctx.slavelist[s].PO2SOconfig = NULL;   /* recovery installs its own */
        if (ecm_eni_soem_po2so_failures() != 0) {
            fprintf(stderr, "ecm_run: --eni: %d PS InitCmd(s) failed, refusing SAFE-OP\n",
                    ecm_eni_soem_po2so_failures());
            ecx_close(&ctx); return 1;
        }
        if (ecm_eni_soem_check_layout(&ctx, &g_eni) != 0) {
            fprintf(stderr, "ecm_run: --eni: mapped process data differs from the ENI, refusing\n");
            ecx_close(&ctx); return 1;
        }
    }

    /* Phase 9.1: ecx_config_map_group()'s return value includes the
     * logstartaddr that its mbxstatuslength is off by (see the Phase 5 note
     * further below); up to Phase 8 this line printed e.g. "65572 byte" for a
     * 36 byte motion map. Print the bytes the group really sends. */
    motion_iomap_size -= (int)ctx.grouplist[GROUP_MOTION].logstartaddr;
    io_iomap_size     -= (int)ctx.grouplist[GROUP_IO].logstartaddr;
    fprintf(stderr, "ecm_run: GROUP_MOTION (%d slave, %d byte IOmap), GROUP_IO (%d slave, %d byte IOmap)%s\n",
            n_motion, motion_iomap_size, n_io, io_iomap_size,
            g_io_active ? "" : " -- GROUP_IO empty: never sent");

    /* ---- Phase 9.10: PDO table (ENI and/or scan), checked against what SOEM
     * mapped, then the --pdo-set/--pdo-get references bound. Scanning
     * uses plain SDO reads: here the cyclic mailbox is not enabled yet. */
    if (g_eni_on && g_eni.npdo) {
        ecm_pdo_table_init(&g_pdo_eni);
        for (int k = 0; k < g_eni.npdo; k++) {
            const ecm_eni_pdo_t *q = &g_eni.pdo[k];
            ecm_pdo_add(&g_pdo_eni, q->pos, q->dir ? ECM_PDO_IN : ECM_PDO_OUT, q->pdo, q->index, q->sub, q->bits);
        }
        g_pdo = &g_pdo_eni;
        fprintf(stderr, "ecm_run: PDO table from the ENI: %d entries\n", g_pdo_eni.n);
    }
    if (g_pdo_scan) {
        ecm_pdo_table_init(&g_pdo_bus);
        if (ecm_pdo_scan(&ctx, &g_pdo_bus) != 0) {
            fprintf(stderr, "ecm_run: --pdo-scan failed, refusing\n"); ecx_close(&ctx); return 1;
        }
        { ec_errort er; while (ecx_poperror(&ctx, &er)) { } }   /* SOEM probing aborts of the scan */
        fprintf(stderr, "ecm_run: PDO table from the bus: %d entries\n", g_pdo_bus.n);
        if (g_pdo) {
            static char perr[8192];
            int d = ecm_pdo_table_compare(&g_pdo_eni, &g_pdo_bus, perr, sizeof(perr));
            if (d) {
                fprintf(stderr, "ecm_run: PDO table of the ENI differs from the bus (%d):\n%s"
                        "ecm_run: refusing\n", d, perr);
                ecx_close(&ctx); return 1;
            }
            fprintf(stderr, "ecm_run: PDO table: ENI == bus (%d entries)\n", g_pdo_bus.n);
        }
        g_pdo = &g_pdo_bus;
    }
    if (g_pdo) {
        if (ecm_pdo_soem_check(&ctx, g_pdo) != 0) {
            fprintf(stderr, "ecm_run: PDO table does not match the mapping, refusing\n");
            ecx_close(&ctx); return 1;
        }
        uint8 *const iomaps[3] = { IOmap_motion, IOmap_motion, IOmap_io };
        ecm_pdo_locate(&ctx, iomaps, 3, g_pdo_loc);
        if (g_pdo_dump) {
            static char tb[65536];
            ecm_pdo_table_format(g_pdo, tb, sizeof(tb));
            fputs(tb, stderr);
        }
    }
    if (g_pdo_set_spec || g_pdo_get_spec) {
        if (!g_pdo) {
            fprintf(stderr, "ecm_run: --pdo-set/--pdo-get need a PDO table: --eni (enicfg 2) or --pdo-scan\n");
            ecx_close(&ctx); return 1;
        }
        const char *specs[2] = { g_pdo_set_spec, g_pdo_get_spec };
        /* Phase 10.1: the table must come from the bus for every bound slave
         * this project did not build (N-03). Checked before any bind, so
         * the refusal names all such slaves at once. */
        if (g_pdo != &g_pdo_bus) {
            uint16_t bound[2 * PDO_MAX_REFS];
            int nb = 0;
            for (int w = 0; w < 2; w++) {
                if (!specs[w]) continue;
                char buf[512];
                snprintf(buf, sizeof(buf), "%s", specs[w]);
                for (char *save = NULL, *t = strtok_r(buf, ",", &save); t && nb < 2 * PDO_MAX_REFS;
                     t = strtok_r(NULL, ",", &save)) {
                    char *eq = strchr(t, '=');
                    if (eq) *eq = '\0';
                    int sl; uint16_t ix; uint8_t sb;
                    if (ecm_pdo_parse_ref(t, &sl, &ix, &sb) == 0) bound[nb++] = (uint16_t)sl;
                }
            }
            uint32_t vend[ECM_PDO_MAX_SLAVES + 1] = { 0 };
            int nv = ctx.slavecount < ECM_PDO_MAX_SLAVES ? ctx.slavecount : ECM_PDO_MAX_SLAVES;
            for (int s = 1; s <= nv; s++) vend[s] = ctx.slavelist[s].eep_man;
            char verr[512];
            int nn = ecm_pdo_scan_required(vend, nv, bound, nb, g_pdo_own, g_pdo_nown, verr, sizeof(verr));
            if (nn && !g_pdo_trust_eni) {
                fprintf(stderr, "ecm_run: binding by (index, sub) into %d slave(s) of another vendor from the ENI alone: %s\n"
                        "ecm_run: their real mapping is not checked; add --pdo-scan (ENI == bus is then enforced), "
                        "--pdo-own-vendor if you built them, or --pdo-trust-eni. Refusing\n", nn, verr);
                ecx_close(&ctx); return 1;
            }
            if (nn) fprintf(stderr, "ecm_run: WARNING --pdo-trust-eni: binding into %s without checking their mapping\n", verr);
        }
        for (int w = 0; w < 2; w++) {
            if (!specs[w]) continue;
            char buf[512], perr[512];
            snprintf(buf, sizeof(buf), "%s", specs[w]);
            for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                char *eq = w == 0 ? strchr(t, '=') : NULL;
                if (w == 0 && !eq) { fprintf(stderr, "ecm_run: --pdo-set %s: expected S:IDX:SUB=VALUE\n", t); ecx_close(&ctx); return 1; }
                if (eq) *eq = '\0';
                int sl; uint16_t ix; uint8_t sb;
                int *cnt = w == 0 ? &g_pdo_nset : &g_pdo_nget;
                ecm_pdo_handle_t *hh = w == 0 ? &g_pdo_set_h[*cnt] : &g_pdo_get_h[*cnt];
                if (*cnt >= PDO_MAX_REFS || ecm_pdo_parse_ref(t, &sl, &ix, &sb) != 0) {
                    fprintf(stderr, "ecm_run: --pdo-%s %s: expected SLAVE:INDEX:SUB (max %d)\n", w ? "get" : "set", t, PDO_MAX_REFS);
                    ecx_close(&ctx); return 1;
                }
                if (ecm_pdo_bind(g_pdo, g_pdo_loc, sl, ix, sb, hh, perr, sizeof(perr)) != 0) {
                    fprintf(stderr, "ecm_run: --pdo-%s: %s\n", w ? "get" : "set", perr);
                    ecx_close(&ctx); return 1;
                }
                if (w == 0) g_pdo_set_v[*cnt] = strtoull(eq + 1, NULL, 0);
                else snprintf(g_pdo_get_name[*cnt], sizeof(g_pdo_get_name[0]), "%s", t);
                fprintf(stderr, "ecm_run: pdo %s slave %d 0x%04X:%02X -> group %u %s bit %u, %u bit%s\n",
                        w ? "get" : "set", sl, ix, sb, hh->group, hh->dir ? "in" : "out", hh->bit, hh->bits,
                        hh->dir == (w ? ECM_PDO_IN : ECM_PDO_OUT) ? "" :
                        (w ? " (an output: reads back what the master writes)" : " -- NOT an output"));
                if (w == 0 && hh->dir != ECM_PDO_OUT) {
                    fprintf(stderr, "ecm_run: --pdo-set: slave %d 0x%04X:%02X is an input, refusing\n", sl, ix, sb);
                    ecx_close(&ctx); return 1;
                }
                (*cnt)++;
            }
        }
    }

    /* ---- Phase 10.3: CiA402 axes. Still PREOP: plain SDO reads (the cyclic
     * mailbox is not running yet), and every refusal comes before SAFE-OP. */
    if (g_naxes) {
        char aerr[1024];
        if (!g_pdo) {
            fprintf(stderr, "ecm_run: --axis needs a PDO table: --eni (enicfg 2) or --pdo-scan\n");
            ecx_close(&ctx); return 1;
        }
        if (g_pdo != &g_pdo_bus) {                     /* Phase 10.1 rule, for axes */
            uint16_t bound[ECM_AXIS_MAX];
            uint32_t vend[ECM_PDO_MAX_SLAVES + 1] = { 0 };
            int nv = ctx.slavecount < ECM_PDO_MAX_SLAVES ? ctx.slavecount : ECM_PDO_MAX_SLAVES;
            for (int s = 1; s <= nv; s++) vend[s] = ctx.slavelist[s].eep_man;
            for (int k = 0; k < g_naxes; k++) bound[k] = g_axes[k].slave;
            int nn = ecm_pdo_scan_required(vend, nv, bound, g_naxes, g_pdo_own, g_pdo_nown, aerr, sizeof(aerr));
            if (nn && !g_pdo_trust_eni) {
                fprintf(stderr, "ecm_run: axes on %d slave(s) of another vendor with the PDO table from the ENI alone: %s\n"
                        "ecm_run: their real mapping is not checked; add --pdo-scan (ENI == bus is then enforced), "
                        "--pdo-own-vendor if you built them, or --pdo-trust-eni. Refusing\n", nn, aerr);
                ecx_close(&ctx); return 1;
            }
            if (nn) fprintf(stderr, "ecm_run: WARNING --pdo-trust-eni: axes on %s without checking their mapping\n", aerr);
        }
        for (int k = 0; k < g_naxes; k++) {
            const ecm_axis_cfg_t *a = &g_axes[k];
            if (a->slave > ctx.slavecount) {
                fprintf(stderr, "ecm_run: axis %s: slave %u, the bus has %d. Refusing\n", a->name, a->slave, ctx.slavecount);
                ecx_close(&ctx); return 1;
            }
            if (ecm_axis_bind(a, g_pdo, g_pdo_loc, &g_axis_b[k], aerr, sizeof(aerr))) {
                fprintf(stderr, "ecm_run: %s. Refusing\n", aerr);
                ecx_close(&ctx); return 1;
            }
            uint16_t i6502 = ecm_axis_index(a, 0x6502);
            uint32_t sup = 0;
            int sz = (int)sizeof(sup);
            int w = ecx_SDOread(&ctx, a->slave, i6502, 0, FALSE, &sz, &sup, EC_TIMEOUTRXM);
            uint32_t abort_code = 0;
            { ec_errort er; while (ecx_poperror(&ctx, &er)) if (er.Etype == EC_ERR_TYPE_SDO_ERROR) abort_code = (uint32_t)er.AbortCode; }
            if (w <= 0) {
                if (!g_axis_no6502) {
                    fprintf(stderr, "ecm_run: axis %s (slave %u): cannot read 0x%04X:00 supported drive modes "
                            "(SDO abort 0x%08X): the modes cannot be checked. Refusing (--axis-no-6502 to go on without)\n",
                            a->name, a->slave, i6502, abort_code);
                    ecx_close(&ctx); return 1;
                }
                fprintf(stderr, "ecm_run: WARNING axis %s: 0x%04X not readable (abort 0x%08X), modes NOT checked (--axis-no-6502)\n",
                        a->name, i6502, abort_code);
            } else if (ecm_axis_check_modes(a, sup, aerr, sizeof(aerr))) {
                fprintf(stderr, "ecm_run: %s. Refusing\n", aerr);
                ecx_close(&ctx); return 1;
            }
            const ecm_axis_bind_t *b = &g_axis_b[k];
            char ms[64];
            ecm_axis_modes_str(a->modes, ms, sizeof(ms));
            fprintf(stderr, "ecm_run: axis %s: slave %u axis %u, modes %s, 0x6502 %s0x%08X; "
                    "cw group %u bit %u, sw group %u bit %u%s%s, mode by %s\n",
                    a->name, a->slave, a->n, ms, w > 0 ? "" : "unread ", (unsigned)sup,
                    b->cw.group, b->cw.bit, b->sw.group, b->sw.bit,
                    b->tpos.bits ? ", target pos" : "", b->tvel.bits ? ", target vel" : "",
                    b->mode_by_sdo ? "SDO/InitCmd (0x6060 not in the PDOs)" : "PDO");
        }
        /* Phase 10.5: one mode and no 0x6060 in the PDOs -> set it by SDO now
         * (an ENI InitCmd may already have; writing it again is harmless) and
         * read it back. With 0x6060 in the PDOs the hook writes it and waits
         * for 0x6061. */
        for (int k = 0; k < g_naxes && g_hook == hook_cia402; k++) {
            const ecm_axis_cfg_t *a = &g_axes[k];
            if (!g_axis_b[k].mode_by_sdo) continue;
            int8_t m = (a->modes & ECM_MODE_CSP) ? ECM_OPMODE_CSP : ECM_OPMODE_CSV, rb = 0;
            int sz = 1;
            uint16_t i6060 = ecm_axis_index(a, 0x6060);
            int w1 = ecx_SDOwrite(&ctx, a->slave, i6060, 0, FALSE, 1, &m, EC_TIMEOUTRXM);
            int w2 = ecx_SDOread(&ctx, a->slave, i6060, 0, FALSE, &sz, &rb, EC_TIMEOUTRXM);
            { ec_errort er; while (ecx_poperror(&ctx, &er)) { } }
            if (w1 <= 0 || w2 <= 0 || rb != m) {
                fprintf(stderr, "ecm_run: axis %s: setting 0x%04X:00 = %d by SDO failed (read back %d). Refusing\n",
                        a->name, i6060, m, rb);
                ecx_close(&ctx); return 1;
            }
            fprintf(stderr, "ecm_run: axis %s: 0x%04X:00 = %d set by SDO (not in the PDOs)\n", a->name, i6060, m);
        }
        if (g_hook == hook_cia402) {
            for (int k = 0; k < g_naxes; k++) {
                int ax = ecm_cia402_add_axis(&g_cia, &g_axes[k], &g_axis_b[k]);
                ecm_cia402_set_step_limit(&g_cia, ax, g_cia_max_pos, g_cia_max_vel);
            }
            fprintf(stderr, "ecm_run: CiA402 step limit per cycle (S4): position %" PRId64 "%s, velocity %" PRId64 "%s\n",
                    g_cia_max_pos, g_cia_max_pos ? "" : " (off)", g_cia_max_vel, g_cia_max_vel ? "" : " (off)");
#if ECM_CIA402_BROKEN
            fprintf(stderr, "ecm_run: NEGATIVE CONTROL BUILD: CiA402 latches off, ECM_CIA402_BROKEN = 0x%X\n", (unsigned)ECM_CIA402_BROKEN);
#endif
        }
        fprintf(stderr, "ecm_run: %d CiA402 axis/axes configured%s\n", g_naxes,
                g_hook == hook_cia402 ? ", CiA402 hook on (Phase 10.5)" : " (no --hook cia402: not driven)");
    }

    /* ---- Phase 10.4: exchange slots (motion group only: the hook gets its IOmap) */
    if (g_xchg_out_spec || g_xchg_in_spec) {
        if (!g_pdo) {
            fprintf(stderr, "ecm_run: --xchg-out/--xchg-in need a PDO table: --eni (enicfg 2) or --pdo-scan\n");
            ecx_close(&ctx); return 1;
        }
        const char *specs[2] = { g_xchg_out_spec, g_xchg_in_spec };
        uint16_t bound[ECM_XCHG_MAX_SLOTS];
        int nb = 0;
        for (int w = 0; w < 2; w++) {
            if (!specs[w]) continue;
            char buf[512], perr[512];
            snprintf(buf, sizeof(buf), "%s", specs[w]);
            for (char *save = NULL, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                int sl; uint16_t ix; uint8_t sb;
                ecm_pdo_handle_t h;
                if (ecm_pdo_parse_ref(t, &sl, &ix, &sb) || ecm_pdo_bind(g_pdo, g_pdo_loc, sl, ix, sb, &h, perr, sizeof(perr))) {
                    fprintf(stderr, "ecm_run: --xchg-%s %s: %s\n", w ? "in" : "out", t,
                            ecm_pdo_parse_ref(t, &sl, &ix, &sb) ? "expected SLAVE:INDEX:SUB" : perr);
                    ecx_close(&ctx); return 1;
                }
                if (h.dir != (w ? ECM_PDO_IN : ECM_PDO_OUT) || h.group != GROUP_MOTION) {
                    fprintf(stderr, "ecm_run: --xchg-%s %s: must be an %s of a motion-group slave. Refusing\n",
                            w ? "in" : "out", t, w ? "input" : "output");
                    ecx_close(&ctx); return 1;
                }
                int k = ecm_xchg_add_slot(&g_xchg, &h);
                if (k < 0) { fprintf(stderr, "ecm_run: more than %d exchange slots\n", ECM_XCHG_MAX_SLOTS); ecx_close(&ctx); return 1; }
                bound[nb++] = (uint16_t)sl;
                fprintf(stderr, "ecm_run: xchg slot %d = %s slave %d 0x%04X:%02X (%s, bit %u, %u bit)\n",
                        k, w ? "in " : "out", sl, ix, sb, w ? "state" : "setpoints", h.bit, h.bits);
            }
        }
        if (g_pdo != &g_pdo_bus) {                     /* Phase 10.1 rule */
            uint32_t vend[ECM_PDO_MAX_SLAVES + 1] = { 0 };
            int nv = ctx.slavecount < ECM_PDO_MAX_SLAVES ? ctx.slavecount : ECM_PDO_MAX_SLAVES;
            for (int s = 1; s <= nv; s++) vend[s] = ctx.slavelist[s].eep_man;
            char verr[512];
            int nn = ecm_pdo_scan_required(vend, nv, bound, nb, g_pdo_own, g_pdo_nown, verr, sizeof(verr));
            if (nn && !g_pdo_trust_eni) {
                fprintf(stderr, "ecm_run: exchange slots on %d slave(s) of another vendor from the ENI alone: %s. "
                        "Add --pdo-scan. Refusing\n", nn, verr);
                ecx_close(&ctx); return 1;
            }
        }
    }
    if (g_hook) fprintf(stderr, "ecm_run: RT hook '%s' registered (Phase 10.4)\n", g_hook_name);

    /* ---- Phase 9.8: freshness per slave. Resolved here, after mapping, so a
     * counter outside a slave's inputs is refused now (by name) instead of
     * being skipped silently every cycle. --fresh-offset N (Phase 7.4) is the
     * same as --fresh all=N; --fresh entries override it. */
    if (g_fresh_off >= 0 || g_fresh_spec) {
        int nf = ctx.slavecount < ECM_SREC_MAX_SLAVES ? ctx.slavecount : ECM_SREC_MAX_SLAVES;
        if (g_fresh_off >= 0)
            for (int s = 1; s <= nf; s++) { g_fresh_on[s] = 1; g_fresh_byte[s] = (uint16_t)g_fresh_off; g_fresh_bits[s] = 16; }
        char ferr[160];
        if (g_fresh_spec && fresh_parse(g_fresh_spec, nf, ferr, sizeof(ferr)) != 0) {
            fprintf(stderr, "ecm_run: --fresh %s: %s\n", g_fresh_spec, ferr);
            ecx_close(&ctx); return 1;
        }
        int bad = 0;
        for (int s = 1; s <= nf; s++) {
            if (!g_fresh_on[s]) continue;
            g_fresh_any = 1;
            uint32_t need = (uint32_t)g_fresh_byte[s] + g_fresh_bits[s] / 8u;
            if (need > ctx.slavelist[s].Ibytes) {
                fprintf(stderr, "ecm_run: freshness slave %d: %d-bit counter at input byte %u needs %u input "
                        "byte(s), the slave maps %u\n", s, g_fresh_bits[s], g_fresh_byte[s], need,
                        (unsigned)ctx.slavelist[s].Ibytes);
                bad++;
            }
        }
        if (bad) { fprintf(stderr, "ecm_run: --fresh: refusing\n"); ecx_close(&ctx); return 1; }
    }

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
    if (!request_all_state(EC_STATE_PRE_OP, state_timeout_us(ECM_ENI_ST_PREOP))) {
        fprintf(stderr, "Failed to reach PREOP\n"); report_state_failure(EC_STATE_PRE_OP, "PREOP"); ecx_close(&ctx); return 1;
    }

    /* ---- Phase 5: enable SOEM's cyclic mailbox handler now that
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

    /* Phase 5: workaround for a length-computation asymmetry in
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

    /* ---- Phase 5: SM watchdog -- written explicitly, never left at
     * whatever the slave's power-on default happens to be (roadmap
     * requirement: "SM watchdog configured explicitly"). Two registers per
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
     *     in Phase 4/5 so far, worst case ~232us, yet trips within
     *     3ms of an actual master hang)
     *   GROUP_IO:     240 * 100us = 24ms (3x its 8ms nominal cycle,
     *     same reasoning)
     *
     * NOTE: soft_bus accepts these writes (generic register space, no
     * special handling needed) but does not yet ACT on the watchdog
     * itself (no trip/countdown behavior simulated) -- out of scope
     * for this pass. DoD for this step is "configured explicitly,
     * verifiable with tshark", not "trip behavior simulated end to end". ---- */
    /* (register/value constants moved to file scope in Phase 7.3:
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

    /* ---- Phase 6: Distributed Clocks, DC(a). Blocking frames, so
     * they go here with the SM watchdog writes: before any thread exists
     * (Phase 5 lesson: a blocking frame from another thread while the
     * RT loop runs desyncs g_tx_order), and in PREOP, before SAFEOP. ---- */
    /* Phase 9.5: with --eni the DC configuration is the ENI's, slave by
     * slave: reference clock, which slaves get SYNC0, SYNC1, shift. What
     * SOEM / ecm_run cannot do is refused here, before SAFE-OP. */
    int first_dc = 0;                       /* first DC-capable slave on the bus */
    for (int s = 1; s <= ctx.slavecount && !first_dc; s++)
        if (ctx.slavelist[s].hasdc) first_dc = s;
    if (g_eni_on && !no_dc) {
        int ref = ecm_eni_refclock(&g_eni);
        int bad = 0;
        if (ref == 0) {
            fprintf(stderr, "ecm_run: ENI has no DC slave -> DC off\n");
            no_dc = 1;
        } else {
            for (int i = 0; i < g_eni.nslaves && i < ctx.slavecount; i++) {
                const ecm_eni_slave_t *e = &g_eni.slave[i];
                int s = i + 1;
                if (!e->dc)
                    continue;
                if (!ctx.slavelist[s].hasdc) {
                    fprintf(stderr, "ecm_run: ENI slave %d (%s) uses DC%s, but the slave on the bus is not "
                            "DC capable (0x0008 bit2 = 0)\n", s, e->name, e->refclock ? " as reference clock" : "");
                    bad++;
                }
                if (e->sync0_ns != (uint32_t)(motion_cycle_us * 1000L)) {
                    fprintf(stderr, "ecm_run: ENI slave %d DC (sync0 %u ns) not supported with motion cycle "
                            "%ld us (need sync0 = cycle)\n", s, e->sync0_ns, motion_cycle_us);
                    bad++;
                }
                if (e->assign != 0x0300 && e->assign != 0x0700) {
                    fprintf(stderr, "ecm_run: ENI slave %d DC AssignActivate 0x%04X not supported "
                            "(0x0300 SYNC0, 0x0700 SYNC0 + SYNC1)\n", s, e->assign);
                    bad++;
                } else if (e->assign == 0x0300 && e->sync1_ns != 0) {
                    fprintf(stderr, "ecm_run: ENI slave %d: CycleTime1 %u ns but SYNC1 not activated "
                            "(AssignActivate 0x0300)\n", s, e->sync1_ns);
                    bad++;
                }
            }
            /* SOEM's ecx_configdc() makes the FIRST DC-capable slave the
             * reference clock (delays are measured from it downstream, the
             * FRMW reads it). TwinCAT picks the same one by default. */
            if (!bad && ref != first_dc) {
                fprintf(stderr, "ecm_run: ENI reference clock is slave %d, but the first DC-capable slave "
                        "on the bus is slave %d; SOEM uses the first one (TwinCAT: Device > EtherCAT > "
                        "Advanced Settings > Distributed Clock > reference clock)\n", ref, first_dc);
                bad++;
            }
        }
        if (bad) {
            fprintf(stderr, "ecm_run: --eni: %d DC setting(s) cannot be applied, refusing SAFE-OP\n", bad);
            ecx_close(&ctx); return 1;
        }
    }
    if (!no_dc) {
        ecx_configdc(&ctx);
        uint16_t ref = ctx.grouplist[GROUP_MOTION].DCnext;

        /* without --eni the reference must be slave 1, as since Phase 6 */
        int want_ref = g_eni_on ? first_dc : 1;
        if (!want_ref || !ctx.slavelist[want_ref].hasdc) {
            fprintf(stderr, "ecm_run: DC off -- slave %d is not DC-capable (0x0008 bit2 = 0)\n", want_ref ? want_ref : 1);
        } else if (!ctx.grouplist[GROUP_MOTION].hasdc || ctx.grouplist[GROUP_IO].hasdc || ref != want_ref) {
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
                /* Phase 8.4: with --eni the ENI decides which slaves get
                 * SYNC0 (and their shift); otherwise GROUP_MOTION as before. */
                int want_sync0 = g_eni_on
                    ? (g_eni.slave[s - 1].dc && ctx.slavelist[s].hasdc)
                    : (ctx.slavelist[s].group == GROUP_MOTION && ctx.slavelist[s].hasdc);
                int sync1 = g_eni_on && want_sync0 && (g_eni.slave[s - 1].assign & 0x0400);
                fprintf(stderr, "ecm_run: dc slave %d hasdc=%d pdelay=%d ns%s%s\n", s,
                        ctx.slavelist[s].hasdc, ctx.slavelist[s].pdelay,
                        want_sync0 ? " SYNC0" : "", sync1 ? " SYNC1" : "");
                if (want_sync0 && g_eni_on && (g_eni.slave[s - 1].shift_ns || sync1))
                    fprintf(stderr, "ecm_run: dc slave %d from the ENI: shift %d ns%s\n", s,
                            g_eni.slave[s - 1].shift_ns, sync1 ? ", SYNC1 too" : "");
                if (want_sync0) {
                    g_dc_cyc_ns = cyc;          /* dc_arm_slave() reads it */
                    dc_arm_slave(s);
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

    /* Phase 9.5: the ENI asked for DC (and --no-dc was not given): a bus
     * that ends up without it would run, but not as configured -> refuse */
    if (g_eni_on && !no_dc && !g_dc_enabled) {
        fprintf(stderr, "ecm_run: --eni: the ENI configures DC but DC could not be enabled (see above), "
                "refusing SAFE-OP\n");
        ecx_close(&ctx); return 1;
    }
    if (!request_all_state(EC_STATE_SAFE_OP, state_timeout_us(ECM_ENI_ST_SAFEOP))) {
        fprintf(stderr, "Failed to reach SAFEOP\n"); report_state_failure(EC_STATE_SAFE_OP, "SAFEOP"); ecx_close(&ctx); return 1;
    }

    /* ---- One real cycle per group BEFORE requesting OP -- satisfies each
     * ESC's "must have received valid outputs" precondition (see L2-04). */
    pd_exchange_all(EC_TIMEOUTRET);

    if (!request_op_keepalive(state_timeout_us(ECM_ENI_ST_OP))) {
        fprintf(stderr, "Failed to reach OPERATIONAL\n"); report_state_failure(EC_STATE_OPERATIONAL, "OPERATIONAL"); ecx_close(&ctx); return 1;
    }

    /* ---- Phase 4: bring up the two rings and the four non-RT threads
     * BEFORE this thread switches itself to SCHED_FIFO below. Order
     * matters: pthread_create() inherits the creating thread's scheduling
     * policy at the moment of creation (PTHREAD_INHERIT_SCHED default), so
     * spawning these while main() is still SCHED_OTHER means they start
     * SCHED_OTHER too -- each one also calls set_non_rt_thread() itself as
     * a second, explicit safeguard, in case this ordering ever changes. */
    ring_init(&g_ring);
    tx_order_ring_init(&g_tx_order);
    ecm_diag_soem_io_init(&g_diag_io);          /* Phase 7.2 */
    ecm_diag_handoff_init(&g_diag_ho);
    ecm_diag_init(&g_diag, ctx.slavecount, EC_STATE_OPERATIONAL);
    {                                           /* Phase 7.3 */
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
        for (int s = 0; s <= ECM_SREC_MAX_SLAVES; s++)
            ecm_fresh_init(&g_fresh[s], g_fresh_stale, g_fresh_bits[s] ? g_fresh_bits[s] : 16);
        fprintf(stderr, "ecm_run: late replies: index quarantine %s (%d ticks, max %d), reply check %s; input freshness %s\n",
                g_quarantine ? "ON" : "OFF (--no-quarantine)", QUAR_TICKS, QUAR_MAX,
                g_reply_check ? "ON" : "OFF (--no-reply-check)",
                g_fresh_any ? "ON" : "off (no --fresh / --fresh-offset)");
        if (g_fresh_off >= 0 && !g_fresh_spec)
            fprintf(stderr, "ecm_run: freshness: 16-bit counter at input byte %d of every slave, "
                    "stale after %u unchanged cycles\n", g_fresh_off, g_fresh_stale);
        else if (g_fresh_any)
            for (int s = 1; s <= ctx.slavecount && s <= ECM_SREC_MAX_SLAVES; s++) {
                if (g_fresh_on[s])
                    fprintf(stderr, "ecm_run: freshness slave %d: %d-bit counter at input byte %u, stale after "
                            "%u unchanged cycles\n", s, g_fresh_bits[s], g_fresh_byte[s], g_fresh_stale);
                else
                    fprintf(stderr, "ecm_run: freshness slave %d: OFF -- \"WKC correct but data old\" is NOT "
                            "detected for this slave\n", s);
            }
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

    /* Phase 7.2 finding (2026-09-25): explicit, small stacks. With
     * mlockall(MCL_CURRENT | MCL_FUTURE) above, every pthread_create()
     * with the default 8 MB stack locks and zero-fills all 8 MB up front:
     * 4 threads took 8-12 ms (measured), right between reaching OP and the
     * RT loop's first tick -- no process data during that time, so the
     * 3 ms GROUP_MOTION SM watchdog expired and every motion slave latched
     * SAFEOP+ERR 0x001B at startup, every run. Invisible before soft_bus
     * modelled the watchdog (Phase 7.1). Deepest thread frame is
     * ~2.5 KB (gcc -fstack-usage) plus libc stdio: 256 KB is >10x margin,
     * and 4 x 256 KB locks in well under 1 ms. */
    pthread_t telemetry_tid, app_tid, mailbox_tid, monitor_tid;
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, NON_RT_STACK_BYTES);
        /* Phase 9.7: from here the RT thread is the only reader of SOEM's error
         * list. What configuration left in it was reported already (InitCmd
         * failures) or is SOEM's own probing (e.g. 0x1C00 read aborts). */
        {
            ec_errort er;
            int left = 0, emcy = 0;
            while (ecx_poperror(&ctx, &er)) { left++; if (er.Etype == EC_ERR_TYPE_EMERGENCY) emcy++; }
            if (left)
                fprintf(stderr, "ecm_run: %d SOEM error list entr%s from configuration discarded (%d EMCY)\n",
                        left, left == 1 ? "y" : "ies", emcy);
            if (g_eni_on) ecm_eni_soem_elist_foreign(1);
        }
        pthread_create(&telemetry_tid, &attr, telemetry_thread_fn, NULL);
        pthread_create(&app_tid,       &attr, app_thread_fn,       NULL);
        pthread_create(&mailbox_tid,   &attr, mailbox_thread_fn,   NULL);
        pthread_create(&monitor_tid,   &attr, monitor_thread_fn,   NULL);
        pthread_attr_destroy(&attr);
    }

    /* ---- NOW switch this thread (the RT thread) to SCHED_FIFO 80,
     * pinned to the isolated core (core 3, isolcpus=3 from Phase 1).
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

    if (g_io_active)
        fprintf(stderr, "ecm_run: all slaves in OPERATIONAL. Starting cyclic loop (base tick = %ld us, IO every %ld ticks).\n",
                motion_cycle_us, ticks_per_io);
    else
        fprintf(stderr, "ecm_run: all slaves in OPERATIONAL. Starting cyclic loop (base tick = %ld us, no IO group).\n",
                motion_cycle_us);
    fprintf(stderr, "ecm_run: no further stderr output from the RT thread until the loop ends -- see the Phase 3 file header for why.\n");

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);                 /* Phase 10.6 S6: same shutdown path */
    g_cstop.max_ticks = ((uint64_t)g_cia_step_ms * 3 + 100) * 1000 / (uint64_t)motion_cycle_us;
    int loop_end = 0;                           /* --duration-sec reached */

    struct timespec next, start;
    clock_gettime(CLOCK_MONOTONIC, &next);
    start = next;
    uint64_t tick = 0;
    g_dcstat.settle_ticks = (uint64_t)(5000000L / motion_cycle_us);   /* Phase 6: DC stats skip first 5 s */

    /* Ticks-per-second, used only to decide when to record a snapshot --
     * still no I/O happens as a result, just an array write. */
    long ticks_per_snapshot = (motion_cycle_us > 0) ? (1000000L / motion_cycle_us) : 1;
    if (ticks_per_snapshot < 1) ticks_per_snapshot = 1;

    unsigned recover_gen = 0;             /* Phase 7.3: RECOVER generations requested */
    while (!(g_stop || loop_end) || cia402_keep_cycling(tick)) {
        /* Phase 8.5: --link etf wakes lead before the target `next`;
         * af_packet wakes at `next` (lead 0), unchanged. */
        struct timespec wake_at = next;
        const int64_t lead_ns = g_link_etf ? g_etf_lead_us * 1000L : 0;
        if (lead_ns) ns_to_ts(ts_to_ns(&next) - (uint64_t)lead_ns, &wake_at);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake_at, NULL);

        struct timespec t_wake;
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        int64_t wake_jitter_ns = ts_diff_ns(&t_wake, &wake_at);   /* quantity #1 */
        int64_t launch_tai_ns = 0;
        if (g_link_etf) {
            static int64_t tai_prev;
            int64_t off = tai_minus_mono_ns();
            if (tai_prev != 0) {
                int64_t d = off - tai_prev; if (d < 0) d = -d;
                if (d > TAI_STEP_NS) { g_tai_steps++; if (d > g_tai_step_max) g_tai_step_max = d; }
            }
            tai_prev = off;
            launch_tai_ns = (int64_t)ts_to_ns(&next) + off;
        }

        /* Phase 7.3 §2: every receive in this tick ends by the deadline,
         * computed from the tick's TARGET time (not t_wake), so a late wake
         * shortens the budget instead of pushing the next tick. */
        /* 8.5 v2: with --link etf the motion frame leaves at `next`, and the
         * next tick's motion frame only has to be SENT by next + cycle - asap
         * (the lead only covers the sleep), so the receives may run until
         * next + cycle - asap - guard. af_packet: next + cycle - guard. */
        g_tick_deadline_ns = ts_to_ns(&next) + (uint64_t)(motion_cycle_us * 1000L)
                           - (uint64_t)(g_link_etf ? g_etf_asap_us * 1000L : 0) - RX_GUARD_NS;
        ecm_bus_tick(&g_bus);
        quar_expire(tick);                          /* Phase 7.4 */
        ecm_event_t bus_ev;

        int64_t  motion_prep_send_ns = 0, motion_total_ns = 0;
        uint64_t motion_rx_ts_ns     = 0;
        int motion_wkc = 0;
        int64_t cycle_occupancy_ns = 0;
        uint8_t io_due = (g_io_active && tick % (uint64_t)ticks_per_io == 0) ? 1 : 0;   /* Phase 9.1 */
        uint8_t any_wkc_mismatch = 0;
        int64_t dc_adjust_ns = 0;
        int hook_in_valid = 0;                         /* Phase 10.4 */

        if (g_bus.state == ECM_BUS_RUN || g_bus.state == ECM_BUS_DEGRADED) {
        /* (body below kept at its Phase 6 indentation so the 7.3 diff stays readable) */
#ifdef ECMASTER_SOEM_TXTIME_PATCH
        if (g_link_etf) ecx_txtime_set_next(&ctx.port, launch_tai_ns);   /* 8.5: only the motion frame */
#endif
        for (int k = 0; k < g_pdo_nset; k++)   /* Phase 9.10: the RT thread owns the IOmap */
            ecm_pdo_set(&g_pdo_set_h[k], g_pdo_set_h[k].group == GROUP_IO ? IOmap_io : IOmap_motion,
                        g_pdo_set_v[k]);
        service_group(&motion, tick, &motion_prep_send_ns, &motion_total_ns, &motion_rx_ts_ns, &motion_wkc);
        cycle_occupancy_ns = motion_total_ns;   /* quantity #4, starts with motion's own cost */

        if (motion_wkc == EC_NOFRAME) quar_add(motion.last_idx, tick);   /* Phase 7.4 */
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
        elist_drain(tick);   /* Phase 9.7: EMCY / SDO aborts -> monitor */

        /* ---- Phase 6: DC(b). ctx.DCtime was refreshed by the motion
         * receive (only GROUP_MOTION carries the FRMW), so read it before
         * anything else can send. Pure arithmetic, no syscalls. ---- */
        if (g_dc_enabled && motion_wkc > 0) {
            uint64_t wraps_before = g_dc.wraps;
            /* Phase 7.4: host time of the SEND, not of the wake: the DC
             * sample is taken when the frame passes the reference clock, and
             * the reply-age gate compares the two. On a non-RT host the
             * thread can be preempted between wake and send; the unwrap and
             * the PI (which works in the DC domain) do not care either way. */
            dc_adjust_ns = ecm_dc_update(&g_dc, (uint64_t)ctx.DCtime,
                                         g_link_etf ? ts_to_ns(&next) : motion.last_send_ns);   /* 8.5: launch time */
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

        /* ---- Phase 7.4 (L5-07): the DC reply-age gate says this motion
         * reply was taken at another time -> it is an old reply that SOEM
         * matched to this frame by a reused index. Put the inputs back and
         * count the cycle as "no frame". ---- */
        int motion_class = (int)ecm_wkc_classify(motion_wkc, (int)motion.expected_wkc);
        hook_in_valid = 0;
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

        /* Phase 7.3 §3: bus state machine, pure arithmetic */
        if (ecm_bus_on_cycle(&g_bus, tick, motion_class, io_class, &bus_ev)) {
            ecm_evring_push(&g_ev, &bus_ev);
            if (bus_ev.to == ECM_BUS_LOST) excl_begin();   /* ends when the bus is back in RUN */
        }

        /* Phase 7.4 (L5-09): input freshness, only on replies that were
         * accepted with the full WKC (a missing slave is WKC's business). */
        hook_in_valid = motion_class == ECM_POL_WKC_OK;   /* Phase 10.4 */
        if (g_fresh_any) {
            if (motion_class == ECM_POL_WKC_OK)
                for (int s = 1; s <= ctx.slavecount; s++)
                    if (ctx.slavelist[s].group == GROUP_MOTION) fresh_feed(s, tick);
            if (io_class == ECM_POL_WKC_OK)
                for (int s = 1; s <= ctx.slavecount; s++)
                    if (ctx.slavelist[s].group == GROUP_IO) fresh_feed(s, tick);
        }

        /* ---- Phase 7.2: diagnostics. One frame per second, never on an
         * IO tick; the reply is collected on a later tick from SOEM's rx
         * buffer (no waiting). One sendto() -> one g_tx_order push. ---- */
        if (g_diag_enabled) {
            static uint64_t diag_next_tick = 0;
            if (g_diag_io.pending) {
                int r = ecm_diag_soem_collect(&ctx, &g_diag_io, &g_diag_raw_rt,
                                              ts_to_ns(&t_wake), DIAG_MAX_AGE_TICKS);
                if (r < 0) quar_add(g_diag_io.idx, tick);    /* Phase 7.4: gave up on it */
                if (r != 0) {
                    g_diag_raw_rt.ngroups = g_io_active ? 2 : 1;   /* Phase 9.1 */
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

        /* Phase 7.3 §4 path A: at most one AL control write per tick,
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
        /* ---- Phase 10.4: the cyclic hook. After the motion receive and
         * DC(b), before the next send: what it writes goes out in the next
         * tick's frame. Runs in LOST/RECOVER too (bus_lost = 1) so a layer
         * above can latch its state; its time counts as occupancy. ---- */
        if (g_hook) {
            struct timespec h0, h1;
            clock_gettime(CLOCK_MONOTONIC, &h0);
            ecm_hook_args_t ha = {
                .iomap = IOmap_motion, .tick = tick,
                .t_send_ns = g_link_etf ? ts_to_ns(&next) : motion.last_send_ns,
                .in_valid = hook_in_valid,
                .bus_lost = !(g_bus.state == ECM_BUS_RUN || g_bus.state == ECM_BUS_DEGRADED),
            };
            g_hook(&ha, g_hook_ctx);
            clock_gettime(CLOCK_MONOTONIC, &h1);
            int64_t hns = ts_diff_ns(&h1, &h0);
            hist_add(&g_hook_hist, hns);
            g_hook_calls++;
            cycle_occupancy_ns += hns;
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

        /* Phase 6: |dc_adjust_ns| <= cycle/20, so the step stays
         * positive and ts_add_ns()'s carry-only normalisation is enough. */
        ts_add_ns(&next, motion_cycle_us * 1000L + dc_adjust_ns);

        if (duration_sec > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (ts_diff_ns(&now, &start) >= duration_sec * 1000000000L) loop_end = 1;
        }
    }

    /* Phase 7.4 (found in the 30 min soak, 25/9): leave OP the moment
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
    if (g_io_active) print_stats(&io);
    else fprintf(stderr, "  [GROUP_IO] empty (no slave assigned), never sent\n");

    /* ---- Phase 6: DC(b) summary (RT loop has exited) ---- */
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

    /* ---- Phase 8.5: link backend ---- */
    if (g_link_etf) {
#ifdef ECMASTER_SOEM_TXTIME_PATCH
        fprintf(stderr, "  [LINK] etf: lead=%ld us asap=%ld us; SOEM: late(sent now)=%u bypass(no ETF)=%u send_err=%u; "
                "ETF drops (error queue): missed=%lu invalid=%lu other=%lu; TAI steps=%" PRIu64 " (max %.1f us)\n",
                g_etf_lead_us, g_etf_asap_us, ctx.port.txtime_late, ctx.port.txtime_bypass, ctx.port.txtime_send_err,
                atomic_load(&g_etf_missed), atomic_load(&g_etf_invalid), atomic_load(&g_etf_other),
                g_tai_steps, g_tai_step_max / 1000.0);
#endif
    } else {
        fprintf(stderr, "  [LINK] af_packet\n");
    }

    /* ---- Phase 4: stop and join the four non-RT threads before
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

    /* ---- Phase 10.4: hook and exchange report (every thread joined) ---- */
    if (g_hook) {
        fprintf(stderr, "  [HOOK] '%s' calls=%" PRIu64 " p50=%" PRIu64 " p99=%" PRIu64 " p99.99=%" PRIu64
                " max=%" PRId64 " ns\n", g_hook_name, g_hook_calls,
                hist_percentile(&g_hook_hist, 0.50), hist_percentile(&g_hook_hist, 0.99),
                hist_percentile(&g_hook_hist, 0.9999), g_hook_hist.max_ns);
    }
    if (g_hook == hook_cia402) {
        if (g_cstop.state == 2)
            fprintf(stderr, "  [CIA402] shutdown (S6): every axis walked down in %" PRIu64 " ticks before leaving OP\n", g_cstop.ticks);
        else if (g_cstop.state == 3) {
            fprintf(stderr, "  [CIA402] shutdown (S6): TIMEOUT after %" PRIu64 " ticks, leaving OP anyway:", g_cstop.ticks);
            for (int k = 0; k < g_cia.naxes; k++)
                fprintf(stderr, " axis %d %s;", k, ecm_cia402_ds_str(g_cia.ax[k].ds));
            fprintf(stderr, "\n");
        } else
            fprintf(stderr, "  [CIA402] shutdown (S6): not run (state %d)\n", g_cstop.state);
        for (int k = 0; k < g_cia.naxes; k++) {
            ecm_cia402_state_t s;
            ecm_cia402_read(&g_cia, k, &s);
            fprintf(stderr, "  [CIA402] axis %d (%s): %s sw=0x%04X cw=0x%04X mode=%d pos=%d vel=%d err=%s 0x603F=0x%04X "
                    "used=%" PRIu64 " late=%" PRIu64 " underrun=%" PRIu64 " dropped=%" PRIu64 " track_max=%" PRId64 " (n=%" PRIu64 ")\n",
                    k, g_cia.ax[k].cfg.name, ecm_cia402_ds_str((ecm_ds_t)s.ds), s.sw, s.cw, s.mode_disp, s.apos, s.avel,
                    ecm_cia402_err_str(s.err), s.ecode, s.used, s.late, s.underrun, s.dropped,
                    g_capp.track_max[k], g_capp.track_n[k]);
            fprintf(stderr, "  [CIA402] S4 axis %d step_refused=%u last_step=%" PRId64 "\n", k, s.step_refused, g_cia.ax[k].step_seen);
        }
        fprintf(stderr, "  [CIA402] commands applied=%" PRIu64 " deferred=%" PRIu64 " bad=%" PRIu64
                "; app pushed=%" PRIu64 " full=%" PRIu64 " events=%" PRIu64 "\n",
                g_cia.cmd_applied, g_cia.cmd_deferred, g_cia.cmd_bad, g_capp.pushed, g_capp.full, g_capp.events);
    }
    if (g_hook == hook_xchg) {
        for (int k = 0; k < g_xchg.nslots; k++) {
            const ecm_xslot_t *sl = &g_xchg.slot[k];
            fprintf(stderr, "  [XCHG] slot %d %s slave %u bit %u: used=%" PRIu64 " late=%" PRIu64 " underrun=%" PRIu64
                    " dropped_lost=%" PRIu64 " ring_full=%" PRIu64 "\n", k, sl->is_out ? "out" : "in ",
                    sl->h.slave, sl->h.bit, sl->used, sl->late, sl->underrun, sl->dropped_lost, g_xchg.sp[k].full_drops);
        }
        fprintf(stderr, "  [XCHG] commands applied=%" PRIu64 " deferred=%" PRIu64 " unknown=%" PRIu64 "\n",
                g_xchg.cmd_applied, g_xchg.cmd_deferred, g_xchg.cmd_unknown);
        if (g_xchg_sine)
            fprintf(stderr, "  [XCHG-APP] lead=%d pushed=%" PRIu64 " full=%" PRIu64 " gaps=%" PRIu64
                    " echo_checked=%" PRIu64 " echo_mismatch=%" PRIu64 " torn=%" PRIu64 " starve=%" PRIu64 "..%" PRIu64 "\n",
                    g_xchg_lead, g_xapp.pushed, g_xapp.full, g_xapp.gaps, g_xapp.checked, g_xapp.mismatch,
                    g_xapp.torn, g_xapp.starve_from, g_xapp.starve_to);
        for (uint64_t k = 0; k < g_xapp.mismatch && k < 4; k++)
            fprintf(stderr, "  [XCHG-APP] mismatch %" PRIu64 ": record tick %" PRIu64 " value 0x%" PRIx64 " want 0x%" PRIx64
                    " (used %" PRIu64 ", underrun %" PRIu64 ")\n", k, g_xapp.mm[k].tick, g_xapp.mm[k].got,
                    g_xapp.mm[k].want, g_xapp.mm[k].used, g_xapp.mm[k].under);
    }

    /* Phase 5: only after the mailbox thread has actually returned
     * from ecm_mailbox_run() (guaranteed by the join right above) --
     * destroying g_mbx any earlier could free the job queue out from
     * under a job still in flight. */
    ecm_mailbox_destroy(g_mbx);

    /* ---- Phase 7.2: final diagnostics (monitor has joined) ---- */
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
        for (int g = 0; g < (g_io_active ? 2 : 1); g++) {   /* Phase 9.1 */
            const group_stats_t *gs = g ? &io : &motion;
            fprintf(stderr, "  [WKC %s] ok=%" PRIu64 " noframe=%" PRIu64 " zero=%" PRIu64
                    " partial=%" PRIu64 " over=%" PRIu64 " max_run_bad=%u\n", gs->label,
                    gs->wkcs.count[ECM_WKC_OK], gs->wkcs.count[ECM_WKC_NOFRAME],
                    gs->wkcs.count[ECM_WKC_ZERO], gs->wkcs.count[ECM_WKC_PARTIAL],
                    gs->wkcs.count[ECM_WKC_OVER], gs->wkcs.max_run_bad);
        }
    }

    /* ---- Phase 7.4: late/stale replies summary ---- */
    fprintf(stderr, "  [LATE] index quarantine %s: parked=%" PRIu64 " early_release=%" PRIu64
            "; motion replies rejected by the DC age gate=%" PRIu64 " (gate resyncs=%" PRIu64 ")\n",
            g_quarantine ? "on" : "OFF", g_quar_total, g_quar_overflow, g_stale_replies,
            g_dc_enabled ? g_dc.gate_resyncs : 0);
    fprintf(stderr, "  [LATE] replies whose datagram headers differ from the frame sent (another frame's "
            "reply through a reused index): motion=%" PRIu64 " io=%" PRIu64 "%s\n",
            g_foreign_replies[GROUP_MOTION], g_foreign_replies[GROUP_IO],
            g_reply_check ? "" : "  (--no-reply-check: counted, NOT rejected)");
    for (int k = 0; k < g_pdo_nget; k++) {   /* Phase 9.10 */
        const ecm_pdo_handle_t *h = &g_pdo_get_h[k];
        uint64_t v = ecm_pdo_get(h, h->group == GROUP_IO ? IOmap_io : IOmap_motion);
        fprintf(stderr, "  [PDO] get %s = 0x%llX (%llu)\n", g_pdo_get_name[k],
                (unsigned long long)v, (unsigned long long)v);
    }
    if (g_fresh_any) {
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

    /* ---- Phase 7.3: policy summary (monitor has joined) ---- */
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
                r->unhealthy ? " UNHEALTHY at exit" : "",
                r->failed_config ? " FAILED(config)" : r->failed ? " FAILED" : "");
    }

    request_all_state(EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&ctx);
    return 0;
}