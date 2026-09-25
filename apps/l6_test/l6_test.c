/* ==========================================================================
 * l6_test.c — Phase 6 (L6) test tool: SOEM v2.0.0 DC(a) + ecm_dc DC(b)
 * against soft_bus over veth.
 *
 * Master-side checks done here:
 *   L6-01  ecx_configdc: every slave hasdc, ref = slave 1, pdelay strictly
 *          increasing (optionally == (i-1)*hop within +-2 ns)
 *   L6-02  ecx_dcsync0: read back 0x0981 == 0x03 and 0x09A0 == cycle on every
 *          slave; first SYNC0 lies in the future at anchor time
 *   L6-04  phase lock survives the 32-bit wraps seen during the run
 *   L6-05  (--switch-ref-at-sec S) move the reference clock to slave K
 *          while in OP: one frame rewrites every 0x0928 relative to K,
 *          then the FRMW is re-targeted (DCnext). Slaves UPSTREAM of K
 *          never see K's time in-frame (a line is processed on the way
 *          out only), so the master feeds them a prediction instead of
 *          SOEM's stale previous-cycle DCtime (--upstream naive = SOEM
 *          default, negative control).
 *   CLK    CLOCK_MONOTONIC vs CLOCK_MONOTONIC_RAW measured over the run:
 *          with --expect-ref-ppm P, the measured drift must equal
 *          P - eff_ppm (soft_bus drifts the ref against RAW, the master
 *          runs on MONOTONIC, which NTP slews).
 *
 * Slave-side ground truth (SYNC0 events per window, SYNC0 intervals without a
 * process-data frame) is printed by soft_bus and checked by run_l6_tests.sh.
 *
 * Single process-data group (group 0) on purpose, like l4_test.c: this tool
 * isolates DC; the 2-group case is exercised by ecm_run.c.
 *
 * Usage:
 *   ./l6_test --iface veth_m [--duration-sec 60] [--cycle-us 1000]
 *             [--setpoint-pct 30] [--expect-hop-ns 800] [--report-sec 5]
 *             [--settle-sec 5] [--naive] [--rt-prio 80] [--cpu N]
 *             [--expect-ref-ppm 50]
 *             [--switch-ref-at-sec 20 --new-ref 4 [--upstream predict|naive]
 *              [--no-delay-rewrite]]
 * Exit code 0 = every master-side check passed.
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include "soem/soem.h"
#include "ecm_dc.h"

/* ---- options ---- */
static const char *o_iface = NULL;
static int     o_duration_s   = 60;
static int64_t o_cycle_ns     = 1000000;
static int     o_setpoint_pct = 30;
static int64_t o_expect_hop   = -1;
static int     o_report_s     = 5;
static int     o_settle_s     = 5;
static int     o_naive        = 0;
static int     o_rt_prio      = 80;
static int     o_cpu          = -1;
static double  o_expect_ppm   = 0.0;
static int     o_has_expect   = 0;
static int     o_switch_at_s  = 0;    /* 0 = no L6-05 */
static int     o_new_ref      = 4;
static int     o_up_naive     = 0;    /* L6-05 negative control #1 */
static int     o_no_delay_rw  = 0;    /* L6-05 negative control #2 */
static int     o_reset_filt   = 0;    /* L6-05: also write 0x0930 (datasheet II §2.15.2.3) */

static ecx_contextt ctx;
static uint8_t      IOmap[4096];
static int          expected_wkc;
static ecm_dc_t     g_dc;
static volatile int g_run = 1;       /* written by main only, read by RT */
static volatile int g_stop_req = 0;  /* SIGINT */

/* ---- stats: RT writes with relaxed atomics, main reads ---- */
#define HIST_US 1001                  /* |e| bins of 1 us, last bin = overflow */
static struct {
    uint64_t cycles, wkc_low, overruns, samples;
    int64_t  sum_e, sum_u;            /* cumulative, after settle */
    uint64_t n;
    int64_t  win_emax;                /* exchanged to 0 by main per window */
    uint64_t wraps_seen;              /* by low-32 decrease, both modes */
    int64_t  wrap_sum_e; uint64_t wrap_n; int64_t wrap_emax;
    int      naive_state_locked;
} st;
static uint64_t hist[HIST_US];        /* written by RT, read after join */

/* ---- CLK: MONOTONIC vs RAW, taken by the RT thread when settle ends ---- */
static uint64_t g_clk_raw0, g_clk_mono0;
static volatile int g_clk_have0 = 0;

/* ---- L6-05 state. Written only by the RT thread; main reads after join
 * (sw_done is also polled by main for the report line, relaxed). ---- */
#define FEED_HIST 1001                 /* |feed error| bins of 100 ns */
static struct {
    int      done;
    uint64_t t_ns;                     /* CLOCK_MONOTONIC of the switch */
    int      frame_wkc, frame_n;       /* WKC sum of the 0x0928 frame  */
    uint64_t feed_n; int64_t feed_sum, feed_absmax;
} sw;
static uint64_t feed_hist[FEED_HIST];

static uint64_t raw_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define ST_ADD(f, v)  __atomic_fetch_add(&st.f, (v), __ATOMIC_RELAXED)
#define ST_LD(f)      __atomic_load_n(&st.f, __ATOMIC_RELAXED)
#define ST_ST(f, v)   __atomic_store_n(&st.f, (v), __ATOMIC_RELAXED)

/* ---- naive controller (= SOEM samples/ec_sample ec_sync()), same gains ---- */
static int64_t naive_syncoffset, naive_integ, naive_err;
static int64_t naive_update(int64_t reftime)
{
    int64_t delta = (reftime - naive_syncoffset) % o_cycle_ns;
    if (delta < 0) delta += o_cycle_ns;
    if (delta > o_cycle_ns / 2) delta -= o_cycle_ns;
    naive_err = delta;
    naive_integ += delta;
    int64_t u = -(delta / 8 + naive_integ / 256), m = o_cycle_ns / 20;
    if (u >  m) { u =  m; naive_integ -= delta; }
    if (u < -m) { u = -m; naive_integ -= delta; }
    return u;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void ns_to_ts(uint64_t ns, struct timespec *ts)
{ ts->tv_sec = (time_t)(ns / 1000000000ull); ts->tv_nsec = (long)(ns % 1000000000ull); }
static uint64_t le64(const uint8_t *p)
{ uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static void on_sigint(int s) { (void)s; g_stop_req = 1; }

/* L6-05: rewrite 0x0928 (system time delay) of EVERY slave in ONE frame,
 * relative to the new reference: delay_i = pdelay_i - pdelay_K (negative
 * for slaves upstream of K). One frame = atomic from the slaves' point of
 * view, so no cyclic frame ever sees a half-rewritten chain. Returns the
 * summed WKC of all datagrams (FPWR: +1 per slave). RT thread only. */
static int write_delays_one_frame(int n, int newref)
{
    static int32 val[EC_MAXSLAVE + 1];
    static uint16 off[2 * EC_MAXSLAVE + 2], len[2 * EC_MAXSLAVE + 2];
    static uint16 scs = 0;
    int nd = 0;
    ecx_portt *port = &ctx.port;
    uint8 idx = ecx_getindex(port);

    scs = htoes(0x1000);                 /* speed counter start: SOEM's value */
    int total = o_reset_filt ? 2 * n : n;
    for (int s = 1; s <= n; s++) {
        val[s] = htoel(ctx.slavelist[s].pdelay - ctx.slavelist[newref].pdelay);
        for (int r = 0; r < (o_reset_filt ? 2 : 1); r++) {
            uint16 ado = r ? ECT_REG_DCSPEEDCNT : ECT_REG_DCSYSDELAY;
            uint16 l   = r ? 2 : 4;
            void  *dp  = r ? (void *)&scs : (void *)&val[s];
            if (nd == 0) {
                ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_FPWR, idx,
                                  ctx.slavelist[s].configadr, ado, l, dp);
                off[nd] = EC_HEADERSIZE;
            } else {
                off[nd] = ecx_adddatagram(port, &(port->txbuf[idx]), EC_CMD_FPWR, idx, nd < total - 1,
                                          ctx.slavelist[s].configadr, ado, l, dp);
            }
            len[nd++] = l;
        }
    }
    int sum = 0;
    if (ecx_srconfirm(port, idx, EC_TIMEOUTRET) > 0) {
        for (int k = 0; k < nd; k++) {
            uint16 le_wkc;
            memcpy(&le_wkc, &(port->rxbuf[idx][off[k] + len[k]]), sizeof le_wkc);
            sum += etohs(le_wkc);
        }
    }
    ecx_setbufstat(port, idx, EC_BUF_EMPTY);
    return sum;
}

/* ========================================================================== */
static void *rt_thread(void *arg)
{
    uint64_t settle_until = *(uint64_t *)arg;
    uint64_t next = (mono_ns() / (uint64_t)o_cycle_ns + 10) * (uint64_t)o_cycle_ns;
    uint32_t prev_lo = (uint32_t)ctx.DCtime;
    uint64_t since_wrap = 1000;
    uint64_t switch_at = o_switch_at_s ? settle_until - (uint64_t)o_settle_s * 1000000000ull
                                         + (uint64_t)o_switch_at_s * 1000000000ull : 0;
    int n = ctx.slavecount;

    /* upstream feed (L6-05): alpha-beta filter on x = (ref time read back)
     * - (master send time). The prediction t_send + x_pred then carries the
     * latency jitter of THIS frame only. The first version used "last read
     * + elapsed send time", which carries the jitter of the previous frame
     * too (variance x2) and lets one outlier leak into the next cycle. */
    int64_t  ab_x = 0, ab_v_q16 = 0;   /* ns, and ns per cycle in Q16 */
    int      ab_init = 0;
    uint64_t t_send_good = 0;

    while (g_run) {
        struct timespec ts; ns_to_ts(next, &ts);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        uint64_t t_wake = mono_ns();

        uint64_t t_send = mono_ns();
        int64_t  x_pred = ab_x;
        if (ab_init)
            x_pred += ((ab_v_q16 * (int64_t)(t_send - t_send_good)) / o_cycle_ns) >> 16;
        if (sw.done && !o_up_naive && ab_init)
            ctx.DCtime = (int64)(t_send + (uint64_t)x_pred);   /* SOEM copies it into the FRMW */
        ecx_send_processdata(&ctx);
        int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);

        if (wkc > 0) {
            if (!ab_init) {
                ab_x = (int64_t)((uint64_t)ctx.DCtime - t_send);
                ab_init = 1;
            } else {
                /* innovation, int32 view: identical for 64-bit, mod 2^32 for 32-bit */
                int64_t r = (int32_t)(uint32_t)((uint64_t)ctx.DCtime - (t_send + (uint64_t)x_pred));
                if (sw.done) {
                    int64_t ar = r < 0 ? -r : r;
                    sw.feed_n++; sw.feed_sum += r;
                    if (ar > sw.feed_absmax) sw.feed_absmax = ar;
                    uint64_t b = (uint64_t)(ar / 100); if (b >= FEED_HIST) b = FEED_HIST - 1;
                    feed_hist[b]++;
                }
                int64_t rc = r > 50000 ? 50000 : r < -50000 ? -50000 : r;   /* outlier influence cap */
                ab_x = x_pred + rc / 8;                 /* alpha = 1/8   */
                ab_v_q16 += (rc * 65536) / 128;         /* beta  = 1/128 */
            }
            t_send_good = t_send;
        }
        if (!g_clk_have0 && t_wake >= settle_until) {
            g_clk_raw0 = raw_ns(); g_clk_mono0 = mono_ns(); g_clk_have0 = 1;
        }
        ST_ADD(cycles, 1);
        if (wkc < expected_wkc) ST_ADD(wkc_low, 1);

        int64_t u = 0, e = 0; int have = 0;
        if (wkc > 0) {
            uint32_t lo = (uint32_t)ctx.DCtime;
            if (lo < prev_lo) { since_wrap = 0; ST_ADD(wraps_seen, 1); }
            prev_lo = lo;
            if (o_naive) { u = naive_update(ctx.DCtime); e = naive_err; }
            else         { u = ecm_dc_update(&g_dc, (uint64_t)ctx.DCtime, t_wake); e = g_dc.err_ns; }
            have = 1;
            ST_ADD(samples, 1);
        }

        if (have && t_wake >= settle_until) {
            int64_t ae = e < 0 ? -e : e;
            ST_ADD(sum_e, e); ST_ADD(sum_u, u); ST_ADD(n, 1);
            if (ae > ST_LD(win_emax)) ST_ST(win_emax, ae);
            uint64_t b = (uint64_t)(ae / 1000); if (b >= HIST_US) b = HIST_US - 1;
            hist[b]++;
            if (since_wrap <= 3) {
                ST_ADD(wrap_sum_e, e); ST_ADD(wrap_n, 1);
                if (ae > ST_LD(wrap_emax)) ST_ST(wrap_emax, ae);
            }
        }
        since_wrap++;

        /* ---- L6-05: after this cycle's exchange, only once, only when the
         * loop is locked (a switch during acquisition proves nothing) ---- */
        if (switch_at && !sw.done && t_wake >= switch_at && (o_naive || g_dc.state == ECM_DC_LOCKED)) {
            sw.frame_n = o_reset_filt ? 2 * n : n;
            sw.frame_wkc = o_no_delay_rw ? -1 : write_delays_one_frame(n, o_new_ref);
            ctx.grouplist[0].DCnext = (uint16)o_new_ref;   /* next FRMW goes to slave K */
            sw.t_ns = t_wake;
            __atomic_store_n(&sw.done, 1, __ATOMIC_RELEASE);
        }

        next += (uint64_t)(o_cycle_ns + u);
        if (mono_ns() > next) ST_ADD(overruns, 1);
    }
    return NULL;
}

static int64_t pct_us(double p, uint64_t total)
{
    uint64_t want = (uint64_t)(p * (double)total), acc = 0;
    for (int i = 0; i < HIST_US; i++) { acc += hist[i]; if (acc > want) return i; }
    return HIST_US - 1;
}

#define CHECK(ok, ...) do { printf("%s ", (ok) ? "  PASS" : "  FAIL"); printf(__VA_ARGS__); \
                            printf("\n"); if (!(ok)) fails++; } while (0)

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i]; const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--iface") && v)         { o_iface = v; i++; }
        else if (!strcmp(a, "--duration-sec") && v)  { o_duration_s = atoi(v); i++; }
        else if (!strcmp(a, "--cycle-us") && v)      { o_cycle_ns = atoll(v) * 1000; i++; }
        else if (!strcmp(a, "--setpoint-pct") && v)  { o_setpoint_pct = atoi(v); i++; }
        else if (!strcmp(a, "--expect-hop-ns") && v) { o_expect_hop = atoll(v); i++; }
        else if (!strcmp(a, "--report-sec") && v)    { o_report_s = atoi(v); i++; }
        else if (!strcmp(a, "--settle-sec") && v)    { o_settle_s = atoi(v); i++; }
        else if (!strcmp(a, "--rt-prio") && v)       { o_rt_prio = atoi(v); i++; }
        else if (!strcmp(a, "--cpu") && v)           { o_cpu = atoi(v); i++; }
        else if (!strcmp(a, "--naive"))              { o_naive = 1; }
        else if (!strcmp(a, "--expect-ref-ppm") && v){ o_expect_ppm = atof(v); o_has_expect = 1; i++; }
        else if (!strcmp(a, "--switch-ref-at-sec") && v) { o_switch_at_s = atoi(v); i++; }
        else if (!strcmp(a, "--new-ref") && v)       { o_new_ref = atoi(v); i++; }
        else if (!strcmp(a, "--upstream") && v)      { o_up_naive = !strcmp(v, "naive"); i++; }
        else if (!strcmp(a, "--no-delay-rewrite"))   { o_no_delay_rw = 1; }
        else if (!strcmp(a, "--reset-filters"))      { o_reset_filt = 1; }
        else { fprintf(stderr, "unknown arg %s\n", a); return 2; }
    }
    if (!o_iface) { fprintf(stderr, "usage: %s --iface veth_m [...]\n", argv[0]); return 2; }
    signal(SIGINT, on_sigint);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    int fails = 0;
    int64_t cyc = o_cycle_ns, sp = o_cycle_ns * o_setpoint_pct / 100;
    printf("l6_test: iface=%s cycle=%lld ns setpoint=%lld ns (%d%%) duration=%d s mode=%s\n",
           o_iface, (long long)cyc, (long long)sp, o_setpoint_pct, o_duration_s,
           o_naive ? "NAIVE (negative control)" : "ecm_dc");

    if (!ecx_init(&ctx, o_iface)) { fprintf(stderr, "ecx_init failed\n"); return 2; }
    if (ecx_config_init(&ctx) <= 0) { fprintf(stderr, "no slaves\n"); return 2; }
    ecx_config_map_group(&ctx, IOmap, 0);
    expected_wkc = ctx.grouplist[0].outputsWKC * 2 + ctx.grouplist[0].inputsWKC;
    int n = ctx.slavecount;
    printf("slaves=%d expectedWKC=%d\n", n, expected_wkc);
    if (o_switch_at_s && (o_new_ref < 2 || o_new_ref > n)) {
        fprintf(stderr, "--new-ref must be 2..%d\n", n); return 2;
    }
    if (o_switch_at_s)
        printf("L6-05 plan: at t=%d s move ref 1 -> %d, upstream feed=%s, delay rewrite=%s%s\n",
               o_switch_at_s, o_new_ref, o_up_naive ? "NAIVE (SOEM default, negative control)" : "predict (alpha-beta)",
               o_no_delay_rw ? "OFF (negative control)" : "one frame",
               o_reset_filt ? " + 0x0930 filter reset" : "");

    /* ---------------- L6-01 ---------------- */
    ecx_configdc(&ctx);
    printf("\n[L6-01] propagation delay after ecx_configdc\n");
    int all_dc = 1, mono = 1, hop_ok = 1;
    for (int s = 1; s <= n; s++) {
        ec_slavet *sl = &ctx.slavelist[s];
        printf("  slave %d addr=0x%04x hasdc=%d pdelay=%d ns\n", s, sl->configadr, sl->hasdc, sl->pdelay);
        if (!sl->hasdc) all_dc = 0;
        if (s > 1 && sl->pdelay <= ctx.slavelist[s - 1].pdelay) mono = 0;
        if (o_expect_hop >= 0) {
            int64_t want = (int64_t)(s - 1) * o_expect_hop;
            if (llabs((long long)(sl->pdelay - want)) > 2) hop_ok = 0;
        }
    }
    uint16_t ref = ctx.grouplist[0].DCnext;
    CHECK(all_dc && ctx.grouplist[0].hasdc, "L6-01a every slave DC-capable, group 0 carries FRMW");
    CHECK(ref == 1, "L6-01b reference clock = first DC slave (DCnext=%u)", ref);
    CHECK(ctx.slavelist[1].pdelay == 0 && mono, "L6-01c pdelay strictly increasing along the chain");
    if (o_expect_hop >= 0)
        CHECK(hop_ok, "L6-01d pdelay == (i-1) * %lld ns (+-2 ns)", (long long)o_expect_hop);

    uint16_t feat = ecx_FPRDw(&ctx.port, ctx.slavelist[ref].configadr, ECT_REG_ESCSUP, EC_TIMEOUTRET);
    printf("  ref clock DC width: %s (0x0008=0x%04x)\n", (feat & 0x0008) ? "64-bit" : "32-bit", feat);

    /* ---------------- L6-02 (master side) ---------------- */
    printf("\n[L6-02] SYNC0 configuration\n");
    int regs_ok = 1;
    for (int s = 1; s <= n; s++) {
        ecx_dcsync0(&ctx, (uint16)s, TRUE, (uint32)cyc, 0);
        uint8_t act = 0; uint32_t c0 = 0;
        ecx_FPRD(&ctx.port, ctx.slavelist[s].configadr, ECT_REG_DCSYNCACT, 1, &act, EC_TIMEOUTRET);
        ecx_FPRD(&ctx.port, ctx.slavelist[s].configadr, ECT_REG_DCCYCLE0, 4, &c0, EC_TIMEOUTRET);
        c0 = etohl(c0);
        if (act != 0x03 || c0 != (uint32_t)cyc) regs_ok = 0;
        printf("  slave %d: 0x0981=0x%02x 0x09A0=%u\n", s, act, c0);
    }
    CHECK(regs_ok, "L6-02a 0x0981==0x03 and 0x09A0==cycle on every slave");

    /* anchor: ONE datagram 0x0910..0x0997 from the reference clock */
    uint8_t snap[0x88];
    memset(snap, 0, sizeof snap);
    int w = ecx_FPRD(&ctx.port, ctx.slavelist[ref].configadr, ECT_REG_DCSYSTIME, sizeof snap, snap, EC_TIMEOUTRET);
    uint64_t t_anchor = mono_ns();
    uint64_t dc_raw = le64(snap), s0_raw = le64(snap + 0x80);
    int32_t lead = (int32_t)((uint32_t)s0_raw - (uint32_t)dc_raw);
    printf("  anchor: wkc=%d DCtime=0x%016" PRIx64 " nextSYNC0=0x%016" PRIx64 " lead=%.3f ms\n",
           w, dc_raw, s0_raw, lead / 1e6);
    CHECK(w == 1 && lead > 0 && lead <= 100000000 + 2 * cyc, "L6-02b first SYNC0 in the future, <= SyncDelay+2 cycles");

    ecm_dc_cfg_t cfg;
    ecm_dc_default_cfg(&cfg, cyc, sp);
    ecm_dc_init(&g_dc, &cfg);
    ecm_dc_anchor(&g_dc, dc_raw, s0_raw, t_anchor);
    naive_syncoffset = (int64_t)(uint32_t)s0_raw + sp;
    if (feat & 0x0008) naive_syncoffset = (int64_t)s0_raw + sp;
    ctx.DCtime = (int64)dc_raw;

    /* ---------------- cyclic ---------------- */
    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);

    pthread_t th; pthread_attr_t at;
    pthread_attr_init(&at);
    struct sched_param prm = { .sched_priority = o_rt_prio };
    if (o_rt_prio > 0) {
        pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&at, SCHED_FIFO);
        pthread_attr_setschedparam(&at, &prm);
    }
    if (o_cpu >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(o_cpu, &cs);
        pthread_attr_setaffinity_np(&at, sizeof cs, &cs);
    }
    uint64_t settle_until = mono_ns() + (uint64_t)o_settle_s * 1000000000ull;
    if (pthread_create(&th, &at, rt_thread, &settle_until) != 0) {
        fprintf(stderr, "SCHED_FIFO %d refused, falling back to SCHED_OTHER\n", o_rt_prio);
        pthread_attr_destroy(&at);
        pthread_attr_init(&at);
        pthread_create(&th, &at, rt_thread, &settle_until);
    }
    pthread_attr_destroy(&at);   /* setaffinity_np allocates inside the attr */

    /* Let the cyclic LRW run before requesting OP: soft_bus (like a real
     * slave) refuses SAFEOP->OP until it has seen valid outputs (0x0019).
     * The first run of this tool requested OP right after pthread_create
     * and got SAFEOP+ERROR (0x14). */
    struct timespec warm = { 0, 300000000 };
    nanosleep(&warm, NULL);

    ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&ctx, 0);
    ecx_statecheck(&ctx, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
    int op_ok = (ctx.slavelist[0].state == EC_STATE_OPERATIONAL);
    printf("\nstate after OP request: 0x%02x (%s)\n", ctx.slavelist[0].state, op_ok ? "OP" : "NOT OP");
    if (!op_ok) {
        ecx_readstate(&ctx);
        for (int s = 1; s <= n; s++)
            printf("  slave %d state=0x%02x ALstatuscode=0x%04x (%s)\n", s, ctx.slavelist[s].state,
                   ctx.slavelist[s].ALstatuscode, ec_ALstatuscode2string(ctx.slavelist[s].ALstatuscode));
    }

    uint64_t t0 = mono_ns(), last_n = 0; int64_t last_se = 0, last_su = 0;
    uint64_t w_raw = raw_ns(), w_mono = mono_ns();
    int sw_announced = 0;
    while (!g_stop_req && mono_ns() - t0 < (uint64_t)o_duration_s * 1000000000ull) {
        struct timespec sl = { o_report_s, 0 };
        nanosleep(&sl, NULL);
        uint64_t nn = ST_LD(n); int64_t se = ST_LD(sum_e), su = ST_LD(sum_u);
        uint64_t dn = nn - last_n;
        int64_t emax = __atomic_exchange_n(&st.win_emax, 0, __ATOMIC_RELAXED);
        uint64_t r1 = raw_ns(), m1 = mono_ns();
        double mono_ppm = ((double)(m1 - w_mono) - (double)(r1 - w_raw)) / (double)(r1 - w_raw) * 1e6;
        w_raw = r1; w_mono = m1;
        if (__atomic_load_n(&sw.done, __ATOMIC_ACQUIRE) && !sw_announced) {
            printf("L6-05 switch done at t=%.3f s: 0x0928 frame wkc=%d/%d, DCnext=%u\n",
                   (sw.t_ns - t0) / 1e9, sw.frame_wkc, sw.frame_n, ctx.grouplist[0].DCnext);
            sw_announced = 1;
        }
        printf("t=%5.1fs state=%s wraps=%" PRIu64 " stale=%" PRIu64 " wkc_low=%" PRIu64
               " overrun=%" PRIu64 " | win: n=%" PRIu64 " mean_e=%+.2fus max|e|=%.1fus drift=%+.0fppb"
               " mono_vs_raw=%+.3fppm\n",
               (mono_ns() - t0) / 1e9,
               o_naive ? "naive" : (g_dc.state == ECM_DC_LOCKED ? "LOCKED" : g_dc.state == ECM_DC_ACQUIRE ? "ACQUIRE" : "UNANCH"),
               ST_LD(wraps_seen), g_dc.stale, ST_LD(wkc_low), ST_LD(overruns), dn,
               dn ? (double)(se - last_se) / dn / 1000.0 : 0.0, emax / 1000.0,
               dn ? -(double)(su - last_su) / dn / (double)cyc * 1e9 : 0.0, mono_ppm);
        fflush(stdout);
        last_n = nn; last_se = se; last_su = su;
    }

    g_run = 0;
    pthread_join(th, NULL);
    uint64_t clk_raw1 = raw_ns(), clk_mono1 = mono_ns();

    /* L6-05 read-back, BEFORE leaving OP: state of every slave + 0x0928 */
    int sw_state_ok = 1, sw_delay_ok = 1;
    if (o_switch_at_s) {
        ecx_readstate(&ctx);
        printf("\n[L6-05] after the reference switch (read back before INIT)\n");
        for (int s = 1; s <= n; s++) {
            int32 d = 0;
            int rw = ecx_FPRD(&ctx.port, ctx.slavelist[s].configadr, ECT_REG_DCSYSDELAY, 4, &d, EC_TIMEOUTRET);
            d = (int32)etohl(d);
            if (rw != 1) sw_delay_ok = 0;
            int32 want = o_no_delay_rw ? ctx.slavelist[s].pdelay
                                       : ctx.slavelist[s].pdelay - ctx.slavelist[o_new_ref].pdelay;
            if (d != want) sw_delay_ok = 0;
            if (ctx.slavelist[s].state != EC_STATE_OPERATIONAL || ctx.slavelist[s].ALstatuscode) sw_state_ok = 0;
            printf("  slave %d%s state=0x%02x ALstatuscode=0x%04x 0x0928=%d ns (want %d, wkc=%d)\n", s,
                   s == o_new_ref ? " [REF]" : s < o_new_ref ? " [upstream]" : "",
                   ctx.slavelist[s].state, ctx.slavelist[s].ALstatuscode, d, want, rw);
        }
    }

    ctx.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&ctx, 0);

    /* ---------------- summary ---------------- */
    uint64_t nn = st.n;
    double run_s = (mono_ns() - t0) / 1e9;
    printf("\n[L6-04] %s, %.1f s in OP, %" PRIu64 " samples after settle\n",
           o_naive ? "NAIVE" : "ecm_dc", run_s, nn);
    printf("  |e| p50=%lld us p99=%lld us p99.9=%lld us p99.99=%lld us\n",
           (long long)pct_us(0.50, nn), (long long)pct_us(0.99, nn),
           (long long)pct_us(0.999, nn), (long long)pct_us(0.9999, nn));
    printf("  mean e=%+.2f us, drift(ref vs master)=%+.0f ppb\n",
           nn ? (double)st.sum_e / nn / 1000.0 : 0.0,
           nn ? -(double)st.sum_u / nn / (double)cyc * 1e9 : 0.0);
    printf("  wraps seen=%" PRIu64 " (ecm_dc counted %" PRIu64 "); 4 samples after each wrap: "
           "mean e=%+.2f us max|e|=%.1f us\n",
           st.wraps_seen, g_dc.wraps,
           st.wrap_n ? (double)st.wrap_sum_e / st.wrap_n / 1000.0 : 0.0, st.wrap_emax / 1000.0);
    printf("  ecm_dc: locks=%" PRIu64 " unlocks=%" PRIu64 " clamps=%" PRIu64 " stale=%" PRIu64 "\n",
           g_dc.locks, g_dc.unlocks, g_dc.clamps, g_dc.stale);

    CHECK(op_ok, "OP reached");
    uint64_t expect_wraps = (uint64_t)((run_s - 1.0) / 4.294967296);
    CHECK(st.wraps_seen >= expect_wraps, "32-bit wraps crossed during run: %" PRIu64 " (>= %" PRIu64 ")",
          st.wraps_seen, expect_wraps);
    if (!o_naive) {
        CHECK(g_dc.state == ECM_DC_LOCKED && g_dc.unlocks == 0, "L6-04s locked for the whole run, 0 unlocks");
        double mean_all  = nn ? (double)st.sum_e / nn : 0.0;
        double mean_wrap = st.wrap_n ? (double)st.wrap_sum_e / st.wrap_n : 0.0;
        CHECK(st.wrap_n > 0 && mean_wrap - mean_all < 10000.0 && mean_wrap - mean_all > -10000.0,
              "L6-04s no wrap-correlated phase step (mean e after wrap - overall = %+.2f us, limit +-10)",
              (mean_wrap - mean_all) / 1000.0);
    }
    /* ---------------- CLK ---------------- */
    double drift_ppb = nn ? -(double)st.sum_u / nn / (double)cyc * 1e9 : 0.0;
    if (g_clk_have0) {
        double eff = ((double)(clk_mono1 - g_clk_mono0) - (double)(clk_raw1 - g_clk_raw0))
                     / (double)(clk_raw1 - g_clk_raw0) * 1e6;
        printf("\n[CLK] CLOCK_MONOTONIC vs RAW over the same span as the drift mean: %+.3f ppm\n", eff);
        if (o_has_expect) {
            double pred = (o_expect_ppm - eff) * 1000.0, resid = drift_ppb - pred;
            printf("  measured drift %+.0f ppb, predicted (ref %+.3f ppm - mono %+.3f ppm) %+.0f ppb, residual %+.0f ppb\n",
                   drift_ppb, o_expect_ppm, eff, pred, resid);
            /* -sum(u)/(N*cycle) misses (phi_end - phi_start)/T, phi = phase of
             * the master's deadline: a controller that answers measurement
             * noise moves phi by up to ~100 us on a jittery host. Allow that
             * much over the span, never less than 1 ppm (1 h -> 1 ppm). */
            double span_s = (double)(clk_raw1 - g_clk_raw0) / 1e9;
            double lim = 100000.0 / span_s; if (lim < 1000.0) lim = 1000.0;
            if (!o_naive)
                CHECK(resid < lim && resid > -lim,
                      "CLK drift == injected ref drift - MONOTONIC slew (residual %+.0f ppb, limit +-%.0f over %.0f s)",
                      resid, lim, span_s);
        }
    }

    /* ---------------- L6-05 ---------------- */
    if (o_switch_at_s) {
        uint64_t want = (uint64_t)(0.99 * (double)sw.feed_n), acc = 0; int64_t p99 = -1;
        for (int b = 0; b < FEED_HIST; b++) { acc += feed_hist[b]; if (acc > want) { p99 = b; break; } }
        printf("\n[L6-05] ref 1 -> %d at t=%.3f s, 0x0928 frame wkc=%d/%d\n",
               o_new_ref, sw.done ? (sw.t_ns - t0) / 1e9 : -1.0, sw.frame_wkc, sw.frame_n);
        printf("  feed innovation (ref time read back - alpha-beta prediction; this is what %s): "
               "n=%" PRIu64 " mean=%+.1f ns p99|.|=%.1f us max|.|=%.1f us\n",
               o_up_naive ? "the upstream slaves WOULD get with prediction; they got the stale DCtime"
                          : "the upstream slaves get as error",
               sw.feed_n, sw.feed_n ? (double)sw.feed_sum / sw.feed_n : 0.0,
               p99 / 10.0, sw.feed_absmax / 1000.0);
        CHECK(sw.done, "L6-05a switch executed while LOCKED");
        if (!o_no_delay_rw)
            CHECK(sw.frame_wkc == sw.frame_n, "L6-05b 0x0928 rewrite: one frame, WKC %d/%d", sw.frame_wkc, sw.frame_n);
        CHECK(sw_delay_ok, "L6-05c 0x0928 read back == pdelay_i - pdelay_%d on every slave%s", o_new_ref,
              o_no_delay_rw ? " (negative control: expected OLD values)" : "");
        CHECK(ctx.grouplist[0].DCnext == o_new_ref, "L6-05d FRMW targets slave %d (DCnext=%u)",
              o_new_ref, ctx.grouplist[0].DCnext);
        CHECK(sw_state_ok, "L6-05e every slave still OP, AL status code 0 (soft_bus does not model sync "
              "errors: the real evidence is soft_bus GT-b/GT-c)");
    }

    printf("\nRESULT(master side): %d fail\n", fails);
    ecx_close(&ctx);
    return fails ? 1 : 0;
}
