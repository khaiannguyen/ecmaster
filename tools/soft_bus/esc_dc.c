/* ==========================================================================
 * esc_dc.c — Distributed Clock model for soft_bus. See esc_dc.h.
 * ========================================================================== */
#include <string.h>
#include <time.h>
#include "esc_dc.h"

/* ---- DC registers (Section II §2.15) ---- */
#define DC_RECV_PORT0     0x0900
#define DC_RECV_PORT1     0x0904
#define DC_RECV_PORT2     0x0908
#define DC_RECV_PORT3     0x090C
#define DC_SYSTIME        0x0910
#define DC_RECV_EPU       0x0918   /* receive time ECAT processing unit (SOF) */
#define DC_SYSOFFSET      0x0920
#define DC_SYSDELAY       0x0928
#define DC_SYSDIFF        0x092C
#define DC_SPEEDSTART     0x0930
#define DC_CUC            0x0980
#define DC_ACTIVATION     0x0981
#define DC_START0         0x0990
#define DC_CYCLE0         0x09A0

#define FEAT_DC_AVAILABLE 0x0004   /* 0x0008 bit2 */
#define FEAT_DC_64BIT     0x0008   /* 0x0008 bit3 */

/* Time control loop (Section I §9.1.3.3 + Section II §2.15.2.4-8): the ESC
 * averages dt (0x0934 "system time difference filter depth", default 4 ->
 * EMA 1/16) and ADJUSTS THE SPEED of the local clock only -- no phase
 * steps. Rate = I - Kp*mean(dt), I -= mean(dt)/Ki_div per write.
 * Gains picked by simulation (1 kHz writes, +-70 ppm static drift):
 * 70 ppm step -> worst 7.7 us, settled < 50 ns in ~0.6 s; 10 us rms feed
 * jitter -> ~2 us phase noise; robust to 1 % of 300 us outliers.
 * (Phase 6, L6-05: the first model -- phase step dt/4 per write + raw dt
 * integral -- ran away as soon as slaves were fed by the master's
 * prediction instead of by the reference clock: every jitter outlier was
 * applied unfiltered. Real ESCs filter; this model now does too.) */
#define TCL_FILT_SHIFT    4        /* EMA 1/16 = filter depth 4            */
#define TCL_KP_PPB_PER_NS 5
#define TCL_KI_DIV        32
#define TCL_TRIM_MAX_PPB  500000   /* +-500 ppm                             */

#define ESC_DC_NOFRAME_LOG_MAX 200

static esc_dc_cfg_t g_cfg;
static int          g_n;

/* Chain-level sync ground truth, per report window, sampled on every PD
 * frame once SYNC0 runs on node 0. The reference is detected, not assumed:
 * it is the node whose 0x0910 the PD frame READ (the FRMW target).
 *   spread = max - min of the system times of the ref and every node
 *            DOWNSTREAM of it, at one common instant (these are disciplined
 *            by the reference's own time, in-frame)
 *   up     = max |system time - ref's| over nodes UPSTREAM of the ref
 *            (they never see the ref's time in-frame; only the master can
 *            feed them, so this is bounded by master jitter, not by DC) */
static int64_t  g_sp_max = INT64_MIN, g_up_max = 0;
static int64_t  g_sp_sum;
static uint64_t g_sp_n;
static int      g_ref_idx = -1, g_ref_changes = 0;

/* ---------- little-endian helpers ---------- */
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p)
{ return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static int overlaps(uint16_t off, uint16_t len, uint16_t reg, uint16_t reg_len)
{
    return (uint32_t)off < (uint32_t)reg + reg_len && (uint32_t)off + len > reg;
}

static int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static int64_t pos_mod(int64_t a, int64_t b)   { int64_t r = a % b; return r < 0 ? r + b : r; }

/* ---------- register views ---------- */
static int64_t offset_internal(const esc_t *esc)
{
    if (g_cfg.width == 64) return (int64_t)rd64(esc->regs + DC_SYSOFFSET);
    return (int64_t)(uint64_t)rd32(esc->regs + DC_SYSOFFSET); /* 32-bit ESC */
}

int64_t esc_dc_frame_sys(const esc_t *esc)
{
    return esc->dc.frame_local_ns + offset_internal(esc);
}

/* Write a continuous time into an 8-byte register slot using the node's DC
 * width: 32-bit ESCs expose only the low 32 bits, upper bytes read as 0. */
static void wr_time_view(uint8_t *p, int64_t t)
{
    if (g_cfg.width == 64) wr64(p, (uint64_t)t);
    else { wr32(p, (uint32_t)t); wr32(p + 4, 0); }
}

/* ========================================================================== */
void esc_dc_setup(esc_t *chain, int n, const esc_dc_cfg_t *cfg)
{
    g_cfg = *cfg;
    g_n   = n;
    if (cfg->width != 32 && cfg->width != 64) { g_cfg.width = 0; return; }

    for (int i = 0; i < n; i++) esc_dc_node_reset(&chain[i], i);
}

/* Power-on DC state of one node (also used by restore_node, Phase 7). */
void esc_dc_node_reset(esc_t *e, int i)
{
    if (!g_cfg.width) return;
    {
        memset(&e->dc, 0, sizeof(e->dc));

        uint16_t feat = (uint16_t)(e->regs[0x0008] | (e->regs[0x0009] << 8));
        feat |= FEAT_DC_AVAILABLE;
        if (g_cfg.width == 64) feat |= FEAT_DC_64BIT;
        e->regs[0x0008] = (uint8_t)feat;
        e->regs[0x0009] = (uint8_t)(feat >> 8);

        /* Node 0 = reference clock candidate. Others spread -A..+A so the
         * time control loop of every node has real work to do. */
        if (i == 0) e->dc.drift_ppb = g_cfg.ref_drift_ppb;
        else        e->dc.drift_ppb = ((int64_t)(i % 5) - 2) * g_cfg.other_drift_ppb / 2;

        /* Arbitrary, distinct power-on local times -> offset compensation
         * has something real to compensate. */
        e->dc.local_ns = (int64_t)(i + 1) * 987654321LL + (int64_t)i * 123457LL;
        e->dc.w_phase_min = INT64_MAX;
        e->dc.w_off0_min  = INT64_MAX;
        e->dc.w_off0_max  = INT64_MIN;
        e->dc.w_phase_max = INT64_MIN;
    }
}

static void clock_advance(esc_dc_state_t *d, uint64_t host_ns)
{
    if (!d->clock_started) { d->clock_started = 1; d->last_host_ns = host_ns; return; }
    if (host_ns <= d->last_host_ns) return;
    uint64_t dt = host_ns - d->last_host_ns;
    __int128 num = (__int128)dt * (1000000000LL + d->drift_ppb + d->trim_ppb) + d->frac_e9;
    d->local_ns += (int64_t)(num / 1000000000LL);
    d->frac_e9   = (int64_t)(num % 1000000000LL);
    d->last_host_ns = host_ns;
}

void esc_dc_frame_begin(esc_t *chain, int n, uint64_t t_host_ns)
{
    if (!g_cfg.width) return;
    for (int i = 0; i < n; i++) {
        esc_dc_state_t *d = &chain[i].dc;
        clock_advance(d, t_host_ns);
        d->frame_local_ns = d->local_ns + (int64_t)i * g_cfg.hop_ns;
    }
}

/* ---------- SYNC0 accounting (ground truth) ---------- */
/* System time of node i minus node r's at the same instant. frame_sys()
 * of node i is taken when the frame reaches it, i*hop after node 0. */
static int64_t off_to(const esc_t *chain, int i, int r)
{
    int64_t o = esc_dc_frame_sys(&chain[i]) - (int64_t)i * g_cfg.hop_ns
              - (esc_dc_frame_sys(&chain[r]) - (int64_t)r * g_cfg.hop_ns);
    return g_cfg.width == 32 ? (int64_t)(int32_t)(uint32_t)o : o;   /* compare mod 2^32 */
}

void esc_dc_frame_end(esc_t *chain, int n, int has_pd)
{
    if (!g_cfg.width) return;

    int ref = -1;
    for (int i = 0; i < n; i++) {
        if (chain[i].dc.read_systime && ref < 0) ref = i;
        chain[i].dc.read_systime = 0;
    }
    if (has_pd && ref >= 0 && chain[0].dc.sync0_active) {
        if (ref != g_ref_idx) { if (g_ref_idx >= 0) g_ref_changes++; g_ref_idx = ref; }
        int64_t mn = 0, mx = 0;                  /* the ref itself: offset 0 */
        for (int i = 0; i < n; i++) {
            int64_t o = off_to(chain, i, ref);
            esc_dc_state_t *d = &chain[i].dc;
            if (o < d->w_off0_min) d->w_off0_min = o;
            if (o > d->w_off0_max) d->w_off0_max = o;
            if (i >= ref) {
                if (o < mn) mn = o;
                if (o > mx) mx = o;
            } else {
                int64_t a = o < 0 ? -o : o;
                if (a > g_up_max) g_up_max = a;
            }
        }
        if (mx - mn > g_sp_max) g_sp_max = mx - mn;
        g_sp_sum += mx - mn;
        g_sp_n++;
    }
    for (int i = 0; i < n; i++) {
        esc_dc_state_t *d = &chain[i].dc;
        if (!d->sync0_active || d->sync0_cycle <= 0) continue;

        int64_t sys = esc_dc_frame_sys(&chain[i]);
        if (sys >= d->sync0_start) {
            int64_t k = (sys - d->sync0_start) / d->sync0_cycle;
            if (k > d->sync0_k_last) {
                int64_t new_ev = k - d->sync0_k_last;
                /* every elapsed interval must have held >= 1 PD frame */
                uint64_t nofr = (uint64_t)((d->pd_since_event ? 0 : 1) + (new_ev - 1));
                d->w_events += (uint64_t)new_ev;  d->tot_events += (uint64_t)new_ev;
                d->w_no_frame += nofr;            d->tot_no_frame += nofr;

                /* NOFRAME diagnostics (node 0 only, capped). new_events
                 * SYNC0 edges fell between two consecutive PD frames:
                 *   gap_us >= ~2 cycles       -> frames really absent (stall
                 *                                of master, soft_bus or wire)
                 *   gap_us ~ 1 cycle and
                 *   phase_now_us near 0       -> no loss: arrival phase is
                 *                                straddling a SYNC0 edge
                 *                                (phase error, e.g. the naive
                 *                                controller's wrap drift)
                 * The wall-clock stamp lines this up with the master log. */
                if (nofr && i == 0 && d->dbg_nofr_logged < ESC_DC_NOFRAME_LOG_MAX) {
                    struct timespec wall;
                    struct tm tmv;
                    clock_gettime(CLOCK_REALTIME, &wall);
                    localtime_r(&wall.tv_sec, &tmv);
                    fprintf(stderr,
                        "NOFRAME %02d:%02d:%02d.%06ld node0 k=%lld new_events=%lld pd_since=%d "
                        "gap_since_last_pd_us=%.1f phase_now_us=%.1f has_pd=%d\n",
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, wall.tv_nsec / 1000,
                        (long long)k, (long long)new_ev, d->pd_since_event,
                        d->dbg_last_pd_host ? (double)(d->last_host_ns - d->dbg_last_pd_host) / 1000.0 : -1.0,
                        (double)pos_mod(sys - d->sync0_start, d->sync0_cycle) / 1000.0, has_pd);
                    if (++d->dbg_nofr_logged == ESC_DC_NOFRAME_LOG_MAX)
                        fprintf(stderr, "NOFRAME log cap (%d) reached, further events only counted\n",
                                ESC_DC_NOFRAME_LOG_MAX);
                }
                d->sync0_k_last = k;
                d->pd_since_event = 0;
            }
        }
        if (has_pd) {
            d->dbg_last_pd_host = d->last_host_ns;
            d->pd_since_event = 1;
            d->w_pd++; d->tot_pd++;
            if (sys >= d->sync0_start) {
                int64_t ph = pos_mod(sys - d->sync0_start, d->sync0_cycle);
                if (ph < d->w_phase_min) d->w_phase_min = ph;
                if (ph > d->w_phase_max) d->w_phase_max = ph;
                d->w_phase_sum += ph;
            }
        }
    }
}

/* ---------- read side ---------- */
void esc_dc_before_read(esc_t *esc, uint16_t off, uint16_t len)
{
    if (!g_cfg.width) return;
    esc_dc_state_t *d = &esc->dc;

    if (overlaps(off, len, DC_SYSTIME, 8)) {
        wr_time_view(esc->regs + DC_SYSTIME, esc_dc_frame_sys(esc));
        d->read_systime = 1;
    }

    if (overlaps(off, len, DC_SYSDIFF, 4)) {
        int64_t f = d->dt_filt_x16 / 16;              /* local - received */
        uint32_t mag = (uint32_t)((f < 0 ? -f : f) & 0x7FFFFFFF);
        uint32_t v = mag | (f >= 0 ? 0x80000000u : 0u); /* bit31: local >= received */
        wr32(esc->regs + DC_SYSDIFF, v);
    }

    /* 0x0990 read while cyclic operation is active = NEXT SYNC0 pulse */
    if (d->sync0_active && d->sync0_cycle > 0 && overlaps(off, len, DC_START0, 8)) {
        int64_t sys = esc_dc_frame_sys(esc);
        int64_t next = d->sync0_start;
        if (sys >= d->sync0_start)
            next = d->sync0_start + (floor_div(sys - d->sync0_start, d->sync0_cycle) + 1) * d->sync0_cycle;
        wr_time_view(esc->regs + DC_START0, next);
    }
}

/* ---------- write side ---------- */
static void latch_receive_times(esc_t *esc)
{
    esc_dc_state_t *d = &esc->dc;
    int pos = esc->position_in_chain;
    int64_t t0 = d->frame_local_ns;

    wr32(esc->regs + DC_RECV_PORT0, (uint32_t)t0);
    if (pos < g_n - 1) {
        /* returning frame re-enters port1 after travelling to the end of the
         * chain and back: 2 * hop per downstream node */
        int64_t t1 = t0 + 2 * g_cfg.hop_ns * (int64_t)(g_n - 1 - pos);
        wr32(esc->regs + DC_RECV_PORT1, (uint32_t)t1);
    } else {
        wr32(esc->regs + DC_RECV_PORT1, 0);
    }
    wr32(esc->regs + DC_RECV_PORT2, 0);
    wr32(esc->regs + DC_RECV_PORT3, 0);
    wr_time_view(esc->regs + DC_RECV_EPU, t0);
}

static void time_control_loop(esc_t *esc, uint32_t received_lo)
{
    esc_dc_state_t *d = &esc->dc;
    uint32_t delay = rd32(esc->regs + DC_SYSDELAY);
    /* Section I §9.1.3.3: dt = (local + offset - delay) - received, low 32 bit */
    int32_t dt = (int32_t)((uint32_t)esc_dc_frame_sys(esc) - delay - received_lo);

    d->last_dt_ns = dt;
    d->dt_writes++;
    d->dt_filt_x16 += (int64_t)dt - d->dt_filt_x16 / (1 << TCL_FILT_SHIFT);   /* x16 fixed point */

    int64_t e = d->dt_filt_x16 / 16;                   /* mean dt, ns */
    /* integral kept x16 as well: e/TCL_KI_DIV in plain ns would leave a
     * dead band of |e| < TCL_KI_DIV ns where the integrator never moves */
    d->tcl_i_x16 -= d->dt_filt_x16 / TCL_KI_DIV;
    if (d->tcl_i_x16 >  TCL_TRIM_MAX_PPB * 16) d->tcl_i_x16 =  TCL_TRIM_MAX_PPB * 16;
    if (d->tcl_i_x16 < -TCL_TRIM_MAX_PPB * 16) d->tcl_i_x16 = -TCL_TRIM_MAX_PPB * 16;
    d->trim_ppb = d->tcl_i_x16 / 16 - e * TCL_KP_PPB_PER_NS;
    if (d->trim_ppb >  TCL_TRIM_MAX_PPB) d->trim_ppb =  TCL_TRIM_MAX_PPB;
    if (d->trim_ppb < -TCL_TRIM_MAX_PPB) d->trim_ppb = -TCL_TRIM_MAX_PPB;
}

static void sync0_activation(esc_t *esc)
{
    esc_dc_state_t *d = &esc->dc;
    uint8_t act = esc->regs[DC_ACTIVATION];

    if ((act & 0x03) != 0x03) { d->sync0_active = 0; return; }

    int64_t now   = esc_dc_frame_sys(esc);
    int64_t cycle = (int64_t)rd32(esc->regs + DC_CYCLE0);
    int64_t start;

    if (g_cfg.width == 64) {
        start = (int64_t)rd64(esc->regs + DC_START0);
        if (start <= now) {                /* 64-bit: never reached */
            d->start_in_past++;
            d->sync0_active = 0;
            return;
        }
    } else {
        uint32_t lo = rd32(esc->regs + DC_START0);
        start = now + (int32_t)(lo - (uint32_t)now);
        if (start <= now) {                /* 32-bit: fires after the wrap */
            d->start_in_past++;
            start += (int64_t)1 << 32;
        }
    }
    d->sync0_start    = start;
    d->sync0_cycle    = cycle;
    d->sync0_k_last   = -1;
    d->pd_since_event = 1;
    d->sync0_active   = 1;
}

void esc_dc_after_write(esc_t *esc, uint16_t off, const uint8_t *data, uint16_t len)
{
    if (!g_cfg.width) return;

    /* write to 0x0900 (at least first byte) latches receive times */
    if (overlaps(off, len, DC_RECV_PORT0, 1))
        latch_receive_times(esc);

    /* write to 0x0910 (at least first byte): compare, NOT store */
    if (overlaps(off, len, DC_SYSTIME, 1) && off <= DC_SYSTIME && off + len >= DC_SYSTIME + 4) {
        time_control_loop(esc, rd32(data + (DC_SYSTIME - off)));
    }
    if (overlaps(off, len, DC_SYSTIME, 8))
        wr_time_view(esc->regs + DC_SYSTIME, esc_dc_frame_sys(esc)); /* undo memcpy */

    if (g_cfg.width == 32 && overlaps(off, len, DC_SYSOFFSET + 4, 4))
        wr32(esc->regs + DC_SYSOFFSET + 4, 0);

    /* "Writing speed counter start resets the internal filters" (§9.1.3.2).
     * The rate correction MUST go too: SOEM's ecx_config_init() does
     * BWR 0x0910 = 0 (-> huge dt, trim slams into its clamp) immediately
     * followed by BWR 0x0930. Resetting only dt_filt left the reference
     * clock running at -450 ppm forever (found on the first real SOEM run). */
    if (overlaps(off, len, DC_SPEEDSTART, 2)) {
        esc->dc.dt_filt_x16 = 0;
        esc->dc.trim_ppb    = 0;
        esc->dc.tcl_i_x16   = 0;
        esc->dc.last_dt_ns  = 0;
    }

    if (overlaps(off, len, DC_ACTIVATION, 1))
        sync0_activation(esc);
}

/* ========================================================================== */
int esc_dc_frame_has_pd(const uint8_t *buf, size_t len)
{
    const size_t eth = 14, ech = 2, dgh = 10, wkc = 2;
    if (len < eth + ech) return 0;
    uint16_t ec_len = (uint16_t)((buf[eth] | (buf[eth + 1] << 8)) & 0x07FF);
    size_t off = eth + ech, end = off + ec_len;
    if (end > len) end = len;
    while (off + dgh + wkc <= end) {
        uint8_t  cmd  = buf[off];
        uint16_t dlen = (uint16_t)((buf[off + 6] | (buf[off + 7] << 8)) & 0x07FF);
        if (cmd == 0x0A || cmd == 0x0B || cmd == 0x0C) return 1;
        off += dgh + dlen + wkc;
    }
    return 0;
}

void esc_dc_report(esc_t *chain, int n, FILE *out, double window_s)
{
    if (!g_cfg.width) return;
    if (g_sp_n > 0) {
        fprintf(out, "dcsync win=%.1fs ref=node%d ref_changes=%d spread_ns_max=%lld spread_ns_mean=%.1f "
                "up_ns_max=%lld off_ref_ns(min/max):",
                window_s, g_ref_idx, g_ref_changes, (long long)g_sp_max, (double)g_sp_sum / (double)g_sp_n,
                (long long)g_up_max);
        for (int i = 0; i < n; i++)
            fprintf(out, " %lld/%lld", (long long)chain[i].dc.w_off0_min, (long long)chain[i].dc.w_off0_max);
        fputc('\n', out);
    }
    g_sp_max = INT64_MIN; g_sp_sum = 0; g_sp_n = 0; g_up_max = 0;
    for (int i = 0; i < n; i++) {
        chain[i].dc.w_off0_min = INT64_MAX;
        chain[i].dc.w_off0_max = INT64_MIN;
    }
    for (int i = 0; i < n; i++) {
        esc_dc_state_t *d = &chain[i].dc;
        if (d->sync0_active && d->w_pd > 0) {
            fprintf(out,
                "dc[%d] win=%.1fs sync0=%llu no_frame=%llu pd=%llu "
                "phase_us min=%.1f mean=%.1f max=%.1f dt_ns=%d trim_ppb=%lld\n",
                i, window_s,
                (unsigned long long)d->w_events, (unsigned long long)d->w_no_frame,
                (unsigned long long)d->w_pd,
                d->w_phase_min / 1000.0, (double)d->w_phase_sum / (double)d->w_pd / 1000.0,
                d->w_phase_max / 1000.0, d->last_dt_ns, (long long)d->trim_ppb);
        }
        d->w_events = d->w_no_frame = d->w_pd = 0;
        d->w_phase_sum = 0;
        d->w_phase_min = INT64_MAX;
        d->w_phase_max = INT64_MIN;
    }
}

void esc_dc_report_totals(esc_t *chain, int n, FILE *out)
{
    if (!g_cfg.width) return;
    for (int i = 0; i < n; i++) {
        esc_dc_state_t *d = &chain[i].dc;
        fprintf(out,
            "dc_total[%d] sync0_active=%d start_lo=0x%08x sync0=%llu no_frame=%llu pd=%llu "
            "start_in_past=%llu dt_writes=%llu last_dt_ns=%d trim_ppb=%lld drift_ppb=%lld\n",
            i, d->sync0_active,
            (unsigned)(uint32_t)d->sync0_start,
            (unsigned long long)d->tot_events, (unsigned long long)d->tot_no_frame,
            (unsigned long long)d->tot_pd, (unsigned long long)d->start_in_past,
            (unsigned long long)d->dt_writes, d->last_dt_ns,
            (long long)d->trim_ppb, (long long)d->drift_ppb);
    }
}
