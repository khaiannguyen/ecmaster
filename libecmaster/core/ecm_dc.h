#ifndef ECM_DC_H
#define ECM_DC_H

#include <stdint.h>

/* ==========================================================================
 * ecm_dc.h — DC(b): lock the master cycle to the DC reference clock
 * ("master shift": the master follows the bus, the bus is never touched).
 *
 * Controlled quantity: phase of the moment the cyclic frame passes the
 * reference clock, measured in reference-clock time, relative to the SYNC0
 * grid of the reference clock:
 *
 *      phase = (DCtime_unwrapped - SYNC0_start_unwrapped) mod cycle
 *
 * NOT `DCtime % cycle`. With a 32-bit DC, 2^32 is not a multiple of the
 * cycle (2^32 mod 1e6 = 967296), so `DCtime % cycle` jumps by 32.704 us at
 * every wrap (~4.295 s) while the slave's SYNC0 grid does not.
 *
 * Unwrapping uses ONLY the low 32 bits of DCtime plus the host monotonic
 * clock to resolve how many wraps elapsed -> identical code for 32- and
 * 64-bit ESCs, and robust to gaps of any length (lost frames, pauses).
 *
 * RT-safe: no malloc, no syscalls, no locks, no floating point.
 * ========================================================================== */

typedef struct {
    int64_t  cycle_ns;        /* master cycle == SYNC0 cycle of the DC group   */
    int64_t  setpoint_ns;     /* wanted phase AFTER SYNC0, in [0, cycle)       */
    int32_t  kp_div;          /* u_P = e / kp_div         (default 8)          */
    int32_t  ki_div;          /* u_I = sum(e) / ki_div    (default 256)        */
    int64_t  max_adjust_ns;   /* |u| clamp per cycle      (default cycle/20)   */
    int64_t  lock_window_ns;  /* |e| inside -> counts toward lock (5 us)       */
    uint32_t lock_samples;    /* consecutive in-window samples to lock (100)   */
    int64_t  unlock_ns;       /* |e| above this while locked -> unlock (c/4)   */
} ecm_dc_cfg_t;

typedef enum {
    ECM_DC_UNANCHORED = 0,    /* no SYNC0 anchor yet: ecm_dc_update() is a no-op */
    ECM_DC_ACQUIRE,           /* P only, integrator frozen                   */
    ECM_DC_LOCKED             /* full PI                                     */
} ecm_dc_state_t;

typedef struct {
    ecm_dc_cfg_t   cfg;
    ecm_dc_state_t state;

    /* unwrap */
    uint32_t prev_lo;
    uint64_t prev_host_ns;
    int64_t  ext_ns;          /* unwrapped reference clock time             */
    int64_t  sync0_ext_ns;    /* unwrapped SYNC0 anchor (any event on grid) */

    /* controller */
    int64_t  integ;           /* sum of e while locked                       */
    int64_t  err_ns;          /* last phase error, in (-cycle/2, cycle/2]    */
    int64_t  adjust_ns;       /* last output (add to next deadline)          */
    int64_t  win_sum;         /* sum of e in the current evaluation window   */
    uint32_t win_n;

    /* counters for telemetry (single writer: RT thread) */
    uint64_t samples, stale, wraps, clamps, locks, unlocks;
} ecm_dc_t;

void ecm_dc_default_cfg(ecm_dc_cfg_t *cfg, int64_t cycle_ns, int64_t setpoint_ns);
void ecm_dc_init(ecm_dc_t *dc, const ecm_dc_cfg_t *cfg);

/* Anchor the SYNC0 grid. dc_raw / sync0_raw: 0x0910 and 0x0990 of the
 * reference clock read IN THE SAME DATAGRAM (FPRD 0x0910, 0x88 bytes), any
 * DC width. host_ns: CLOCK_MONOTONIC at (about) the time of that read. */
void ecm_dc_anchor(ecm_dc_t *dc, uint64_t dc_raw, uint64_t sync0_raw, uint64_t host_ns);

/* One RT cycle. Call ONLY when the frame carrying the DC datagram came back
 * (WKC of the DC group > 0). dc_raw = ctx.DCtime, host_ns = t_wake of this
 * cycle (CLOCK_MONOTONIC). Returns ns to add to the next absolute deadline. */
int64_t ecm_dc_update(ecm_dc_t *dc, uint64_t dc_raw, uint64_t host_ns);

/* Drift of the reference clock relative to the master clock, estimated from
 * the integrator (ppb, positive = reference runs faster). */
int64_t ecm_dc_ref_drift_ppb(const ecm_dc_t *dc);

#endif /* ECM_DC_H */
