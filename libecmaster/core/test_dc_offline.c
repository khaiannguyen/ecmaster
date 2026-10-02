/* ==========================================================================
 * test_dc_offline.c — Phase 7.4: ecm_dc reply-age gate (L5-07);
 * W1/W2 (26/9): closed loop across 32-bit wraps, one late send.
 * Synthetic reference clock: ref = host * (1 + drift) + offset, sampled once
 * per 1 ms cycle at the setpoint phase.   make test
 * ========================================================================== */
#include <stdio.h>
#include <stdint.h>

#include "ecm_dc.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, long got, long want)
{
    if (got == want) { printf("  [PASS] %-60s = %ld\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-60s = %ld (expected %ld)\n", name, got, want); g_fail++; }
}

#define CYC 1000000LL
static uint64_t ref_at(uint64_t host) { return 5000000000ull + host + host / 20000; }   /* +50 ppm */


/* ---------- W: closed loop across 32-bit wraps (soak 26/9 question) ----------
 * Reference clock with the drift measured on the Jetson (-48.6 ppm), only the
 * low 32 bits visible (32-bit ESC). The master wakes at its deadline, the DC
 * sample is the reference time when the frame passes (send + fixed path).
 * Optional: one late send of spike_ns at cycle spike_at. Returns counters. */
typedef struct { long wraps, unlocks, clamps; long max_e_wrap, max_e_other, max_e_after_spike_100; } wres_t;
static wres_t run_wraps(long cycles, long spike_at, long spike_ns)
{
    const int64_t drift_ppb = -48584, path_ns = 12000;
    ecm_dc_cfg_t c; ecm_dc_t d; wres_t r = {0};
    ecm_dc_default_cfg(&c, CYC, CYC * 30 / 100); c.gate_ns = CYC;
    ecm_dc_init(&d, &c);
    int64_t host = 3000000000LL;                       /* host = master CLOCK_MONOTONIC */
    #define REF(h) (int64_t)(0x00000000F0000000LL + (h) + ((h) / 1000) * drift_ppb / 1000000)
    int64_t r0 = REF(host);
    ecm_dc_anchor(&d, (uint32_t)r0, (uint32_t)(r0 + CYC - (r0 % CYC) + 170000), host);
    int64_t deadline = host + CYC, since_wrap = 1000;
    for (long i = 0; i < cycles; i++) {
        int64_t send = deadline + (i == spike_at ? spike_ns : 0);
        uint64_t w0 = d.wraps;
        int64_t u = ecm_dc_update(&d, (uint32_t)REF(send + path_ns), (uint64_t)send);
        if (d.wraps != w0) since_wrap = 0;
        long ae = (long)(d.err_ns < 0 ? -d.err_ns : d.err_ns);
        if (i > 20000) {                               /* locked and settled */
            if (i == spike_at) { }                     /* the spike itself: expected */
            else if (spike_at >= 0 && i > spike_at && i <= spike_at + 100) { if (ae > r.max_e_after_spike_100) r.max_e_after_spike_100 = ae; }
            else if (since_wrap <= 3) { if (ae > r.max_e_wrap) r.max_e_wrap = ae; }
            else if (ae > r.max_e_other) r.max_e_other = ae;
        }
        since_wrap++;
        deadline += CYC + u;
    }
    r.wraps = (long)d.wraps; r.unlocks = (long)d.unlocks; r.clamps = (long)d.clamps;
    return r;
}

int main(void)
{
    ecm_dc_cfg_t cfg;
    ecm_dc_t dc;
    ecm_dc_default_cfg(&cfg, CYC, CYC * 30 / 100);
    check("G0 gate off by default", (long)cfg.gate_ns, 0);
    cfg.gate_ns = CYC;                          /* as ecm_run sets it */
    ecm_dc_init(&dc, &cfg);
    uint64_t host = 1000000000ull;
    uint64_t r0 = ref_at(host);
    /* SYNC0 grid: next SYNC0 1 cycle ahead of r0, sample at +30 % */
    ecm_dc_anchor(&dc, r0, r0 + CYC - (r0 % CYC), host);

    printf("G1 normal samples are accepted\n");
    for (int i = 1; i <= 200; i++) {
        host += CYC;
        ecm_dc_update(&dc, ref_at(host), host);
    }
    check("G1a no rejection in 200 normal cycles", (long)dc.implausible, 0);
    check("G1b last_rejected clear", dc.last_rejected, 0);
    uint64_t samples_before = dc.samples;
    int64_t err_before = dc.err_ns;

    printf("G2 a reply 15 cycles old (SOEM index reuse) is rejected\n");
    host += CYC;
    ecm_dc_update(&dc, ref_at(host - 15 * CYC), host);
    check("G2a rejected", dc.last_rejected, 1);
    check("G2b implausible counted", (long)dc.implausible, 1);
    check("G2c not used as a sample", (long)(dc.samples - samples_before), 0);
    host += CYC;
    ecm_dc_update(&dc, ref_at(host), host);
    check("G2d next genuine sample accepted", dc.last_rejected, 0);
    long derr = (long)(dc.err_ns - err_before); if (derr < 0) derr = -derr;
    check("G2e phase error not disturbed (< 2 us change)", derr < 2000, 1);

    printf("G3 a reply half a cycle off but inside the window is accepted\n");
    host += CYC;
    ecm_dc_update(&dc, ref_at(host) + 300000, host);
    check("G3a 300 us jitter accepted", dc.last_rejected, 0);

    host += CYC;
    ecm_dc_update(&dc, ref_at(host) + 850000, host);
    check("G3b 850 us (a reply at the very end of its receive budget) accepted", dc.last_rejected, 0);
    host += CYC;
    ecm_dc_update(&dc, ref_at(host), host);
    host += CYC;
    ecm_dc_update(&dc, ref_at(host - 5 * CYC), host);
    check("G3c 5 cycles old (the youngest a reused index can be) rejected", dc.last_rejected, 1);
    host += CYC;
    ecm_dc_update(&dc, ref_at(host), host);

    printf("G4 a real clock step: resync after %d rejections in a row\n", ECM_DC_GATE_RESYNC);
    uint64_t step = 10 * CYC;
    int rej = 0;
    for (int i = 0; i < 5; i++) {
        host += CYC;
        ecm_dc_update(&dc, ref_at(host) + step, host);
        rej += dc.last_rejected;
    }
    check("G4a rejected only the first ECM_DC_GATE_RESYNC-1", rej, ECM_DC_GATE_RESYNC - 1);
    check("G4b resync counted", (long)dc.gate_resyncs, 1);
    check("G4c then accepted again", dc.last_rejected, 0);

    printf("G5 gate off: an old sample is not rejected (l6_test behaviour)\n");
    {
        ecm_dc_cfg_t c2;
        ecm_dc_t d2;
        ecm_dc_default_cfg(&c2, CYC, CYC * 30 / 100);
        ecm_dc_init(&d2, &c2);
        uint64_t h2 = 1000000000ull, r2 = ref_at(h2);
        ecm_dc_anchor(&d2, r2, r2 + CYC - (r2 % CYC), h2);
        for (int i = 0; i < 10; i++) { h2 += CYC; ecm_dc_update(&d2, ref_at(h2), h2); }
        h2 += CYC;
        ecm_dc_update(&d2, ref_at(h2 - 15 * CYC), h2);
        check("G5a not rejected", d2.last_rejected, 0);
        check("G5b no implausible count", (long)d2.implausible, 0);
    }


    printf("W1 closed loop, 32-bit reference, -48.6 ppm, 120 000 cycles (~28 wraps)\n");
    {
        wres_t r = run_wraps(120000, -1, 0);
        check("W1a wraps counted (first at 0.27 s, then every 4.295 s)", r.wraps, 28);
        check("W1b no unlock", r.unlocks, 0);
        check("W1c |e| in the 4 samples after each wrap <= 1 us", r.max_e_wrap <= 1000, 1);
        check("W1d |e| elsewhere <= 1 us", r.max_e_other <= 1000, 1);
        printf("      max|e| after wrap %ld ns, elsewhere %ld ns\n", r.max_e_wrap, r.max_e_other);
    }
    printf("W2 same, one send 290 us late (host jitter), right after a wrap or not\n");
    for (long at = 40000; at <= 40000 + 4296; at += 4296 / 2) {
        wres_t r = run_wraps(60000, at, 290000);
        char n1[80]; snprintf(n1, sizeof n1, "W2 spike at %ld: no unlock", at);
        check(n1, r.unlocks, 0);
        snprintf(n1, sizeof n1, "W2 spike at %ld: next 100 samples |e| < 40 us", at);
        check(n1, r.max_e_after_spike_100 < 40000, 1);
        printf("      max|e| in the 100 samples after the spike: %ld ns\n", r.max_e_after_spike_100);
    }

    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
