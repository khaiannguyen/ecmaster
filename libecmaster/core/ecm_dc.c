/* ==========================================================================
 * ecm_dc.c — DC(b) master-shift phase lock. See ecm_dc.h.
 * ========================================================================== */
#include "ecm_dc.h"

#define TWO32 ((int64_t)1 << 32)

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static int64_t pos_mod(int64_t a, int64_t b)
{
    int64_t r = a % b;
    return r < 0 ? r + b : r;
}

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

void ecm_dc_default_cfg(ecm_dc_cfg_t *cfg, int64_t cycle_ns, int64_t setpoint_ns)
{
    cfg->cycle_ns       = cycle_ns;
    cfg->setpoint_ns    = pos_mod(setpoint_ns, cycle_ns);
    /* kp=1/8, ki=1/256: characteristic eq. z^2 + (kp+ki-2) z + (1-kp) = 0
     * -> real poles ~0.952 / 0.920, near-critically damped, tau ~20 cycles.
     * Steady state under constant drift d: e -> 0, integ -> d * ki_div. */
    cfg->kp_div         = 8;
    cfg->ki_div         = 256;
    cfg->max_adjust_ns  = cycle_ns / 20;
    cfg->lock_samples   = 100;
    cfg->lock_window_ns = cycle_ns / 100;
    cfg->unlock_ns      = cycle_ns / 20;
}

void ecm_dc_init(ecm_dc_t *dc, const ecm_dc_cfg_t *cfg)
{
    ecm_dc_t z = {0};
    *dc = z;
    dc->cfg   = *cfg;
    dc->state = ECM_DC_UNANCHORED;
}

void ecm_dc_anchor(ecm_dc_t *dc, uint64_t dc_raw, uint64_t sync0_raw, uint64_t host_ns)
{
    uint32_t lo    = (uint32_t)dc_raw;
    uint32_t s0_lo = (uint32_t)sync0_raw;

    /* Domain of ext_ns: aligned with the low 32 bits of DCtime, so that
     * (ext_ns >> 32) counts wraps directly. */
    dc->ext_ns       = (int64_t)lo;
    dc->sync0_ext_ns = dc->ext_ns + (int32_t)(s0_lo - lo);   /* |diff| < 2^31 ns */
    dc->prev_lo      = lo;
    dc->prev_host_ns = host_ns;
    dc->win_sum      = 0;
    dc->win_n        = 0;
    dc->state        = ECM_DC_ACQUIRE;
}

/* Advance ext_ns to the new sample. The host clock tells us roughly how much
 * reference time elapsed (drift << 2^31 ns over any realistic gap), which
 * picks the right number of 2^32 wraps. */
static void unwrap(ecm_dc_t *dc, uint32_t lo, uint64_t host_ns)
{
    int64_t pred  = (int64_t)(host_ns - dc->prev_host_ns);
    int64_t d_lo  = (int64_t)(uint32_t)(lo - dc->prev_lo);        /* 0 .. 2^32-1 */
    int64_t k     = floor_div(pred - d_lo + TWO32 / 2, TWO32);
    int64_t delta = d_lo + k * TWO32;
    int64_t old   = dc->ext_ns;

    dc->ext_ns       = old + delta;
    dc->wraps       += (uint64_t)((dc->ext_ns >> 32) - (old >> 32));
    dc->prev_lo      = lo;
    dc->prev_host_ns = host_ns;
}

int64_t ecm_dc_update(ecm_dc_t *dc, uint64_t dc_raw, uint64_t host_ns)
{
    if (dc->state == ECM_DC_UNANCHORED) return 0;

    uint32_t lo = (uint32_t)dc_raw;
    if (lo == dc->prev_lo) {            /* DC datagram did not come back */
        dc->stale++;
        dc->adjust_ns = 0;
        return 0;
    }
    unwrap(dc, lo, host_ns);
    dc->samples++;

    const ecm_dc_cfg_t *c = &dc->cfg;
    int64_t ph = pos_mod(dc->ext_ns - dc->sync0_ext_ns, c->cycle_ns);
    int64_t e  = ph - c->setpoint_ns;
    if (e >  c->cycle_ns / 2)  e -= c->cycle_ns;
    if (e <= -c->cycle_ns / 2) e += c->cycle_ns;
    dc->err_ns = e;

    if (dc->state == ECM_DC_LOCKED) dc->integ += e;

    /* e > 0: frame reaches the reference clock later than wanted
     *        -> wake earlier -> negative adjustment */
    int64_t u = -(e / c->kp_div + dc->integ / c->ki_div);
    if (u > c->max_adjust_ns || u < -c->max_adjust_ns) {
        u = (u > 0) ? c->max_adjust_ns : -c->max_adjust_ns;
        dc->clamps++;
        if (dc->state == ECM_DC_LOCKED) dc->integ -= e;   /* anti-windup */
    }

    /* lock supervision on the window mean */
    dc->win_sum += e;
    if (++dc->win_n >= c->lock_samples) {
        int64_t mean = iabs64(dc->win_sum / (int64_t)dc->win_n);
        if (dc->state == ECM_DC_ACQUIRE && mean < c->lock_window_ns) {
            dc->state = ECM_DC_LOCKED; dc->locks++;
        } else if (dc->state == ECM_DC_LOCKED && mean > c->unlock_ns) {
            dc->state = ECM_DC_ACQUIRE; dc->unlocks++;   /* integ kept: drift estimate */
        }
        dc->win_sum = 0; dc->win_n = 0;
    }
    dc->adjust_ns = u;
    return u;
}

int64_t ecm_dc_ref_drift_ppb(const ecm_dc_t *dc)
{
    return dc->integ * 1000000000LL / ((int64_t)dc->cfg.ki_div * dc->cfg.cycle_ns);
}
