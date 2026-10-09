/* ==========================================================================
 * esc_cia402.c — Phase 10.2 virtual CiA402 drive (see esc_cia402.h).
 *
 * ctl commands (soft_bus --ctl FIFO; node is 0-based like every other
 * soft_bus command, axis 0..AXES-1 or "all"):
 *   drv_status [node]                     print every axis of the node(s)
 *   drv_fault <node> <axis> <code> [latched]
 *                                         drive fault: 0x603F = code, EMCY
 *                                         posted, fault reaction (decelerate
 *                                         if enabled) -> Fault. "latched":
 *                                         fault reset is refused until
 *                                         drv_clear
 *   drv_clear <node> [axis]               end every injection on the axis
 *   drv_refuse_enable <node> <axis> <0|1> Switched on -> Operation enabled
 *                                         is refused (stays Switched on)
 *   drv_slow <node> <axis> <ms>           every commanded transition takes
 *                                         <ms> longer
 *   drv_ferr <node> <axis> <inc>          following error offset added to
 *                                         0x60F4 (fault if > 0x6065)
 *   drv_quickstop <node> <axis>           drive-side quick stop (DI): from
 *                                         Operation enabled -> Quick stop active
 *   drv_lose_sync <node>                  every axis: sync error fault
 *   drv_pos <node> <axis> <inc>           set the actual position (motor
 *                                         turned by hand while disabled)
 *   drv_home_switch <node> <axis> <inc|off>
 *                                         virtual home switch for homing 19/21
 * ========================================================================== */
#include "esc_cia402.h"
#include "esc_profile.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- defaults ---------------------------------------------------------- */

#define BIG              1e15     /* "no limit" for velocity / acceleration */
#define DFLT_PROFILE_VEL 100000.0 /* inc/s, PP/PV when 0x6081 is absent or 0 */
#define DFLT_ACCEL       1000000.0
#define DFLT_QS_DECEL    10000000.0

void esc_cia402_cal_default(esc_cia402_cal_t *c)
{
    memset(c, 0, sizeof(*c));
    c->power_on_ms     = 0;
    c->trans_cycles    = 1;
    c->code_lost_op    = 0x8700;   /* CiA 301: sync / communication. IS620N: R-07 */
    c->code_ferr       = 0x8611;   /* CiA 402: following error              */
    c->code_sync       = 0x8700;
    c->supported_modes = 0x000001A5; /* PP, PV, HM, CSP, CSV (0x6502 bits 0,2,5,7,8) */
    c->lost_op_emcy    = 1;
}

int esc_cia402_cal_load(esc_cia402_cal_t *c, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "%s: cannot open", path); return -1; }
    char ln[256];
    int no = 0;
    while (fgets(ln, sizeof(ln), f)) {
        no++;
        char *h = strchr(ln, '#');
        if (h) *h = 0;
        char k[64];
        double v;
        int n = sscanf(ln, " %63s %lf", k, &v);
        if (n <= 0) continue;
        if (n != 2 || v < 0) { snprintf(err, errlen, "%s:%d: expected 'key value' (value >= 0)", path, no); fclose(f); return -1; }
        if      (!strcmp(k, "power_on_ms"))       c->power_on_ms = (uint32_t)v;
        else if (!strcmp(k, "trans_cycles"))      c->trans_cycles = (uint32_t)v;
        else if (!strcmp(k, "code_lost_op"))      c->code_lost_op = (uint16_t)v;
        else if (!strcmp(k, "code_ferr"))         c->code_ferr = (uint16_t)v;
        else if (!strcmp(k, "code_sync"))         c->code_sync = (uint16_t)v;
        else if (!strcmp(k, "supported_modes"))   c->supported_modes = (uint32_t)v;
        else if (!strcmp(k, "csp_lag_ms"))        c->csp_lag_ms = v;
        else if (!strcmp(k, "vel_scale"))         c->vel_scale = v;
        else if (!strcmp(k, "lost_pd_fault"))     c->lost_pd_fault = v != 0;
        else if (!strcmp(k, "lost_op_emcy"))      c->lost_op_emcy = v != 0;
        else if (!strcmp(k, "lost_op_autoclear")) c->lost_op_autoclear = v != 0;
        else if (!strcmp(k, "reinit_ms"))         c->reinit_ms = (uint32_t)v;
        else if (!strcmp(k, "csp_target_window")) c->csp_target_window = (uint32_t)v;
        else if (!strcmp(k, "sw_target_hold"))    c->sw_target_hold = v != 0;
        else if (!strcmp(k, "disable_op_decel"))  c->disable_op_decel = v;
        else if (!strcmp(k, "op_delay_ms"))       c->op_delay_ms = (uint32_t)v;
        else { snprintf(err, errlen, "%s:%d: unknown key '%s'", path, no, k); fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

static const char *const DS_NAME[DS_COUNT] = {
    "Not ready to switch on", "Switch on disabled", "Ready to switch on", "Switched on",
    "Operation enabled", "Quick stop active", "Fault reaction active", "Fault"
};

const char *esc_cia402_ds_name(cia402_ds_t ds)
{
    return (unsigned)ds < DS_COUNT ? DS_NAME[ds] : "?";
}

uint16_t esc_cia402_sw_state(cia402_ds_t ds)
{
    switch (ds) {                       /* CiA 402 statusword, bits 0..6   */
    case DS_NOT_READY: return 0x0000;
    case DS_SOD:       return SW_SOD;                              /* x1xx 0000 */
    case DS_RTSO:      return SW_QS | SW_RTSO;                     /* x01x 0001 */
    case DS_SO:        return SW_QS | SW_RTSO | SW_SO;             /* x01x 0011 */
    case DS_OE:        return SW_QS | SW_RTSO | SW_SO | SW_OE;     /* x01x 0111 */
    case DS_QSA:       return SW_RTSO | SW_SO | SW_OE;             /* x00x 0111 */
    case DS_FRA:       return SW_RTSO | SW_SO | SW_OE | SW_FAULT;  /* x0xx 1111 */
    case DS_FAULT:     return SW_FAULT;                            /* x0xx 1000 */
    default:           return 0;
    }
}

/* Device control commands (CiA 402 controlword table). Bit 7 must be 0 for
 * every command; a rising bit 7 is "fault reset" and only acts in Fault. */
#define CMD_DISABLE_VOLTAGE(cw) (((cw) & 0x0082) == 0x0000)
#define CMD_QUICK_STOP(cw)      (((cw) & 0x0086) == 0x0002)
#define CMD_SHUTDOWN(cw)        (((cw) & 0x0087) == 0x0006)
#define CMD_SWITCH_ON(cw)       (((cw) & 0x008F) == 0x0007)
#define CMD_ENABLE_OP(cw)       (((cw) & 0x008F) == 0x000F)

cia402_ds_t esc_cia402_next(cia402_ds_t ds, uint16_t cw, uint16_t cw_prev, int16_t qs_option)
{
    int fr_edge = (cw & CW_FR) && !(cw_prev & CW_FR);
    switch (ds) {
    case DS_NOT_READY: return DS_NOT_READY;            /* leaves by itself (power-on) */
    case DS_FAULT:     return fr_edge ? DS_SOD : DS_FAULT;              /* T15 */
    case DS_FRA:       return DS_FRA;                  /* leaves by itself (T14)      */
    case DS_SOD:       return CMD_SHUTDOWN(cw) ? DS_RTSO : DS_SOD;      /* T2  */
    case DS_RTSO:
        if (CMD_DISABLE_VOLTAGE(cw) || CMD_QUICK_STOP(cw)) return DS_SOD;  /* T7 */
        if (CMD_SWITCH_ON(cw) || CMD_ENABLE_OP(cw)) return DS_SO;          /* T3 */
        return DS_RTSO;
    case DS_SO:
        if (CMD_DISABLE_VOLTAGE(cw) || CMD_QUICK_STOP(cw)) return DS_SOD;  /* T10 */
        if (CMD_SHUTDOWN(cw)) return DS_RTSO;                              /* T6  */
        if (CMD_ENABLE_OP(cw)) return DS_OE;                               /* T4  */
        return DS_SO;
    case DS_OE:
        if (CMD_DISABLE_VOLTAGE(cw)) return DS_SOD;                        /* T9  */
        if (CMD_QUICK_STOP(cw)) return DS_QSA;                             /* T11 */
        if (CMD_SHUTDOWN(cw)) return DS_RTSO;                              /* T8  */
        if (CMD_SWITCH_ON(cw)) return DS_SO;                               /* T5  */
        return DS_OE;
    case DS_QSA:
        if (CMD_DISABLE_VOLTAGE(cw)) return DS_SOD;                        /* T12 */
        if (qs_option >= 5 && qs_option <= 8 && CMD_ENABLE_OP(cw)) return DS_OE; /* T16 */
        return DS_QSA;
    default:           return ds;
    }
}

/* ---- object dictionary and PDO access ---------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* bits of index:sub in the profile OD and its value (up to 32 bit), or 0 */
static int od_get(const esc_t *e, uint16_t idx, uint8_t sub, uint32_t *v)
{
    int g = esc_prof_sub(e, idx, sub);
    if (g < 0) return 0;
    const esc_prof_sub_t *s = &e->prof->sub[g];
    const uint8_t *p = esc_prof_value(e, g);
    uint32_t x = 0;
    for (int i = 0; i < s->len && i < 4; i++) x |= (uint32_t)p[i] << (8 * i);
    *v = x;
    return s->bits;
}

static void od_set(esc_t *e, uint16_t idx, uint8_t sub, uint32_t v)
{
    int g = esc_prof_sub(e, idx, sub);
    if (g < 0) return;
    const esc_prof_sub_t *s = &e->prof->sub[g];
    uint8_t *p = esc_prof_value(e, g);
    for (int i = 0; i < s->len && i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_bits(const uint8_t *base, uint32_t bit, uint16_t bits)
{
    uint32_t v = 0;
    for (uint16_t k = 0; k < bits && k < 32; k++)
        if (base[(bit + k) / 8] & (1u << ((bit + k) % 8))) v |= 1u << k;
    return v;
}

static void set_bits(uint8_t *base, uint32_t bit, uint16_t bits, uint32_t v)
{
    for (uint16_t k = 0; k < bits && k < 32; k++) {
        uint8_t m = (uint8_t)(1u << ((bit + k) % 8));
        if (v & (1u << k)) base[(bit + k) / 8] |= m;
        else               base[(bit + k) / 8] &= (uint8_t)~m;
    }
}

static int32_t sext(uint32_t v, int bits)
{
    if (bits <= 0 || bits >= 32) return (int32_t)v;
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return (int32_t)((v ^ m) - m);
}

/* Which axis does an object belong to; base index of axis 0. -1 = none. */
static int axis_of(const esc_cia402_t *d, uint16_t idx, uint16_t *base)
{
    if (idx < 0x6000) return -1;
    int a = (idx - 0x6000) / CIA402_AXIS_OFF;
    if (a >= d->axes) return -1;
    *base = (uint16_t)(idx - a * CIA402_AXIS_OFF);
    return a;
}

/* master -> drive objects */
static int put_cmd(esc_cia402_axis_t *x, uint16_t base, uint8_t sub, uint32_t v, int bits)
{
    switch (base) {
    case 0x6040: x->cw = (uint16_t)v; return 1;
    case 0x6060: x->mode_req = (int8_t)sext(v, bits ? bits : 8); return 1;
    case 0x607A: x->target_pos = sext(v, bits); return 1;
    case 0x60FF: x->target_vel = sext(v, bits); return 1;
    case 0x6071: x->target_torque = (int16_t)sext(v, bits); return 1;
    case 0x6081: x->profile_vel = v; return 1;
    case 0x607F: x->max_profile_vel = v; return 1;
    case 0x6083: x->accel = v; return 1;
    case 0x6084: x->decel = v; return 1;
    case 0x6085: x->qs_decel = v; return 1;
    case 0x60C5: x->max_accel = v; return 1;
    case 0x60C6: x->max_decel = v; return 1;
    case 0x6080: x->max_motor_speed = v; return 1;
    case 0x605A: x->qs_option = (int16_t)sext(v, bits); return 1;
    case 0x6065: x->ferr_window = v; return 1;
    case 0x6098: x->hm_method = (int8_t)sext(v, bits ? bits : 8); return 1;
    case 0x6099: if (sub == 1) x->hm_speed_fast = v; else if (sub == 2) x->hm_speed_slow = v; return 1;
    case 0x609A: x->hm_accel = v; return 1;
    case 0x607C: x->hm_offset = sext(v, bits); return 1;
    default:     return 0;
    }
}

static int32_t pos_i32(double p)
{
    long long q = llround(p);
    return (int32_t)(uint32_t)(unsigned long long)q;     /* wraps like a 32-bit counter */
}

/* drive -> master objects; 1 if the object is one the drive produces */
static int get_state(const esc_cia402_axis_t *x, uint16_t base, uint8_t sub, uint32_t *v)
{
    (void)sub;
    switch (base) {
    case 0x6041: *v = x->sw; return 1;
    case 0x6061: *v = (uint8_t)x->mode; return 1;
    case 0x6064: *v = (uint32_t)pos_i32(x->pos); return 1;
    case 0x6063: *v = (uint32_t)pos_i32(x->pos); return 1;
    case 0x606C: *v = (uint32_t)(int32_t)llround(x->vel); return 1;
    case 0x6077: *v = 0; return 1;
    case 0x6078: *v = 0; return 1;
    case 0x603F: *v = x->err_code; return 1;
    case 0x60F4: *v = (uint32_t)x->ferr; return 1;
    case 0x6502: *v = x->supported; return 1;
    case 0x60FD: {                                  /* bit 2: home switch       */
        uint32_t di = 0;
        if (x->hm_has_switch && x->pos >= x->hm_switch) di |= 1u << 2;
        *v = di; return 1;
    }
    default: return 0;
    }
}

typedef void (*entry_fn)(esc_t *e, uint16_t idx, uint8_t sub, uint16_t bits, uint8_t *base, uint32_t bit);

/* Every entry of the PDOs assigned now in one direction (0 out = SM2, 1 in
 * = SM3), with its bit position in that SM's DPRAM area. */
static void for_each_entry(esc_t *e, int dir, entry_fn fn)
{
    const uint8_t *sm = e->regs + REG_SM_BASE + (2 + dir) * REG_SM_ENTRY_SIZE;
    uint16_t start = rd16(sm + SM_OFF_PHYS_START), len = rd16(sm + SM_OFF_LENGTH);
    if (!(sm[SM_OFF_ACTIVATE] & SM_ACT_ENABLE) || !len || (uint32_t)start + len > ESC_REG_SPACE_SIZE) return;
    uint16_t obj = dir ? 0x1C13 : 0x1C12;
    uint32_t n = 0, bit = 0;
    if (!od_get(e, obj, 0, &n)) return;
    for (uint32_t a = 1; a <= n; a++) {
        uint32_t pdo = 0, m = 0;
        if (!od_get(e, obj, (uint8_t)a, &pdo) || !od_get(e, (uint16_t)pdo, 0, &m)) continue;
        for (uint32_t k = 1; k <= m; k++) {
            uint32_t ent = 0;
            if (!od_get(e, (uint16_t)pdo, (uint8_t)k, &ent)) continue;
            uint16_t bits = (uint16_t)(ent & 0xFF);
            if (bit + bits > (uint32_t)len * 8) return;
            fn(e, (uint16_t)(ent >> 16), (uint8_t)(ent >> 8), bits, e->regs + start, bit);
            bit += bits;
        }
    }
}

static void take_output(esc_t *e, uint16_t idx, uint8_t sub, uint16_t bits, uint8_t *base, uint32_t bit)
{
    uint16_t b;
    int a = axis_of(e->drv, idx, &b);
    if (a < 0 || !idx) return;
    uint32_t v = get_bits(base, bit, bits);
    if (put_cmd(&e->drv->ax[a], b, sub, v, bits)) od_set(e, idx, sub, v);   /* SDO sees it too */
}

static void give_input(esc_t *e, uint16_t idx, uint8_t sub, uint16_t bits, uint8_t *base, uint32_t bit)
{
    uint16_t b;
    int a = axis_of(e->drv, idx, &b);
    uint32_t v;
    if (a < 0 || !idx || !get_state(&e->drv->ax[a], b, sub, &v)) return;
    set_bits(base, bit, bits, v);
}

/* Parameters and commands that are not in a PDO come from the OD (SDO,
 * ENI InitCmds). Read every one the profile has, before the PDO pass. */
static const struct { uint16_t idx; uint8_t sub; } OD_IN[] = {
    { 0x6040, 0 }, { 0x6060, 0 }, { 0x607A, 0 }, { 0x60FF, 0 }, { 0x6071, 0 },
    { 0x6081, 0 }, { 0x607F, 0 }, { 0x6083, 0 }, { 0x6084, 0 }, { 0x6085, 0 },
    { 0x605A, 0 }, { 0x6065, 0 }, { 0x6098, 0 }, { 0x6099, 1 }, { 0x6099, 2 },
    { 0x609A, 0 }, { 0x607C, 0 }, { 0x60C5, 0 }, { 0x60C6, 0 }, { 0x6080, 0 },
};
static const struct { uint16_t idx; uint8_t sub; } OD_OUT[] = {
    { 0x6041, 0 }, { 0x6061, 0 }, { 0x6064, 0 }, { 0x6063, 0 }, { 0x606C, 0 },
    { 0x6077, 0 }, { 0x603F, 0 }, { 0x60F4, 0 }, { 0x60FD, 0 },
};

static void od_pull(esc_t *e, int a)
{
    esc_cia402_axis_t *x = &e->drv->ax[a];
    uint16_t off = (uint16_t)(a * CIA402_AXIS_OFF);
    for (size_t k = 0; k < sizeof(OD_IN) / sizeof(OD_IN[0]); k++) {
        uint32_t v;
        int bits = od_get(e, (uint16_t)(OD_IN[k].idx + off), OD_IN[k].sub, &v);
        if (bits) put_cmd(x, OD_IN[k].idx, OD_IN[k].sub, v, bits);
    }
    uint32_t s;
    if (od_get(e, (uint16_t)(0x6502 + off), 0, &s)) x->supported = s;
}

static void od_push(esc_t *e, int a)
{
    esc_cia402_axis_t *x = &e->drv->ax[a];
    uint16_t off = (uint16_t)(a * CIA402_AXIS_OFF);
    for (size_t k = 0; k < sizeof(OD_OUT) / sizeof(OD_OUT[0]); k++) {
        uint32_t v;
        if (get_state(x, OD_OUT[k].idx, OD_OUT[k].sub, &v))
            od_set(e, (uint16_t)(OD_OUT[k].idx + off), OD_OUT[k].sub, v);
    }
}

/* ---- attach / reset ----------------------------------------------------- */

static void axis_reset(esc_cia402_axis_t *x, uint32_t supported)
{
    memset(x, 0, sizeof(*x));
    x->ds = DS_NOT_READY;
    x->qs_option = 2;
    x->ferr_window = 0xFFFFFFFFu;
    x->supported = supported;
}

int esc_cia402_attach(esc_t *esc, int axes)
{
    if (!esc->prof || axes < 1 || axes > CIA402_MAX_AXES) return -1;
    if (!esc->drv) {
        esc->drv = calloc(1, sizeof(*esc->drv));
        if (!esc->drv) return -1;
        esc_cia402_cal_default(&esc->drv->cal);
    }
    esc->drv->axes = axes;
    esc_cia402_reset(esc);
    return 0;
}

void esc_cia402_reset(esc_t *esc)
{
    esc_cia402_t *d = esc->drv;
    if (!d) return;
    for (int a = 0; a < CIA402_MAX_AXES; a++) axis_reset(&d->ax[a], d->cal.supported_modes);
    d->last_pd_ns = 0;
    d->dt_ns = 0;
    d->was_op = 0;
}

void esc_cia402_detach(esc_t *esc)
{
    free(esc->drv);
    esc->drv = NULL;
}

/* ---- the drive ---------------------------------------------------------- */

#define LOGD(log, ...) do { if (log) { fprintf(log, "cia402: " __VA_ARGS__); fputc('\n', log); fflush(log); } } while (0)

static void set_ds(esc_t *e, int a, cia402_ds_t to, FILE *log, const char *why)
{
    esc_cia402_axis_t *x = &e->drv->ax[a];
    if (x->ds == to) return;
    LOGD(log, "node %d axis %d %s -> %s (%s, cw 0x%04X)", e->position_in_chain, a,
         esc_cia402_ds_name(x->ds), esc_cia402_ds_name(to), why, x->cw);
    if (x->ds == DS_OE && e->drv->cal.sw_target_hold && (x->sw & SW_TARGET) && to != DS_FRA && to != DS_FAULT)
        x->tr_hold = 1;                        /* IS620N: 0x0631 after disable */
    if (to == DS_OE) x->tr_hold = 0;
    if (to != DS_OE) x->braking = 0;
    x->ds = to;
    x->transitions++;
    x->wait_cycles = 0;
    if (to != DS_OE) {                         /* motion state of an enable ends */
        x->pp_active = x->pp_buffered = x->pp_ack = 0;
        x->hm_running = 0;
    }
}

static void raise_fault(esc_t *e, int a, uint16_t code, FILE *log, const char *why)
{
    esc_cia402_axis_t *x = &e->drv->ax[a];
    if (x->ds == DS_FAULT || x->ds == DS_FRA) return;
    x->err_code = code;
    x->faults++;
    x->tr_hold = 0;
    x->braking = 0;
    if (code != e->drv->cal.code_lost_op || e->drv->cal.lost_op_emcy) {   /* IS620N: 0x0E08, no EMCY */
        x->emcy++;
        e->fault.emcy_code = code;             /* posted through SM1 (esc_fault.c, Phase 9.7) */
        e->fault.emcy_reg = 0x01;              /* error register: generic error          */
        e->fault.emcy_left++;
    }
    int moving = x->ds == DS_OE || x->ds == DS_QSA;
    set_ds(e, a, moving ? DS_FRA : DS_FAULT, log, why);
}

static int mode_bit(int8_t mode)
{
    switch (mode) {
    case MODE_PP: return 0; case MODE_VL: return 1; case MODE_PV: return 2; case MODE_TQ: return 3;
    case MODE_HM: return 5; case MODE_IP: return 6; case MODE_CSP: return 7; case MODE_CSV: return 8;
    case MODE_CST: return 9; default: return -1;
    }
}

/* velocity one step toward v_to, rising with acc, falling with dec */
static double ramp(double v, double v_to, double acc, double dec, double dt)
{
    double dv = v_to - v;
    double lim = (fabs(v_to) > fabs(v) && v * v_to >= 0) ? acc * dt : dec * dt;
    if (dv > lim) dv = lim;
    if (dv < -lim) dv = -lim;
    return v + dv;
}

static double lim_or(double v, double dflt) { return v > 0 ? v : dflt; }

static void stop_motion(esc_cia402_axis_t *x, double dec, double dt)
{
    x->vel = ramp(x->vel, 0.0, dec, dec, dt);
    x->pos += x->vel * dt;
}

/* PP: one step toward pp_target with a trapezoidal profile */
static int pp_step(esc_cia402_axis_t *x, double dt)
{
    double vmax = lim_or(x->profile_vel, DFLT_PROFILE_VEL);
    if (x->max_profile_vel && x->max_profile_vel < vmax) vmax = x->max_profile_vel;
    double acc = lim_or(x->accel, DFLT_ACCEL), dec = lim_or(x->decel, DFLT_ACCEL);
    double rem = (double)x->pp_target - x->pos;
    if (fabs(rem) < 0.5 && fabs(x->vel) * dt < 0.5) { x->pos = x->pp_target; x->vel = 0; return 1; }
    double dir = rem > 0 ? 1.0 : -1.0;
    double vb = sqrt(2.0 * dec * fabs(rem));
    double vcmd = dir * (vb < vmax ? vb : vmax);
    x->vel = ramp(x->vel, vcmd, acc, dec, dt);
    double step = x->vel * dt;
    if ((rem > 0 && step >= rem) || (rem < 0 && step <= rem)) { x->pos = x->pp_target; x->vel = 0; return 1; }
    x->pos += step;
    return 0;
}

static void motion(esc_t *e, int a, double dt, FILE *log)
{
    esc_cia402_axis_t *x = &e->drv->ax[a];
    uint16_t edge4 = (x->cw & CW_OMS4) && !(x->cw_prev & CW_OMS4);
    uint16_t sw_oms = 0;
    /* Cyclic modes follow the master's setpoint; only the drive limits
     * (max acceleration 0x60C5/0x60C6, max motor speed 0x6080) apply. The
     * profile objects 0x6081/0x6083/0x6084 are for PP/PV (CiA 402). */
    double acc = lim_or(x->max_accel, BIG), dec = lim_or(x->max_decel, BIG);
    double vmax = x->max_motor_speed ? (double)x->max_motor_speed : BIG;

    if (x->ds == DS_QSA || x->ds == DS_FRA) {
        stop_motion(x, lim_or(x->qs_decel, DFLT_QS_DECEL), dt);
        return;
    }
    if (x->ds != DS_OE) { x->vel = 0; return; }
    if (x->braking) {                                          /* 10.2: Disable operation, IS620N brakes in OE */
        stop_motion(x, e->drv->cal.disable_op_decel, dt);
        x->ferr = (int32_t)llround((double)x->target_pos - x->pos);
        return;
    }

    int halt = (x->cw & CW_HALT) != 0;
    double vs = e->drv->cal.vel_scale > 0 ? e->drv->cal.vel_scale : 1.0;   /* 10.2: IS620N -0.37 % */
    switch (x->mode) {
    case MODE_CSP: {
        double tau = e->drv->cal.csp_lag_ms > 0 ? e->drv->cal.csp_lag_ms * 1e-3 : dt;   /* 10.2: IS620N ~26 ms */
        if (tau < dt) tau = dt;
        double v_des = ((double)x->target_pos - x->pos) / tau;
        /* 32-bit wrap: the shortest way round, as a drive with a 32-bit target does */
        if (v_des * tau > 2147483648.0) v_des -= 4294967296.0 / tau;
        if (v_des * tau < -2147483648.0) v_des += 4294967296.0 / tau;
        double v = ramp(x->vel, v_des, acc, dec, dt);
        if (v > vmax) v = vmax;
        if (v < -vmax) v = -vmax;
        x->vel = v;
        x->pos += v * dt;
        x->ferr = (int32_t)llround((double)x->target_pos - x->pos) + x->inj_ferr;
        sw_oms |= SW_OMS12;                                   /* following the target */
        if (e->drv->cal.csp_target_window && (uint32_t)abs(x->ferr) <= e->drv->cal.csp_target_window)
            sw_oms |= SW_TARGET;                              /* IS620N 0x1637 standing */
        if (x->ferr_window != 0xFFFFFFFFu && (uint32_t)abs(x->ferr) > x->ferr_window) {
            sw_oms |= SW_OMS13;
            raise_fault(e, a, e->drv->cal.code_ferr, log, "following error > 0x6065");
            return;
        }
        break;
    }
    case MODE_CSV: {
        double v = ramp(x->vel, halt ? 0.0 : (double)x->target_vel * vs, acc, dec, dt);
        if (v > vmax) v = vmax;
        if (v < -vmax) v = -vmax;
        x->vel = v;
        x->pos += v * dt;
        sw_oms |= SW_OMS12;
        break;
    }
    case MODE_PV: {
        double pa = lim_or(x->accel, DFLT_ACCEL), pd = lim_or(x->decel, DFLT_ACCEL);
        double to = halt ? 0.0 : (double)x->target_vel * vs;
        x->vel = ramp(x->vel, to, pa, pd, dt);
        x->pos += x->vel * dt;
        if (fabs(x->vel - to) < 0.5) sw_oms |= SW_TARGET;
        if (fabs(x->vel) < 0.5) sw_oms |= SW_OMS12;          /* speed = 0 */
        break;
    }
    case MODE_PP: {
        if (edge4) {
            int32_t base = x->pp_active ? x->pp_target : pos_i32(x->pos);
            int32_t t = (x->cw & CW_OMS6) ? base + x->target_pos : x->target_pos;
            if (!x->pp_active || (x->cw & CW_OMS5)) {
                x->pp_target = t; x->pp_active = 1; x->pp_ack = 1;
                x->pp_buffered = 0;
            } else if (!x->pp_buffered) {
                x->pp_next = t; x->pp_buffered = 1; x->pp_ack = 1;
            }
        }
        if (!(x->cw & CW_OMS4)) x->pp_ack = 0;
        if (halt) {
            stop_motion(x, lim_or(x->decel, DFLT_ACCEL), dt);
            if (fabs(x->vel) < 0.5) sw_oms |= SW_TARGET;
        } else if (x->pp_active) {
            if (pp_step(x, dt)) {
                x->pp_active = 0;
                if (x->pp_buffered) {
                    x->pp_target = x->pp_next; x->pp_buffered = 0; x->pp_active = 1;
                }
            }
        } else {
            x->vel = 0;
        }
        if (!x->pp_active && !halt) sw_oms |= SW_TARGET;
        if (x->pp_ack) sw_oms |= SW_OMS12;
        x->ferr = x->inj_ferr;
        break;
    }
    case MODE_HM: {
        if (edge4) {
            x->hm_done = x->hm_error = 0;
            x->hm_phase = 0;
            if (x->hm_method == 35 || x->hm_method == 37) {      /* current position */
                x->pos = x->hm_offset; x->vel = 0; x->hm_done = 1;
            } else if ((x->hm_method == 19 || x->hm_method == 21) && x->hm_has_switch) {
                x->hm_running = 1;
            } else {
                x->hm_error = 1;                                  /* method not supported here */
            }
        }
        if (!(x->cw & CW_OMS4) && x->hm_running) {                /* start bit cleared: stop */
            x->hm_running = 0;
        }
        if (x->hm_running) {
            double dir = x->hm_method == 19 ? 1.0 : -1.0;
            double sp = lim_or(x->hm_speed_fast, DFLT_PROFILE_VEL);
            double ha = lim_or(x->hm_accel, DFLT_ACCEL);
            x->vel = ramp(x->vel, dir * sp, ha, ha, dt);
            double np = x->pos + x->vel * dt;
            int hit = dir > 0 ? (x->pos < x->hm_switch && np >= x->hm_switch)
                              : (x->pos > x->hm_switch && np <= x->hm_switch);
            if (hit || (dir > 0 ? x->pos >= x->hm_switch : x->pos <= x->hm_switch)) {
                /* the switch edge becomes the home offset; stop there (simplified:
                 * no slow second approach) */
                x->pos = x->hm_offset;
                x->hm_switch = x->hm_offset;     /* the switch moved with the new frame */
                x->vel = 0;
                x->hm_running = 0;
                x->hm_done = 1;
            } else {
                x->pos = np;
            }
        } else {
            x->vel = 0;
        }
        if (x->hm_done) sw_oms |= SW_OMS12 | SW_TARGET;
        if (x->hm_error) sw_oms |= SW_OMS13 | SW_TARGET;
        break;
    }
    default:
        x->vel = 0;                                               /* no mode: hold */
        break;
    }
    x->sw |= sw_oms;
}

static uint32_t ms_to_cycles(uint32_t ms, uint64_t dt_ns)
{
    if (!ms || !dt_ns) return 0;
    return (uint32_t)(((uint64_t)ms * 1000000ull + dt_ns - 1) / dt_ns);
}

static void axis_step(esc_t *e, int a, int op, double dt, uint64_t dt_ns, FILE *log)
{
    esc_cia402_t *d = e->drv;
    esc_cia402_axis_t *x = &d->ax[a];

    if (x->ds == DS_NOT_READY) {
        x->t_since_power_ns += dt_ns;
        uint32_t ms = x->reinit ? d->cal.reinit_ms : d->cal.power_on_ms;
        if (x->t_since_power_ns >= (uint64_t)ms * 1000000ull) {
            set_ds(e, a, DS_SOD, log, x->reinit ? "re-initialised after the cleared fault" : "power-on self test done");
            x->reinit = 0;
        }
    }

    /* faults first: injected, ESM left OP while enabled */
    if (x->inj_fault_code && x->ds != DS_FAULT && x->ds != DS_FRA)
        raise_fault(e, a, x->inj_fault_code, log, "drv_fault");
    if (!op && (x->ds == DS_OE || x->ds == DS_QSA))
        raise_fault(e, a, d->cal.code_lost_op, log, "EtherCAT state left OP while enabled");
    else if (!op && (x->ds == DS_RTSO || x->ds == DS_SO)) {
        /* 10.2: IS620N, TwinCAT capture 9/10: process data lost (SM watchdog,
         * AL 0x001B) in Ready to switch on -> Fault 0x0E08 too */
        if (d->cal.lost_pd_fault && rd16(e->regs + REG_AL_STATUS_CODE) == 0x001B)
            raise_fault(e, a, d->cal.code_lost_op, log, "process data lost (SM watchdog)");
        else
            set_ds(e, a, DS_SOD, log, "EtherCAT state left OP");
    }
    /* 10.2: IS620N clears the lost-OP fault itself back in OP (W-06, X-05):
     * Fault -> Not ready (0x603F 0) -> Switch on disabled after reinit_ms */
    if (op && d->cal.lost_op_autoclear && x->ds == DS_FAULT && x->err_code == d->cal.code_lost_op
        && !x->inj_fault_code) {
        x->err_code = 0;
        x->reinit = 1;
        x->t_since_power_ns = 0;
        set_ds(e, a, DS_NOT_READY, log, "lost-OP fault cleared by the drive");
    }
    if (x->inj_quickstop && x->ds == DS_OE) {
        x->inj_quickstop = 0;
        set_ds(e, a, DS_QSA, log, "drv_quickstop");
    }

    /* mode of operation (only from OP outputs or SDO) */
    if (x->mode_req != x->mode) {
        int b = mode_bit(x->mode_req);
        if (x->mode_req == 0 || (b >= 0 && (x->supported & (1u << b)))) {
            if (x->mode != x->mode_req)
                LOGD(log, "node %d axis %d mode %d -> %d", e->position_in_chain, a, x->mode, x->mode_req);
            x->mode = x->mode_req;
            x->warn_mode = 0;
            x->pp_active = x->pp_buffered = x->pp_ack = 0;
            x->hm_running = x->hm_done = x->hm_error = 0;
        } else {
            x->warn_mode = 1;
        }
    }

    /* commanded transitions (outputs are only valid in OP) */
    if (op && x->ds != DS_NOT_READY && x->ds != DS_FRA) {
        cia402_ds_t nx = esc_cia402_next(x->ds, x->cw, x->cw_prev, x->qs_option);
        if (nx == DS_SOD && x->ds == DS_FAULT && x->inj_latched && x->err_code) {
            LOGD(log, "node %d axis %d fault reset refused: fault 0x%04X still present (latched)",
                 e->position_in_chain, a, x->err_code);
            nx = DS_FAULT;
        }
        if (nx == DS_OE && x->ds != DS_QSA && x->inj_refuse_enable) nx = x->ds;
        /* 10.2: Disable operation while moving: the IS620N brakes and reports
         * Operation enabled until it stands (W-07 / W-08) */
        if (x->ds == DS_OE && nx == DS_SO && d->cal.disable_op_decel > 0 && fabs(x->vel) >= 0.5) {
            if (!x->braking) LOGD(log, "node %d axis %d Disable operation: braking in OE", e->position_in_chain, a);
            x->braking = 1;
            nx = DS_OE;
        }
        if (nx != x->ds) {
            int urgent = nx == DS_QSA || nx == DS_SOD;   /* stopping is never delayed */
            uint32_t need = urgent ? 0 : (d->cal.trans_cycles ? d->cal.trans_cycles - 1 : 0)
                                         + ms_to_cycles(x->inj_slow_ms, dt_ns);
            if (x->wait_cycles == 0 || x->pending != nx) { x->pending = nx; x->wait_cycles = need + 1; }
            if (--x->wait_cycles == 0) {
                if (nx == DS_SOD && x->ds == DS_FAULT) x->err_code = 0;   /* fault reset */
                if (nx == DS_OE && x->mode == MODE_CSP) x->ferr = 0;
                set_ds(e, a, nx, log, x->ds == DS_FAULT ? "fault reset" : "controlword");
            }
        } else {
            x->wait_cycles = 0;
        }
    }

    x->sw = esc_cia402_sw_state(x->ds) | SW_VE | SW_REMOTE;
    if (x->warn_mode) x->sw |= SW_WARNING;
    motion(e, a, dt, log);

    /* end of a fault reaction / quick stop: the motor has stopped */
    if (x->ds == DS_FRA && fabs(x->vel) < 0.5) {
        x->vel = 0;
        set_ds(e, a, DS_FAULT, log, "fault reaction done");
    } else if (x->ds == DS_QSA && fabs(x->vel) < 0.5 && (x->qs_option <= 4)) {
        x->vel = 0;
        set_ds(e, a, DS_SOD, log, "quick stop done");
    }
    x->sw = (uint16_t)((x->sw & ~0x006Fu) | esc_cia402_sw_state(x->ds));
    if (x->ds != DS_OE) x->sw &= (uint16_t)~(SW_OMS12 | SW_OMS13 | SW_TARGET);
    if (x->tr_hold && (x->ds == DS_RTSO || x->ds == DS_SO || x->ds == DS_SOD)) x->sw |= SW_TARGET;
    x->cw_prev = x->cw;
    x->steps++;
}

static uint64_t step_dt(esc_t *e, uint64_t now_ns)
{
    esc_cia402_t *d = e->drv;
    uint32_t sync0 = rd32(e->regs + REG_DC_SYNC0_CYCLE);
    uint64_t dt;
    if ((e->regs[0x0981] & 0x01) && sync0 >= 100000u && sync0 <= 10000000u) {
        dt = sync0;                                 /* SYNC0 active: its cycle    */
    } else if (d->last_pd_ns && now_ns > d->last_pd_ns) {
        dt = now_ns - d->last_pd_ns;
        if (dt < 100000u) dt = 100000u;
        if (dt > 10000000u) dt = 10000000u;
    } else {
        dt = 1000000u;
    }
    d->last_pd_ns = now_ns;
    return dt;
}

void esc_cia402_step_dt(esc_t *e, uint64_t dt_ns, FILE *log)
{
    esc_cia402_t *d = e->drv;
    if (!d || !e->prof || e->fault.powered_off) return;
    uint8_t al = e->regs[REG_AL_STATUS] & 0x0F;
    e->fault.op_delay_ms = d->cal.op_delay_ms;  /* survives a ctl "clear" of the fault block */
    /* 10.2: SAFEOP -> OP deferred by op_delay_ms (IS620N ~300 ms) */
    if (d->cal.op_delay_ms && al == ESM_SAFEOP && (e->regs[REG_AL_CONTROL] & 0x0F) == ESM_OP
        && !(e->regs[REG_AL_STATUS] & 0x10)) {
        d->op_since_ns += dt_ns;
        if (d->op_since_ns >= (uint64_t)d->cal.op_delay_ms * 1000000ull) {
            e->regs[REG_AL_STATUS] = ESM_OP; e->regs[REG_AL_STATUS + 1] = 0;
            e->regs[REG_AL_STATUS_CODE] = 0; e->regs[REG_AL_STATUS_CODE + 1] = 0;
            d->op_since_ns = 0;
            al = ESM_OP;
        }
    } else {
        d->op_since_ns = 0;
    }
    int op = al == ESM_OP;
    int safeop = al == ESM_SAFEOP || al == ESM_OP;
    d->dt_ns = dt_ns;
    for (int a = 0; a < d->axes; a++) od_pull(e, a);
    if (op) for_each_entry(e, 0, take_output);
    double dt = (double)dt_ns * 1e-9;
    for (int a = 0; a < d->axes; a++) {
        axis_step(e, a, op, dt, dt_ns, log);
        od_push(e, a);
    }
    if (safeop && !e->fault.stale_frame) for_each_entry(e, 1, give_input);
    d->was_op = (uint8_t)op;
}

void esc_cia402_step(esc_t *e, uint64_t now_ns, FILE *log)
{
    if (!e->drv) return;
    esc_cia402_step_dt(e, step_dt(e, now_ns), log);
}

/* ---- ctl --------------------------------------------------------------- */

static void print_axis(FILE *log, const esc_t *e, int a)
{
    const esc_cia402_axis_t *x = &e->drv->ax[a];
    fprintf(log, "cia402: node %d axis %d %s sw=0x%04X cw=0x%04X mode=%d(req %d) pos=%d vel=%.0f "
            "err=0x%04X ferr=%d steps=%llu transitions=%llu faults=%llu%s%s%s\n",
            e->position_in_chain, a, esc_cia402_ds_name(x->ds), x->sw, x->cw, x->mode, x->mode_req,
            pos_i32(x->pos), x->vel, x->err_code, x->ferr, (unsigned long long)x->steps,
            (unsigned long long)x->transitions, (unsigned long long)x->faults,
            x->inj_refuse_enable ? " [refuse_enable]" : "", x->inj_slow_ms ? " [slow]" : "",
            x->inj_latched ? " [latched]" : "");
}

void esc_cia402_print(const esc_t *e, FILE *log)
{
    if (!e->drv || !log) return;
    for (int a = 0; a < e->drv->axes; a++) print_axis(log, e, a);
    fflush(log);
}

static int parse_l(const char *s, long *v)
{
    char *end;
    if (!s) return -1;
    *v = strtol(s, &end, 0);
    return (end == s || *end) ? -1 : 0;
}

int esc_cia402_command(esc_t *chain, int n, int argc, char **argv, FILE *log, uint64_t now_ns)
{
    (void)now_ns;
    const char *cmd = argv[0];
    long node = -1, axis = -1, v = 0;
    if (!strcmp(cmd, "drv_status") && argc <= 2) {
        if (argc == 2 && (parse_l(argv[1], &node) || node < 0 || node >= n)) goto bad;
        for (int i = 0; i < n; i++)
            if ((node < 0 || i == node) && chain[i].drv) esc_cia402_print(&chain[i], log);
        return 0;
    }
    if (argc < 2 || parse_l(argv[1], &node) || node < 0 || node >= n || !chain[node].drv) {
        LOGD(log, "ERROR: '%s': node must be 0..%d and have --cia402", cmd, n - 1);
        return -1;
    }
    esc_t *e = &chain[node];
    esc_cia402_t *d = e->drv;
    if (!strcmp(cmd, "drv_lose_sync") && argc == 2) {
        for (int a = 0; a < d->axes; a++) d->ax[a].inj_fault_code = d->cal.code_sync;
        LOGD(log, "drv_lose_sync: node %ld, every axis -> fault 0x%04X", node, d->cal.code_sync);
        return 0;
    }
    int lo = 0, hi = d->axes - 1;
    if (argc >= 3 && strcmp(argv[2], "all") != 0) {
        if (parse_l(argv[2], &axis) || axis < 0 || axis >= d->axes) {
            LOGD(log, "ERROR: '%s': axis must be 0..%d or all", cmd, d->axes - 1);
            return -1;
        }
        lo = hi = (int)axis;
    } else if (argc < 3 && strcmp(cmd, "drv_clear") != 0) {
        goto bad;
    }
    for (int a = lo; a <= hi; a++) {
        esc_cia402_axis_t *x = &d->ax[a];
        if (!strcmp(cmd, "drv_fault") && (argc == 4 || argc == 5)) {
            if (parse_l(argv[3], &v) || v <= 0 || v > 0xFFFF) goto bad;
            if (argc == 5 && strcmp(argv[4], "latched")) goto bad;
            x->inj_fault_code = (uint16_t)v;
            x->inj_latched = argc == 5;
            LOGD(log, "drv_fault: node %ld axis %d fault 0x%04lX%s", node, a, v, x->inj_latched ? " latched" : "");
        } else if (!strcmp(cmd, "drv_clear") && argc <= 3) {
            x->inj_fault_code = 0; x->inj_latched = 0; x->inj_refuse_enable = 0;
            x->inj_slow_ms = 0; x->inj_ferr = 0; x->inj_quickstop = 0;
            LOGD(log, "drv_clear: node %ld axis %d", node, a);
        } else if (!strcmp(cmd, "drv_refuse_enable") && argc == 4) {
            if (parse_l(argv[3], &v) || (v != 0 && v != 1)) goto bad;
            x->inj_refuse_enable = (uint8_t)v;
            LOGD(log, "drv_refuse_enable: node %ld axis %d = %ld", node, a, v);
        } else if (!strcmp(cmd, "drv_slow") && argc == 4) {
            if (parse_l(argv[3], &v) || v < 0 || v > 600000) goto bad;
            x->inj_slow_ms = (uint32_t)v;
            LOGD(log, "drv_slow: node %ld axis %d transitions +%ld ms", node, a, v);
        } else if (!strcmp(cmd, "drv_ferr") && argc == 4) {
            if (parse_l(argv[3], &v)) goto bad;
            x->inj_ferr = (int32_t)v;
            LOGD(log, "drv_ferr: node %ld axis %d following error offset %ld", node, a, v);
        } else if (!strcmp(cmd, "drv_quickstop") && argc == 3) {
            x->inj_quickstop = 1;
            LOGD(log, "drv_quickstop: node %ld axis %d", node, a);
        } else if (!strcmp(cmd, "drv_pos") && argc == 4) {
            if (parse_l(argv[3], &v)) goto bad;
            x->pos = (double)v;
            LOGD(log, "drv_pos: node %ld axis %d actual position = %ld", node, a, v);
        } else if (!strcmp(cmd, "drv_home_switch") && argc == 4) {
            if (!strcmp(argv[3], "off")) {
                x->hm_has_switch = 0;
            } else {
                if (parse_l(argv[3], &v)) goto bad;
                x->hm_switch = (int32_t)v; x->hm_has_switch = 1;
            }
            LOGD(log, "drv_home_switch: node %ld axis %d %s", node, a, argv[3]);
        } else {
            goto bad;
        }
    }
    return 0;
bad:
    LOGD(log, "ERROR: unknown command or wrong arguments: '%s' (see esc_cia402.c)", cmd);
    return -1;
}
