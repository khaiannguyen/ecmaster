/* ==========================================================================
 * test_dc_e2e.c — offline closed-loop simulation of Phase 6, simulated time.
 *
 *   master side : ecm_dc (DC(b) PI)  +  SOEM-equivalent ecx_configdc /
 *                 ecx_dcsync0 sequences, rebuilt datagram-by-datagram
 *   slave side  : the real soft_bus esc_core.c + esc_dc.c (not a mock)
 *
 * Time is simulated (no sleeping): 1 h of bus time runs in seconds, which is
 * what makes L6-03/L6-04 checkable BEFORE touching veth/Jetson.
 *
 * Build: gcc -std=gnu11 -O2 -Wall -I../../tools/soft_bus -I../../libecmaster/core -o test_dc_e2e \
 *            test_dc_e2e.c ../../tools/soft_bus/esc_core.c ../../tools/soft_bus/esc_dc.c ../../libecmaster/core/ecm_dc.c
 * Run:   ./test_dc_e2e            (all scenarios, exit 0 = all pass)
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "esc_types.h"
#include "esc_core.h"
#include "esc_dc.h"
#include "ecm_dc.h"

#define N_NODES     8
#define CYCLE_NS    1000000LL
#define SETPOINT_NS 300000LL                     /* 30 % after SYNC0 */
#define EPOCH2000   (26LL * 365 * 86400 * 1000000000LL) /* ~"now" since 2000 */
#define SYNC_DELAY  100000000LL                  /* SOEM ec_dc.c SyncDelay */

static esc_t    chain[N_NODES];
static uint64_t T;                               /* simulated host time, ns */
static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static uint64_t rd64le(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)rd64le(p) ; }
static void     wr64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; } }

/* ---- frame builder: up to 2 datagrams per frame ---- */
typedef struct { uint8_t cmd; uint16_t adp, ado, len; uint8_t *data; uint16_t wkc; } dg_t;

static void xfer(dg_t *d, int nd)
{
    uint8_t buf[1600] = {0};
    size_t off = 16;
    buf[12] = 0x88; buf[13] = 0xA4;
    for (int i = 0; i < nd; i++) {
        uint8_t *g = buf + off;
        g[0] = d[i].cmd;
        g[2] = (uint8_t)d[i].adp; g[3] = (uint8_t)(d[i].adp >> 8);
        g[4] = (uint8_t)d[i].ado; g[5] = (uint8_t)(d[i].ado >> 8);
        uint16_t lf = (uint16_t)(d[i].len | (i < nd - 1 ? 0x8000 : 0));
        g[6] = (uint8_t)lf; g[7] = (uint8_t)(lf >> 8);
        memcpy(g + 10, d[i].data, d[i].len);
        off += 10 + d[i].len + 2;
    }
    uint16_t eclen = (uint16_t)(off - 16);
    buf[14] = (uint8_t)(eclen | 0); buf[15] = (uint8_t)(0x10 | (eclen >> 8)); /* type 1 */

    esc_dc_frame_begin(chain, N_NODES, T);
    process_frame(chain, N_NODES, buf, off);
    esc_dc_frame_end(chain, N_NODES, esc_dc_frame_has_pd(buf, off));

    off = 16;
    for (int i = 0; i < nd; i++) {
        uint8_t *g = buf + off;
        memcpy(d[i].data, g + 10, d[i].len);
        d[i].wkc = (uint16_t)(g[10 + d[i].len] | (g[11 + d[i].len] << 8));
        off += 10 + d[i].len + 2;
    }
}

static uint16_t one(uint8_t cmd, uint16_t adp, uint16_t ado, void *data, uint16_t len)
{
    dg_t d = { cmd, adp, ado, len, data, 0 };
    xfer(&d, 1);
    T += 60000;                      /* each blocking transaction ~60 us */
    return d.wkc;
}

static uint16_t sta(int i) { return (uint16_t)(0x1001 + i); }

/* ---- SOEM ecx_configdc(), line topology, rebuilt datagram by datagram ---- */
static int64_t pdelay[N_NODES];

static void configdc(void)
{
    uint8_t z[4] = {0};
    one(CMD_BWR, 0, 0x0900, z, 4);                      /* latch all */
    int64_t mastertime = (int64_t)T + EPOCH2000;
    uint32_t rtA[N_NODES], rtB[N_NODES];

    for (int i = 0; i < N_NODES; i++) {
        uint8_t b[8];
        one(CMD_FPRD, sta(i), 0x0900, b, 4); rtA[i] = rd32le(b);
        one(CMD_FPRD, sta(i), 0x0918, b, 8);
        int64_t hrt = -(int64_t)rd64le(b) + mastertime;
        wr64le(b, (uint64_t)hrt);
        one(CMD_FPWR, sta(i), 0x0920, b, 8);
        one(CMD_FPRD, sta(i), 0x0904, b, 4); rtB[i] = rd32le(b);

        pdelay[i] = 0;
        if (i > 0) {
            int32_t dt3 = (int32_t)(rtB[i - 1] - rtA[i - 1]);
            int32_t dt1 = (i < N_NODES - 1) ? (int32_t)(rtB[i] - rtA[i]) : 0;
            pdelay[i] = (dt3 - dt1) / 2 + pdelay[i - 1];
            uint8_t w[4] = { (uint8_t)pdelay[i], (uint8_t)(pdelay[i] >> 8),
                             (uint8_t)(pdelay[i] >> 16), (uint8_t)(pdelay[i] >> 24) };
            one(CMD_FPWR, sta(i), 0x0928, w, 4);
        }
    }
}

/* ---- SOEM ecx_dcsync0(act=TRUE, CyclShift=0) ---- */
static void dcsync0(int i)
{
    uint8_t ra = 0, h = 0, b[8];
    one(CMD_FPWR, sta(i), 0x0981, &ra, 1);
    one(CMD_FPWR, sta(i), 0x0980, &h, 1);
    one(CMD_FPRD, sta(i), 0x0910, b, 8);
    int64_t t1 = (int64_t)rd64le(b);
    int64_t t  = ((t1 + SYNC_DELAY) / CYCLE_NS) * CYCLE_NS + CYCLE_NS;
    wr64le(b, (uint64_t)t);
    one(CMD_FPWR, sta(i), 0x0990, b, 8);
    uint8_t c[4] = { (uint8_t)CYCLE_NS, (uint8_t)(CYCLE_NS >> 8), (uint8_t)(CYCLE_NS >> 16), (uint8_t)(CYCLE_NS >> 24) };
    one(CMD_FPWR, sta(i), 0x09A0, c, 4);
    ra = 3;
    one(CMD_FPWR, sta(i), 0x0981, &ra, 1);
}

/* ---- naive controller = SOEM sample ec_sync(), same gains as ecm_dc ---- */
typedef struct { int64_t syncoffset, integ, err; } naive_t;
static int64_t naive_update(naive_t *nv, int64_t reftime)
{
    int64_t delta = (reftime - nv->syncoffset) % CYCLE_NS;
    if (delta < 0) delta += CYCLE_NS;
    if (delta > CYCLE_NS / 2) delta -= CYCLE_NS;
    nv->err = delta;
    nv->integ += delta;
    int64_t u = -(delta / 8 + nv->integ / 256);
    if (u >  CYCLE_NS / 20) { u =  CYCLE_NS / 20; nv->integ -= delta; }
    if (u < -CYCLE_NS / 20) { u = -CYCLE_NS / 20; nv->integ -= delta; }
    return u;
}

/* ========================================================================== */
typedef struct {
    const char *name;
    int  width;
    int  naive;
    double seconds;
    int64_t ref_drift_ppb;
} scen_t;

static int run(const scen_t *s)
{
    memset(chain, 0, sizeof(chain));
    for (int i = 0; i < N_NODES; i++) esc_init(&chain[i], (uint8_t)i, 4);
    esc_chain_wire(chain, N_NODES);
    esc_dc_cfg_t dcfg = { s->width, 800, s->ref_drift_ppb, 20000 };
    esc_dc_setup(chain, N_NODES, &dcfg);

    T = 5000000000ull;
    for (int i = 0; i < N_NODES; i++) {                /* station addresses */
        uint8_t a[2] = { (uint8_t)sta(i), (uint8_t)(sta(i) >> 8) };
        one(CMD_APWR, (uint16_t)(-i), 0x0010, a, 2);
    }
    /* ecx_config_init() does this BEFORE ecx_configdc (ec_config.c:105-109):
     * BWR 0x0910 = 0 (4 byte) -> huge dt in every node's control loop,
     * then BWR 0x0930 (speed counter start) which must reset the loop.
     * Missing this reset in soft_bus left the ref clock at -450 ppm on the
     * first real SOEM run. */
    { uint8_t z4[4] = {0}; one(CMD_BWR, 0, 0x0910, z4, 4);
      uint8_t w2[2] = {0x00, 0x10}; one(CMD_BWR, 0, 0x0930, w2, 2); }
    configdc();

    /* L6-01 check */
    int mono = 1;
    for (int i = 1; i < N_NODES; i++) if (pdelay[i] <= pdelay[i - 1]) mono = 0;

    for (int i = 0; i < N_NODES; i++) dcsync0(i);

    /* anchor: ONE datagram 0x0910..0x0997 from the reference clock (node 0) */
    uint8_t snap[0x88];
    one(CMD_FPRD, sta(0), 0x0910, snap, sizeof(snap));
    uint64_t dc_raw = rd64le(snap), s0_raw = rd64le(snap + 0x80);

    ecm_dc_t dc; ecm_dc_cfg_t cfg;
    ecm_dc_default_cfg(&cfg, CYCLE_NS, SETPOINT_NS);
    ecm_dc_init(&dc, &cfg);
    ecm_dc_anchor(&dc, dc_raw, s0_raw, T);
    naive_t nv = { (int64_t)(uint32_t)s0_raw + SETPOINT_NS, 0, 0 };

    int64_t ctx_DCtime = (int64_t)dc_raw;
    uint64_t D = (T / CYCLE_NS + 2) * CYCLE_NS;
    uint64_t ncycles = (uint64_t)(s->seconds * 1e9 / CYCLE_NS);
    uint64_t settle = 2000;                           /* ignore first 2 s */

    int64_t emax = 0; uint64_t e_over10 = 0, lost = 0; __int128 usum = 0, isum = 0; uint64_t un = 0;
    uint32_t prev_lo = (uint32_t)dc_raw; uint64_t since_wrap = 1000, nwrap_seen = 0;
    int64_t wrap_emax = 0; __int128 wrap_esum = 0; uint64_t wrap_en = 0;
    uint64_t nofr0 = 0, nofr_all0 = 0, first_slip_cycle = 0;
    int64_t gt_min = INT64_MAX, gt_max = INT64_MIN;
    FILE *devnull = fopen("/dev/null", "w");

    for (uint64_t k = 0; k < ncycles; k++) {
        /* wake latency: mostly 3-18 us, rare 120 us spikes (one-sided) */
        uint64_t lat = 3000 + xr() % 15000 + ((xr() % 10000) == 0 ? 120000 : 0);
        uint64_t wake = D + lat;
        T = wake + 20000 + xr() % 5000;               /* prep+send+wire */

        int64_t lrwbuf = 0; int64_t frmw = ctx_DCtime;
        dg_t d[2] = {
            { CMD_LRW,  0x0000, 0x0001, 4, (uint8_t *)&lrwbuf, 0 },
            { CMD_FRMW, sta(0), 0x0910, 8, (uint8_t *)&frmw,   0 },
        };
        int frame_lost = (xr() % 100000) == 0;
        xfer(d, 2);
        int64_t u = 0;
        if (!frame_lost) {
            ctx_DCtime = frmw;
            if ((uint32_t)ctx_DCtime < prev_lo) { since_wrap = 0; nwrap_seen++; }
            prev_lo = (uint32_t)ctx_DCtime;
            if (s->naive) u = naive_update(&nv, ctx_DCtime);
            else          u = ecm_dc_update(&dc, (uint64_t)ctx_DCtime, wake);
        } else lost++;

        if (k == settle) {
            nofr0 = chain[0].dc.tot_no_frame;
            for (int i = 0; i < N_NODES; i++) nofr_all0 += chain[i].dc.tot_no_frame;
            esc_dc_report(chain, N_NODES, devnull, 0);
        }
        if (k > settle) {
            int64_t e = s->naive ? nv.err : dc.err_ns;
            int64_t ae = e < 0 ? -e : e;
            if (ae > emax) emax = ae;
            if (ae > 10000) e_over10++;
            usum += u; isum += dc.integ; un++;
            if (since_wrap <= 3) {                    /* 4 samples right after a wrap */
                if (ae > wrap_emax) wrap_emax = ae;
                wrap_esum += e; wrap_en++;
            }
            if (!first_slip_cycle && chain[0].dc.tot_no_frame > nofr0) first_slip_cycle = k;
            if (k % 10000 == 0) {                     /* fold 10 s windows */
                if (chain[0].dc.w_phase_min < gt_min) gt_min = chain[0].dc.w_phase_min;
                if (chain[0].dc.w_phase_max > gt_max) gt_max = chain[0].dc.w_phase_max;
                esc_dc_report(chain, N_NODES, devnull, 0);
            }
        }
        since_wrap++;
        D += (uint64_t)((int64_t)CYCLE_NS + u);
    }
    fclose(devnull);

    uint64_t nofr_all = 0;
    for (int i = 0; i < N_NODES; i++) nofr_all += chain[i].dc.tot_no_frame;
    nofr_all -= nofr_all0;

    printf("\n=== %s  (DC %d-bit, %.0f s, ref drift %+lld ppb, %s) ===\n",
           s->name, s->width, s->seconds, (long long)s->ref_drift_ppb,
           s->naive ? "NAIVE raw%%cycle" : "ecm_dc");
    printf("  pdelay[ns]:");
    for (int i = 0; i < N_NODES; i++) printf(" %lld", (long long)pdelay[i]);
    printf("  -> monotonic=%s\n", mono ? "yes" : "NO");
    printf("  master: state=%d locks=%llu unlocks=%llu wraps=%llu stale=%llu lost=%llu\n",
           dc.state, (unsigned long long)dc.locks, (unsigned long long)dc.unlocks,
           (unsigned long long)dc.wraps, (unsigned long long)dc.stale, (unsigned long long)lost);
    printf("  master: max|e| after settle = %.2f us, samples |e|>10us = %llu, drift est = %lld ppb\n",
           emax / 1000.0, (unsigned long long)e_over10, (long long)ecm_dc_ref_drift_ppb(&dc));
    printf("  master: mean(u) -> ref drift = %.0f ppb, mean(integ)/ki -> %.0f ppb\n",
           -(double)usum / (double)un / CYCLE_NS * 1e9, (double)isum / (double)un / 256.0 / CYCLE_NS * 1e9);
    printf("  master: 32-bit wraps seen=%llu, in 4 samples after each wrap: max|e|=%.2f us, mean e=%+.2f us\n",
           (unsigned long long)nwrap_seen, wrap_emax / 1000.0,
           wrap_en ? (double)wrap_esum / (double)wrap_en / 1000.0 : 0.0);
    printf("  GROUND TRUTH node0 phase after SYNC0: min=%.1f us max=%.1f us (setpoint %.0f us)\n",
           gt_min / 1000.0, gt_max / 1000.0, SETPOINT_NS / 1000.0);
    printf("  GROUND TRUTH SYNC0 intervals without PD frame (all nodes, after settle) = %llu",
           (unsigned long long)nofr_all);
    if (first_slip_cycle) printf("  first slip at t=%.1f s", first_slip_cycle / 1000.0);
    printf("\n  node-side dt (last, ns):");
    for (int i = 0; i < N_NODES; i++) printf(" %d", chain[i].dc.last_dt_ns);
    printf("\n");

    int ctrl_ok = s->naive ? 1 : (dc.unlocks == 0 && dc.state == ECM_DC_LOCKED);
    printf("  ref clock node0 trim_ppb=%lld (must be 0: nobody writes 0x0910 of the ref)\n",
           (long long)chain[0].dc.trim_ppb);
    if (chain[0].dc.trim_ppb != 0) ctrl_ok = 0;
    int ok = mono && nofr_all == 0 && ctrl_ok
             && gt_min > SETPOINT_NS - 150000 && gt_max < SETPOINT_NS + 250000;
    return ok;
}

int main(void)
{
    scen_t sc[] = {
        { "S1 L6-03 64-bit, 10 min",          64, 0,  600.0,  50000 },
        { "S2 L6-04 32-bit, 1 h",             32, 0, 3600.0,  50000 },
        { "S3 L6-04 negative control, naive", 32, 1,  300.0,  50000 },
        { "S4 32-bit, ref drift negative",    32, 0,  600.0, -80000 },
    };
    int expect[] = { 1, 1, 0, 1 };
    int fails = 0;
    for (size_t i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
        int ok = run(&sc[i]);
        int pass = (ok == expect[i]);
        printf("  -> %s (expected %s)\n", pass ? "PASS" : "FAIL", expect[i] ? "clean" : "slips");
        if (!pass) fails++;
    }
    printf("\nRESULT: %d scenario(s) failed\n", fails);
    return fails ? 1 : 0;
}
