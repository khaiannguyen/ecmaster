#ifndef ESC_DC_STATE_H
#define ESC_DC_STATE_H

#include <stdint.h>

/* ==========================================================================
 * esc_dc_state.h — per-node Distributed Clock state, embedded in esc_t as
 * `esc_dc_state_t dc;`. Kept in its own header (no dependency on esc_t) so
 * esc_types.h can include it without a circular include.
 *
 * Internal time is ALWAYS kept as a continuous 64-bit value, even when the
 * node is configured as a 32-bit DC. The 32-bit behaviour exists only in the
 * REGISTER VIEW (what the master reads/writes over the wire). That split is
 * what makes the 32-bit wrap testable: the node itself never "jumps", only
 * the master's view of it wraps every 2^32 ns (~4.295 s).
 * ========================================================================== */
typedef struct {
    /* ---- clock model ---- */
    int      clock_started;
    uint64_t last_host_ns;      /* host CLOCK_MONOTONIC_RAW at last advance */
    int64_t  local_ns;          /* local time (continuous, 64-bit)          */
    int64_t  frac_e9;           /* sub-ns remainder of rate integration      */
    int64_t  drift_ppb;         /* simulated crystal error of this node      */
    int64_t  trim_ppb;          /* time control loop rate correction         */
    int64_t  tcl_i_x16;         /* integral part of trim_ppb, x16 fixed pt   */
    int64_t  frame_local_ns;    /* local time when current frame hit (SOF)   */

    /* ---- time control loop (0x0910 write -> 0x092C) ---- */
    int32_t  last_dt_ns;        /* last raw delta-t (local - received)       */
    int64_t  dt_filt_x16;       /* EMA of delta-t, x16 fixed point           */
    uint64_t dt_writes;

    /* ---- SYNC0 ---- */
    int      sync0_active;
    int64_t  sync0_start;       /* continuous system time of first event     */
    int64_t  sync0_cycle;
    int64_t  sync0_k_last;      /* index of last event already accounted     */
    int      pd_since_event;    /* a process-data frame arrived since the
                                 * last SYNC0 event                          */
    uint64_t start_in_past;     /* activation with start time already passed */

    /* ---- statistics: window (reset on each report) + totals ---- */
    uint64_t dbg_last_pd_host;  /* host time of last PD frame (NOFRAME log) */
    uint64_t dbg_nofr_logged;   /* NOFRAME lines printed so far (capped)      */
    int      read_systime;      /* 0x0910 read by the current frame (-> ref) */
    int64_t  w_off0_min, w_off0_max; /* this node's system time minus the
                                      * reference clock's, same instant */
    uint64_t w_events, w_no_frame, w_pd;
    int64_t  w_phase_min, w_phase_max, w_phase_sum;
    uint64_t tot_events, tot_no_frame, tot_pd;
} esc_dc_state_t;

#endif /* ESC_DC_STATE_H */
