/* ==========================================================================
 * test_ecm_dc_unit.c — fast unit tests for ecm_dc (no soft_bus involved).
 * Build: gcc -std=gnu11 -O2 -Wall -I../../libecmaster/core -o test_ecm_dc_unit \
 *            test_ecm_dc_unit.c ../../libecmaster/core/ecm_dc.c
 * ========================================================================== */
#include <stdio.h>
#include <stdint.h>
#include "ecm_dc.h"

#define C 1000000LL
static int fails = 0;
#define CHECK(cond, ...) do { if (cond) printf("  ok   "); else { printf("  FAIL "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* ideal reference clock, exposed as 32-bit or 64-bit DCtime */
static uint64_t view(int64_t t, int width) { return width == 64 ? (uint64_t)t : (uint64_t)(uint32_t)t; }

static void t_phase_and_wrap(int width)
{
    ecm_dc_cfg_t cfg; ecm_dc_t dc;
    ecm_dc_default_cfg(&cfg, C, 300000);
    ecm_dc_init(&dc, &cfg);

    int64_t sync0 = 800000000000123000LL / C * C + 7 * C;   /* on grid, "since 2000" */
    int64_t ref   = sync0 - 50 * C + 300000;                /* frame at +300 us phase */
    uint64_t host = 1000000000ull;
    ecm_dc_anchor(&dc, view(ref, width), view(sync0, width), host);

    int64_t emax = 0;
    for (int k = 0; k < 20000; k++) {                       /* 20 s -> >= 4 wraps */
        ref += C; host += C;
        ecm_dc_update(&dc, view(ref, width), host);
        int64_t ae = dc.err_ns < 0 ? -dc.err_ns : dc.err_ns;
        if (ae > emax) emax = ae;
    }
    CHECK(emax == 0, "[%d-bit] 20 s on-setpoint input -> max|e| = %lld ns (want 0)", width, (long long)emax);
    CHECK(dc.wraps >= 4, "[%d-bit] wraps counted = %llu (want >= 4)", width, (unsigned long long)dc.wraps);

    /* 10.5 s gap (> two wraps) with host clock advancing: unwrap must pick
     * the right number of wraps from the host prediction */
    ref += 10500 * C; host += 10500 * C;
    ecm_dc_update(&dc, view(ref, width), host);
    CHECK(dc.err_ns == 0, "[%d-bit] after 10.5 s gap -> e = %lld ns (want 0)", width, (long long)dc.err_ns);

    /* stale sample (same DCtime twice) must be ignored, not treated as 0 elapsed */
    uint64_t st = dc.stale;
    host += C;
    int64_t u = ecm_dc_update(&dc, view(ref, width), host);
    CHECK(dc.stale == st + 1 && u == 0, "[%d-bit] stale sample ignored", width);

    /* phase error sign + normalisation near +-cycle/2 */
    ref += C + 20000; host += C;                            /* 20 us late */
    ecm_dc_update(&dc, view(ref, width), host);
    CHECK(dc.err_ns == 20000 && dc.adjust_ns < 0, "[%d-bit] late frame -> e=+20 us, u<0 (u=%lld)",
          width, (long long)dc.adjust_ns);
    ref += C - 20000 + 499000; host += C;                   /* phase 799 us -> e = -1 us... */
    ecm_dc_update(&dc, view(ref, width), host);
    CHECK(dc.err_ns == 499000, "[%d-bit] phase 799 us -> e=+499 us (got %lld)", width, (long long)dc.err_ns);
    ref += C + 2000; host += C;                             /* phase 801 us -> wraps to -499 us */
    ecm_dc_update(&dc, view(ref, width), host);
    CHECK(dc.err_ns == -499000, "[%d-bit] phase 801 us -> e=-499 us (got %lld)", width, (long long)dc.err_ns);
}

static void t_naive_step_size(void)
{
    /* documents the number that the naive `DCtime % cycle` gets wrong */
    int64_t step = ((int64_t)1 << 32) % C;
    CHECK(step == 967296, "2^32 mod 1e6 = %lld -> naive phase jumps %lld ns per wrap",
          (long long)step, (long long)(C - step));
}

int main(void)
{
    t_phase_and_wrap(64);
    t_phase_and_wrap(32);
    t_naive_step_size();
    printf("RESULT: %d fail\n", fails);
    return fails ? 1 : 0;
}
