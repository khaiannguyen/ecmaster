/* ecm_cia402_axis.c — Phase 10.5, see ecm_cia402_axis.h */
#include "ecm_cia402_axis.h"

#include <string.h>

/* ---- names --------------------------------------------------------------- */

static const char *const DS_STR[ECM_DS_COUNT] = {
    "unknown", "Not ready to switch on", "Switch on disabled", "Ready to switch on",
    "Switched on", "Operation enabled", "Quick stop active", "Fault reaction active", "Fault"
};

const char *ecm_cia402_ds_str(ecm_ds_t ds) { return (unsigned)ds < ECM_DS_COUNT ? DS_STR[ds] : "?"; }

const char *ecm_cia402_err_str(int err)
{
    switch (err) {
    case ECM_AXERR_NONE:     return "none";
    case ECM_AXERR_TIMEOUT:  return "transition timeout";
    case ECM_AXERR_FAULT:    return "drive fault";
    case ECM_AXERR_BUS_LOST: return "bus lost";
    case ECM_AXERR_MODE:     return "mode";
    case ECM_AXERR_IN_FAULT: return "enable refused: drive in fault";
    case ECM_AXERR_DROPPED:  return "left Operation enabled without a command";
    case ECM_AXERR_STEP:     return "setpoint step above the limit";
    case ECM_AXERR_SHUTDOWN: return "enable refused: shutting down";
    case ECM_AXERR_HANDSHAKE: return "PP set-point not acknowledged";
    case ECM_AXERR_HOMING:   return "homing failed";
    case ECM_AXERR_CONFIG:   return "slave refused its configuration";
    case ECM_AXERR_SLAVE_FAILED: return "slave recovery failed";
    default:                 return "?";
    }
}

const char *ecm_cia402_mode_str(int m)
{
    switch (m) {
    case ECM_OPMODE_PP: return "PP"; case ECM_OPMODE_PV: return "PV"; case ECM_OPMODE_HM: return "HM";
    case ECM_OPMODE_CSP: return "CSP"; case ECM_OPMODE_CSV: return "CSV"; case ECM_OPMODE_CST: return "CST";
    default: return "?";
    }
}

ecm_ds_t ecm_cia402_decode(uint16_t sw)
{
    switch (sw & 0x4F) {                   /* CiA 402 statusword masks */
    case 0x00: return ECM_DS_NOT_READY;
    case 0x40: return ECM_DS_SOD;
    case 0x0F: return ECM_DS_FRA;
    case 0x08: return ECM_DS_FAULT;
    default: break;
    }
    switch (sw & 0x6F) {
    case 0x21: return ECM_DS_RTSO;
    case 0x23: return ECM_DS_SO;
    case 0x27: return ECM_DS_OE;
    case 0x07: return ECM_DS_QSA;
    default:   return ECM_DS_UNKNOWN;
    }
}

/* ---- setup --------------------------------------------------------------- */

void ecm_cia402_init(ecm_cia402_t *c, uint64_t cycle_ns, uint32_t step_timeout_ms)
{
    memset(c, 0, sizeof(*c));
    c->cycle_ns = cycle_ns ? cycle_ns : 1000000;
    c->step_timeout_ticks = (uint32_t)(((uint64_t)step_timeout_ms * 1000000ull + c->cycle_ns - 1) / c->cycle_ns);
    if (!c->step_timeout_ticks) c->step_timeout_ticks = 1;
    ecm_cia402_set_home_timeout(c, 30000);
    ecm_xring_init(&c->cmd, ECM_XCHG_CMD_CAP);
    for (int i = 0; i < ECM_AXIS_MAX; i++) ecm_xring_init(&c->sp[i], ECM_XRING_CAP);
}

int ecm_cia402_add_axis(ecm_cia402_t *c, const ecm_axis_cfg_t *cfg, const ecm_axis_bind_t *b)
{
    if (c->naxes >= ECM_AXIS_MAX) return -1;
    ecm_cia402_axis_t *a = &c->ax[c->naxes];
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;
    a->b = *b;
    const uint32_t m = cfg->modes;
    a->mode_req = (m & ECM_MODE_CSP) ? ECM_OPMODE_CSP : (m & ECM_MODE_CSV) ? ECM_OPMODE_CSV :
                  (m & ECM_MODE_PP) ? ECM_OPMODE_PP : (m & ECM_MODE_PV) ? ECM_OPMODE_PV :
                  (m & ECM_MODE_HM) ? ECM_OPMODE_HM : 0;
    return c->naxes++;
}

void ecm_cia402_set_ext_error(ecm_cia402_t *c, int axis, int err)
{
    if (axis < 0 || axis >= c->naxes) return;
    atomic_store_explicit(&c->ext_err[axis], (uint8_t)err, memory_order_release);
}

void ecm_cia402_set_home_timeout(ecm_cia402_t *c, uint32_t ms)
{
    c->home_timeout_ticks = (uint32_t)(((uint64_t)ms * 1000000ull + c->cycle_ns - 1) / c->cycle_ns);
    if (!c->home_timeout_ticks) c->home_timeout_ticks = 1;
}

void ecm_cia402_set_step_limit(ecm_cia402_t *c, int axis, int64_t max_pos, int64_t max_vel)
{
    if (axis < 0 || axis >= c->naxes) return;
    c->ax[axis].max_step_pos = max_pos > 0 ? max_pos : 0;
    c->ax[axis].max_step_vel = max_vel > 0 ? max_vel : 0;
}

void ecm_cia402_shutdown(ecm_cia402_t *c)
{
    if (ECM_CIA402_LATCH_OFF(6)) return;      /* negative control: leave OP from wherever */
    c->shutdown = 1;
    for (int i = 0; i < c->naxes; i++) c->ax[i].target = ECM_TGT_DISABLED;
}

int ecm_cia402_all_down(const ecm_cia402_t *c, int bus_lost)
{
    if (ECM_CIA402_LATCH_OFF(6) || bus_lost) return 1;
    for (int i = 0; i < c->naxes; i++) {
        ecm_ds_t ds = c->ax[i].ds;
        if (ds == ECM_DS_RTSO || ds == ECM_DS_SO || ds == ECM_DS_OE || ds == ECM_DS_QSA) return 0;
    }
    return 1;
}

/* ---- RT ------------------------------------------------------------------ */

static int64_t sext(uint64_t v, uint16_t bits)
{
    if (!bits || bits >= 64) return (int64_t)v;
    uint64_t m = 1ull << (bits - 1);
    v &= (1ull << bits) - 1;
    return (int64_t)((v ^ m) - m);
}

static int mode_allowed(const ecm_cia402_axis_t *a, int64_t m)
{
    if (m == ECM_OPMODE_CSP) return (a->cfg.modes & ECM_MODE_CSP) != 0;
    if (m == ECM_OPMODE_CSV) return (a->cfg.modes & ECM_MODE_CSV) != 0;
    if (m == ECM_OPMODE_PP)  return (a->cfg.modes & ECM_MODE_PP) != 0;    /* 10.7 */
    if (m == ECM_OPMODE_PV)  return (a->cfg.modes & ECM_MODE_PV) != 0;
    if (m == ECM_OPMODE_HM)  return (a->cfg.modes & ECM_MODE_HM) != 0;
    return 0;
}

static void apply_cmd(ecm_cia402_t *c, ecm_cia402_axis_t *a, const ecm_xcmd_t *x, uint64_t tick)
{
    a->last_seq = x->seq;
    switch (x->op) {
    case ECM_CIA_OP_ENABLE:
        if (c->shutdown) { a->err = ECM_AXERR_SHUTDOWN; break; }                       /* S6 */
        {
            uint8_t xe = atomic_load_explicit(&c->ext_err[a - c->ax], memory_order_acquire);
            if (xe && !ECM_CIA402_LATCH_OFF(10)) { a->err = xe; a->target = ECM_TGT_DISABLED; break; }   /* 10.8 */
        }
        if (a->ds == ECM_DS_FAULT || a->ds == ECM_DS_FRA) { a->err = ECM_AXERR_IN_FAULT; a->target = ECM_TGT_DISABLED; break; }
        if (!mode_allowed(a, a->mode_req)) { a->err = ECM_AXERR_MODE; a->target = ECM_TGT_DISABLED; break; }
        a->target = ECM_TGT_ENABLED;
        a->err = ECM_AXERR_NONE;
        a->err_ds = 0;
        a->ds_wait = a->ds;
        a->step_since = tick;
        a->reached_oe = 0;
        break;
    case ECM_CIA_OP_DISABLE:
        a->target = ECM_TGT_DISABLED;
        break;
    case ECM_CIA_OP_QUICKSTOP:
        a->target = ECM_TGT_QUICKSTOP;
        break;
    case ECM_CIA_OP_FAULT_RESET:
        a->target = ECM_TGT_DISABLED;          /* after a reset the axis stays disabled */
        if (a->ds == ECM_DS_FAULT) a->reset_pulse = 2;
        if (a->err == ECM_AXERR_FAULT || a->err == ECM_AXERR_IN_FAULT) a->err = ECM_AXERR_NONE;
        break;
    case ECM_CIA_OP_SET_MODE:
        if (!mode_allowed(a, x->arg0) || (!a->b.mode.bits && x->arg0 != a->mode_req)) {
            a->err = ECM_AXERR_MODE;          /* not configured, or no 0x6060 in the PDOs */
            break;
        }
        if (x->arg0 != a->mode_req) {
            a->mode_req = (int8_t)x->arg0;
            a->mode_ok = 0;
            a->armed = 0;                       /* the next setpoint is of the new kind */
        }
        break;
    case ECM_CIA_OP_PP_POINT:                  /* 10.7: only while running in PP */
        if (!a->running || a->mode_req != ECM_OPMODE_PP) { a->err = ECM_AXERR_MODE; a->pp_lost++; break; }
        if (a->pp_n == ECM_CIA402_PP_QUEUE) { a->pp_lost++; break; }
        {
            int i = (a->pp_head + a->pp_n) % ECM_CIA402_PP_QUEUE;
            a->pp_q[i] = x->arg0;
            a->pp_qf[i] = (uint8_t)x->arg1;
            a->pp_n++;
        }
        break;
    case ECM_CIA_OP_PV_VEL:
        if (!a->running || a->mode_req != ECM_OPMODE_PV) { a->err = ECM_AXERR_MODE; break; }
        a->pv_vel = x->arg0;
        break;
    case ECM_CIA_OP_HOME:                      /* no homing queued for a later enable */
        if (!a->running || a->mode_req != ECM_OPMODE_HM) { a->err = ECM_AXERR_MODE; break; }
        a->hm_phase = ECM_HM_RUNNING;
        a->hm_since = tick;
        a->hm_seen_low = 0;
        a->homed = 0;
        break;
    default:
        c->cmd_bad++;
        break;
    }
}

static void axis_rt(ecm_cia402_t *c, ecm_cia402_axis_t *a, int idx, uint8_t *io, uint64_t tick,
                    int in_valid, int bus_lost)
{
    const ecm_axis_bind_t *b = &a->b;
    const uint64_t k1 = tick + 1;

    /* 1. inputs (a bad frame keeps the last good ones) */
    if (in_valid) {
        a->sw = (uint16_t)ecm_pdo_get(&b->sw, io);
        if (b->apos.bits) a->apos = (int32_t)sext(ecm_pdo_get(&b->apos, io), b->apos.bits);
        if (b->avel.bits) a->avel = (int32_t)sext(ecm_pdo_get(&b->avel, io), b->avel.bits);
        if (b->err.bits)  a->ecode = (uint16_t)ecm_pdo_get(&b->err, io);
        if (b->mode_disp.bits) a->mode_disp = (int8_t)sext(ecm_pdo_get(&b->mode_disp, io), b->mode_disp.bits);
        else a->mode_disp = a->mode_req;       /* set by SDO / InitCmd before OP (10.3) */
        ecm_ds_t ds = ecm_cia402_decode(a->sw);
        if (ds != a->ds) a->transitions++;
        a->ds = ds;
    }
    a->mode_ok = a->mode_req != 0 && a->mode_disp == a->mode_req;

    /* 2. latches: bus lost, drive fault */
    if (bus_lost) {
        if (a->target != ECM_TGT_DISABLED && !ECM_CIA402_LATCH_OFF(5)) {            /* S5 */
            a->target = ECM_TGT_DISABLED; a->err = ECM_AXERR_BUS_LOST;
        }
    } else if ((a->ds == ECM_DS_FAULT || a->ds == ECM_DS_FRA) && a->target != ECM_TGT_DISABLED) {
        a->target = ECM_TGT_DISABLED;
        a->err = ECM_AXERR_FAULT;
        a->err_ds = (uint8_t)a->ds;
        if (ECM_CIA402_LATCH_OFF(7)) { a->target = ECM_TGT_ENABLED; a->reached_oe = 0; }   /* neg. control: resume */
    }
    if (ECM_CIA402_LATCH_OFF(7) && a->ds == ECM_DS_FAULT && a->err == ECM_AXERR_FAULT && !a->reset_pulse)
        a->reset_pulse = 2;                     /* negative control: reset without a command */
    /* 10.8: an error found by the monitor (slave configuration refused,
     * recovery gave up): latched like a fault, the root cause is shown */
    {
        uint8_t xe = atomic_load_explicit(&c->ext_err[idx], memory_order_acquire);
        if (xe && !ECM_CIA402_LATCH_OFF(10)) { a->target = ECM_TGT_DISABLED; a->err = xe; }
    }
    /* S1: Operation enabled left without a command (recovery took the slave
     * out of OP, drive-side quick stop / DI, ...) -> latched disabled, never
     * walked back up by the master */
    if (in_valid && a->target == ECM_TGT_ENABLED) {
        if (a->ds == ECM_DS_OE) a->reached_oe = 1;
        else if (a->reached_oe && !ECM_CIA402_LATCH_OFF(1)) {
            a->target = ECM_TGT_DISABLED;
            a->err = ECM_AXERR_DROPPED;
            a->err_ds = (uint8_t)a->ds;
        }
    }

    /* 3. (commands were applied by the caller) 4. controlword */
    uint16_t cw = 0x0000;
    int enabling_now = 0;
    switch (a->target) {
    case ECM_TGT_ENABLED:
        switch (a->ds) {
        case ECM_DS_SOD:  cw = 0x0006; break;
        case ECM_DS_RTSO: cw = 0x0007; break;
        case ECM_DS_SO:
            if (a->mode_ok) { cw = 0x000F; enabling_now = 1; }
            else cw = 0x0007;                  /* wait for 0x6061 */
            break;
        case ECM_DS_OE:   cw = 0x000F; break;
        default:          cw = 0x0000; break;  /* QSA / Not ready / unknown: back down */
        }
        break;
    case ECM_TGT_QUICKSTOP:
        if (a->ds == ECM_DS_OE || a->ds == ECM_DS_QSA) cw = 0x0002;
        else { a->target = ECM_TGT_DISABLED; cw = 0x0000; }
        break;
    default:                                    /* DISABLED: OE -> SO -> RTSO -> SOD */
        switch (a->ds) {
        case ECM_DS_OE:   cw = 0x0007; break;
        case ECM_DS_SO:   cw = 0x0006; break;
        default:          cw = 0x0000; break;
        }
        break;
    }
    if (a->reset_pulse == 2) { cw = 0x0080; a->reset_pulse = 1; }
    else if (a->reset_pulse == 1) { cw = 0x0000; a->reset_pulse = 0; }

    /* 5. timeout of a commanded transition (ENABLED only) */
    if (a->target == ECM_TGT_ENABLED && a->ds != ECM_DS_OE && !bus_lost) {
        if (a->ds != a->ds_wait) { a->ds_wait = a->ds; a->step_since = tick; }
        else if (tick - a->step_since > c->step_timeout_ticks) {
            a->err = ECM_AXERR_TIMEOUT;
            a->err_ds = (uint8_t)a->ds;
            a->target = ECM_TGT_DISABLED;
            cw = a->ds == ECM_DS_SO ? 0x0006 : 0x0000;
        }
    }

    if (bus_lost) return;                       /* nothing goes out without a bus */

    /* 6. setpoints for k+1 */
    int csp = a->mode_req == ECM_OPMODE_CSP, csv = a->mode_req == ECM_OPMODE_CSV;
    int running = a->target == ECM_TGT_ENABLED && a->ds == ECM_DS_OE && a->mode_ok;
    const int cyclic = csp || csv;
    a->running = (uint8_t)running;
    ecm_xring_t *r = &c->sp[idx];
    ecm_xsp_t e;
    int got = 0, skipped = 0;
    while (ecm_xring_peek(r, &e) == 0) {
        if (e.tick > k1) break;                 /* for a later tick */
        if (e.tick < k1 || !running || !cyclic) {   /* late, or not usable now: dropped */
            if (skipped == ECM_XCHG_SP_SKIP_MAX) break;
            if (e.tick < k1) a->late++; else a->dropped++;
            ecm_xring_pop(r);
            skipped++;
            continue;
        }
        a->last_sp = e.v[0];
        a->armed = 1;
        a->used++;
        got = 1;
        ecm_xring_pop(r);
        break;
    }
    if (running && cyclic && !got && a->armed) a->underrun++;
    /* S4: a step above the limit is not sent: quick stop, error STEP */
    if (got && !ECM_CIA402_LATCH_OFF(4)) {
        int64_t lim = csp ? a->max_step_pos : csv ? a->max_step_vel : 0;
        int64_t ref = a->last_sent;             /* CSP: = actual until armed (S2) */
        int64_t d = a->last_sp - ref;
        if (lim && (d > lim || d < -lim)) {
            a->step_seen = d;
            a->step_refused++;
            a->err = ECM_AXERR_STEP;
            a->target = ECM_TGT_QUICKSTOP;
            if (a->ds == ECM_DS_OE) cw = 0x0002;
            a->last_sp = ref;
            got = 0;
            running = 0;
            a->armed = 0;
        }
    }
    if (csp && b->tpos.bits) {
        int64_t t;
        if (!running || enabling_now || !a->armed) {
            t = a->apos;                        /* S2: target = actual (enable does not jump) */
            if (ECM_CIA402_LATCH_OFF(2)) t = a->last_sent;   /* negative control: stale target */
            if (!running) a->armed = 0;
            a->last_sp = t;
            a->last_delta = 0;
        } else if (got) {
            t = a->last_sp;                     /* new setpoint */
            a->last_delta = t - a->last_sent;
        } else {
            t = a->last_sp;                     /* S3: underrun, the last one held */
            if (ECM_CIA402_LATCH_OFF(3)) { t = a->last_sent + a->last_delta; a->last_sp = t; }  /* neg. control: extrapolate */
        }
        ecm_pdo_set(&b->tpos, io, (uint64_t)t);
        a->last_sent = t;
    }
    if (csv && b->tvel.bits) {
        int64_t v = (running && got) ? a->last_sp : 0;   /* underrun: velocity 0, not held */
        if (!running) a->armed = 0;
        ecm_pdo_set(&b->tvel, io, (uint64_t)v);
        a->last_sent = v;
    }
    /* 7. profile modes (10.7) */
    if (!running) {                             /* nothing in flight survives a stop */
        if (a->pp_n || a->pp_phase == 1) a->pp_lost += a->pp_n + (a->pp_phase == 1);
        a->pp_n = 0;
        a->pp_phase = 0;
        a->pv_vel = 0;
        if (a->hm_phase == ECM_HM_RUNNING) { a->hm_phase = ECM_HM_FAILED; a->err = a->err ? a->err : ECM_AXERR_HOMING; }
    }
    const uint16_t sw = a->sw;
    if (a->mode_req == ECM_OPMODE_PP) {
        if (!running) a->pp_cur = a->apos;
        else if (a->pp_phase == 0) {
            if (a->pp_n && ((!(sw & 0x1000) && ((a->pp_qf[a->pp_head] & ECM_PP_IMM) || (sw & 0x0400)))
                            || ECM_CIA402_LATCH_OFF(8))) {      /* negative control: no handshake */
                a->pp_cur = a->pp_q[a->pp_head];
                a->pp_flags = a->pp_qf[a->pp_head];
                a->pp_head = (uint8_t)((a->pp_head + 1) % ECM_CIA402_PP_QUEUE);
                a->pp_n--;
                a->pp_phase = 1;
                a->pp_since = tick;
                a->pp_sent++;
            }
        } else if (a->pp_phase == 1) {
            if (ECM_CIA402_LATCH_OFF(8)) { a->pp_phase = 0; a->pp_acked++; }   /* one tick of bit 4 */
            else if ((sw & 0x1000) && tick >= a->pp_since + 2) { a->pp_phase = 2; a->pp_acked++; }
            else if (tick - a->pp_since > c->step_timeout_ticks) {
                a->err = ECM_AXERR_HANDSHAKE;
                a->target = ECM_TGT_DISABLED;
                a->pp_phase = 0;
                a->pp_lost++;
                cw = 0x0007;
            }
        } else if (!(sw & 0x1000)) {
            a->pp_phase = 0;                    /* ack low again: ready for the next */
        }
        if (b->tpos.bits) ecm_pdo_set(&b->tpos, io, (uint64_t)a->pp_cur);
        if (running && cw == 0x000F && a->pp_phase == 1)
            cw |= 0x0010 | ((a->pp_flags & ECM_PP_IMM) ? 0x0020 : 0) | ((a->pp_flags & ECM_PP_REL) ? 0x0040 : 0);
    } else if (a->mode_req == ECM_OPMODE_PV) {
        if (b->tvel.bits) ecm_pdo_set(&b->tvel, io, (uint64_t)(running ? a->pv_vel : 0));
    } else if (a->mode_req == ECM_OPMODE_HM && running && a->hm_phase == ECM_HM_RUNNING) {
        const int seen = tick >= a->hm_since + 2 || ECM_CIA402_LATCH_OFF(9);   /* the first statusword after the start */
        if (seen && !(sw & 0x1000)) a->hm_seen_low = 1;
        if (seen && (sw & 0x2000)) {
            a->hm_phase = ECM_HM_FAILED;
            a->err = ECM_AXERR_HOMING;
        } else if (seen && (sw & 0x1400) == 0x1400 && (a->hm_seen_low || tick >= a->hm_since + ECM_CIA402_HM_SETTLE || ECM_CIA402_LATCH_OFF(9))) {
            a->hm_phase = ECM_HM_DONE;
            a->homed = 1;
        } else if (tick - a->hm_since > c->home_timeout_ticks) {
            a->hm_phase = ECM_HM_FAILED;
            a->err = ECM_AXERR_HOMING;
        }
        if (a->hm_phase == ECM_HM_RUNNING && cw == 0x000F) cw |= 0x0010;
    }
    if (b->mode.bits) ecm_pdo_set(&b->mode, io, (uint64_t)(uint8_t)a->mode_req);
    ecm_pdo_set(&b->cw, io, cw);
    a->cw = cw;
}

static void publish(ecm_cia402_t *c, int i, uint64_t tick)
{
    const ecm_cia402_axis_t *a = &c->ax[i];
    uint64_t w[ECM_XST_WORDS] = {
        (uint64_t)a->sw | (uint64_t)a->cw << 16 | (uint64_t)a->ds << 32 | (uint64_t)a->target << 40 |
            (uint64_t)a->err << 48 | (uint64_t)a->err_ds << 56,
        (uint64_t)(uint32_t)a->apos | (uint64_t)(uint32_t)a->avel << 32,
        (uint64_t)a->ecode | (uint64_t)(uint8_t)a->mode_disp << 16 | (uint64_t)a->mode_ok << 24 |
            (uint64_t)(uint8_t)a->mode_req << 32 | (uint64_t)a->homed << 40 | (uint64_t)a->hm_phase << 41 |
            (uint64_t)a->pp_phase << 43 | (uint64_t)a->pp_n << 45,
        a->used, a->late, a->underrun, (uint64_t)a->last_seq | (a->step_refused & 0xFFFFFFFFull) << 32,
        (a->dropped & 0xFFFFFFFFull) | (a->pp_acked & 0xFFFFFFFFull) << 32,
    };
    ecm_xst_write(&c->st[i], tick, w, ECM_XST_WORDS);
}

void ecm_cia402_rt(ecm_cia402_t *c, uint8_t *iomap, uint64_t tick, uint64_t t_send_ns,
                   int in_valid, int bus_lost)
{
    /* commands first (FIFO, bounded): they act on this tick's controlword */
    int n = 0;
    ecm_xcmd_t x;
    while (ecm_xring_peek(&c->cmd, &x) == 0) {
        if (x.tick > tick + 1) break;
        if (n == ECM_XCHG_CMD_PER_TICK) { c->cmd_deferred++; break; }
        if (x.target < c->naxes) apply_cmd(c, &c->ax[x.target], &x, tick);
        else c->cmd_bad++;
        c->cmd_applied++;
        ecm_xring_pop(&c->cmd);
        n++;
    }
    for (int i = 0; i < c->naxes; i++) {
        axis_rt(c, &c->ax[i], i, iomap, tick, in_valid, bus_lost);
        publish(c, i, tick);
    }
    uint64_t cw[ECM_XST_WORDS] = { t_send_ns, c->cycle_ns, (uint64_t)in_valid, (uint64_t)bus_lost,
                                   c->cmd_applied, c->cmd_deferred, c->cmd_bad, 0 };
    ecm_xst_write(&c->clock, tick, cw, ECM_XST_WORDS);
}

/* ---- application side ---------------------------------------------------- */

int ecm_cia402_cmd(ecm_cia402_t *c, int axis, int op, int64_t arg, uint32_t seq, uint64_t tick)
{
    if (axis < 0 || axis >= c->naxes) return -1;
    ecm_xcmd_t x = { .tick = tick, .target = (uint16_t)axis, .op = (uint16_t)op, .seq = seq, .arg0 = arg };
    return ecm_xring_push(&c->cmd, &x);
}

int ecm_cia402_cmd2(ecm_cia402_t *c, int axis, int op, int64_t arg0, int64_t arg1, uint32_t seq, uint64_t tick)
{
    if (axis < 0 || axis >= c->naxes) return -1;
    ecm_xcmd_t x = { .tick = tick, .target = (uint16_t)axis, .op = (uint16_t)op, .seq = seq, .arg0 = arg0, .arg1 = arg1 };
    return ecm_xring_push(&c->cmd, &x);
}

int ecm_cia402_setpoint(ecm_cia402_t *c, int axis, uint64_t tick, int64_t v)
{
    if (axis < 0 || axis >= c->naxes) return -1;
    ecm_xsp_t e = { .tick = tick, .v = { v, 0, 0 } };
    return ecm_xring_push(&c->sp[axis], &e);
}

int ecm_cia402_read(ecm_cia402_t *c, int axis, ecm_cia402_state_t *s)
{
    if (axis < 0 || axis >= c->naxes) return -1;
    uint64_t w[ECM_XST_WORDS], t;
    if (ecm_xst_read(&c->st[axis], &t, w, ECM_XST_WORDS, ECM_XST_TRIES)) return -1;
    memset(s, 0, sizeof(*s));
    s->tick = t;
    s->sw = (uint16_t)w[0]; s->cw = (uint16_t)(w[0] >> 16); s->ds = (uint8_t)(w[0] >> 32);
    s->target = (uint8_t)(w[0] >> 40); s->err = (uint8_t)(w[0] >> 48); s->err_ds = (uint8_t)(w[0] >> 56);
    s->apos = (int32_t)(uint32_t)w[1]; s->avel = (int32_t)(uint32_t)(w[1] >> 32);
    s->ecode = (uint16_t)w[2]; s->mode_disp = (int8_t)(uint8_t)(w[2] >> 16); s->mode_ok = (uint8_t)(w[2] >> 24);
    s->mode_req = (int8_t)(uint8_t)(w[2] >> 32);
    s->homed = (uint8_t)(w[2] >> 40) & 1; s->hm_phase = (uint8_t)(w[2] >> 41) & 3;
    s->pp_phase = (uint8_t)(w[2] >> 43) & 3; s->pp_n = (uint8_t)(w[2] >> 45) & 31;
    s->used = w[3]; s->late = w[4]; s->underrun = w[5]; s->last_seq = (uint32_t)w[6]; s->step_refused = (uint32_t)(w[6] >> 32); s->dropped = (uint32_t)w[7]; s->pp_acked = (uint32_t)(w[7] >> 32);
    return 0;
}

int ecm_cia402_read_clock(ecm_cia402_t *c, uint64_t *tick, uint64_t *t_send_ns)
{
    uint64_t w[ECM_XST_WORDS];
    if (ecm_xst_read(&c->clock, tick, w, ECM_XST_WORDS, ECM_XST_TRIES)) return -1;
    if (t_send_ns) *t_send_ns = w[0];
    return 0;
}
