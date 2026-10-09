/*
 * test_cia402.c -- Phase 10.2 virtual CiA402 drive, offline (no sockets).
 *
 *   A-01  state machine: every (state, command) pair against a table written
 *         from CiA 402 (not derived from esc_cia402.c), incl. commands that
 *         must leave the state unchanged; fault reset only on a rising bit 7
 *   A-02  motion: CSP follows a sine with one cycle of delay; CSV; PP
 *         trapezoid (peak = profile velocity, end exactly on target, set-point
 *         handshake, buffered set-point); PV ramp; homing 37 and 19
 *   A-03  injections: fault -> statusword bit 3 + EMCY + 0x603F, reset only
 *         on a rising edge, latched fault; refuse enable; slow transitions;
 *         following error; quick stop; leaving OP while enabled
 *   A-06  multi-axis (0x800 * a): 4 axes, independent state and objects
 *   A-07  IS620N mapping (0x1701 / 0x1B01, mode by SDO/InitCmd 0x6060)
 *
 * Everything runs through the node's DPRAM exactly as soft_bus does: the
 * test writes the outputs where SM2 points and reads the inputs where SM3
 * points, by walking the PDO mapping the profile assigns.
 */
#include "esc_cia402.h"
#include "esc_core.h"
#include "esc_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;
static void check(const char *name, long got, long want)
{
    int ok = got == want;
    printf("  [%s] %-66s = %ld%s\n", ok ? "PASS" : "FAIL", name, got, ok ? "" : " (unexpected)");
    if (ok) g_pass++; else g_fail++;
}
static void check_true(const char *name, int cond) { check(name, cond ? 1 : 0, 1); }

#ifndef PROF_DIR
#define PROF_DIR "../../config/profiles/"
#endif

static esc_t *g_e;
static FILE  *g_log;
static const uint64_t DT = 1000000;      /* 1 ms */

/* ---- a node as the master would have configured it ------------------- */

static esc_t *make_node(const char *prof, int axes)
{
    char err[512];
    esc_profile_t *p = esc_prof_load(prof, err, sizeof(err));
    if (!p) { fprintf(stderr, "%s\n", err); exit(2); }
    esc_t *e = calloc(1, sizeof(*e));
    esc_init(e, 0, 0);
    if (esc_prof_attach(e, p)) { fprintf(stderr, "attach failed\n"); exit(2); }
    if (esc_cia402_attach(e, axes)) { fprintf(stderr, "cia402 attach failed\n"); exit(2); }
    for (int s = 2; s <= 3; s++) {                         /* SM2/SM3 as in the SII */
        uint8_t *sm = e->regs + REG_SM_BASE + s * REG_SM_ENTRY_SIZE;
        sm[0] = (uint8_t)p->sm[s].start; sm[1] = (uint8_t)(p->sm[s].start >> 8);
        sm[2] = (uint8_t)p->sm[s].len;   sm[3] = (uint8_t)(p->sm[s].len >> 8);
        sm[SM_OFF_CONTROL] = p->sm[s].ctrl;
        sm[SM_OFF_ACTIVATE] = SM_ACT_ENABLE;
    }
    e->regs[REG_AL_STATUS] = ESM_OP;
    return e;
}

/* where index:sub is in SM2 (dir 0) / SM3 (dir 1), walking the assignment */
static int find(esc_t *e, int dir, uint16_t idx, uint8_t sub, uint32_t *bit, uint16_t *bits)
{
    uint16_t obj = dir ? 0x1C13 : 0x1C12;
    int g = esc_prof_sub(e, obj, 0);
    int n = esc_prof_value(e, g)[0];
    uint32_t b = 0;
    for (int a = 1; a <= n; a++) {
        const uint8_t *v = esc_prof_value(e, esc_prof_sub(e, obj, (uint8_t)a));
        uint16_t pdo = (uint16_t)(v[0] | v[1] << 8);
        int m = esc_prof_value(e, esc_prof_sub(e, pdo, 0))[0];
        for (int k = 1; k <= m; k++) {
            const uint8_t *q = esc_prof_value(e, esc_prof_sub(e, pdo, (uint8_t)k));
            uint32_t ent = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
            if ((ent >> 16) == idx && ((ent >> 8) & 0xFF) == sub) { *bit = b; *bits = (uint16_t)(ent & 0xFF); return 0; }
            b += ent & 0xFF;
        }
    }
    return -1;
}

static uint8_t *sm_area(esc_t *e, int s)
{
    const uint8_t *sm = e->regs + REG_SM_BASE + s * REG_SM_ENTRY_SIZE;
    return e->regs + (sm[0] | sm[1] << 8);
}

static void out(esc_t *e, uint16_t idx, int64_t v)
{
    uint32_t bit; uint16_t bits;
    if (find(e, 0, idx, 0, &bit, &bits)) { fprintf(stderr, "0x%04X not in the outputs\n", idx); exit(2); }
    uint8_t *b = sm_area(e, 2);
    for (uint16_t k = 0; k < bits; k++) {
        uint8_t m = (uint8_t)(1u << ((bit + k) % 8));
        if ((uint64_t)v & (1ull << k)) b[(bit + k) / 8] |= m; else b[(bit + k) / 8] &= (uint8_t)~m;
    }
}

static int64_t in(esc_t *e, uint16_t idx)
{
    uint32_t bit; uint16_t bits;
    if (find(e, 1, idx, 0, &bit, &bits)) { fprintf(stderr, "0x%04X not in the inputs\n", idx); exit(2); }
    const uint8_t *b = sm_area(e, 3);
    uint64_t v = 0;
    for (uint16_t k = 0; k < bits; k++)
        if (b[(bit + k) / 8] & (1u << ((bit + k) % 8))) v |= 1ull << k;
    if (bits < 64 && (v & (1ull << (bits - 1)))) v |= ~0ull << bits;   /* sign */
    return (int64_t)v;
}

static void step(esc_t *e) { esc_cia402_step_dt(e, DT, g_log); }
static void steps(esc_t *e, int k) { while (k--) step(e); }
static uint16_t sw(esc_t *e) { return (uint16_t)in(e, 0x6041); }
static uint16_t swst(esc_t *e) { return sw(e) & 0x6F; }

static void ctl(const char *line)
{
    char tmp[128], *argv[8]; int argc = 0;
    snprintf(tmp, sizeof(tmp), "%s", line);
    for (char *t = strtok(tmp, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    esc_cia402_command(g_e, 1, argc, argv, g_log, 0);
}

/* drive to Operation enabled with the standard sequence */
static void enable(esc_t *e)
{
    out(e, 0x6040, 0x06); step(e);
    out(e, 0x6040, 0x07); step(e);
    out(e, 0x6040, 0x0F); step(e);
}

/* ---- A-01 -------------------------------------------------------------- */

static void a01(void)
{
    printf("[A-01 state machine against the CiA 402 table]\n");
    enum { DV, QS, SD, SON, EO, FR, QS2, SD2, NCMD };
    const uint16_t cmd[NCMD] = { 0x00, 0x02, 0x06, 0x07, 0x0F, 0x80, 0x0B, 0x0E };
    const char *cname[NCMD] = { "disable voltage 0x00", "quick stop 0x02", "shutdown 0x06",
                                "switch on 0x07", "enable op 0x0F", "fault reset 0x80 (edge)",
                                "quick stop 0x0B", "shutdown 0x0E" };
    /* rows: SOD RTSO SO OE QSA(opt 2) QSA(opt 6) FAULT; written from the spec table */
    const cia402_ds_t want[7][NCMD] = {
        /* SOD  */ { DS_SOD, DS_SOD, DS_RTSO, DS_SOD, DS_SOD, DS_SOD, DS_SOD, DS_RTSO },
        /* RTSO */ { DS_SOD, DS_SOD, DS_RTSO, DS_SO, DS_SO, DS_RTSO, DS_SOD, DS_RTSO },
        /* SO   */ { DS_SOD, DS_SOD, DS_RTSO, DS_SO, DS_OE, DS_SO, DS_SOD, DS_RTSO },
        /* OE   */ { DS_SOD, DS_QSA, DS_RTSO, DS_SO, DS_OE, DS_OE, DS_QSA, DS_RTSO },
        /* QSA2 */ { DS_SOD, DS_QSA, DS_QSA, DS_QSA, DS_QSA, DS_QSA, DS_QSA, DS_QSA },
        /* QSA6 */ { DS_SOD, DS_QSA, DS_QSA, DS_QSA, DS_OE, DS_QSA, DS_QSA, DS_QSA },
        /* FLT  */ { DS_FAULT, DS_FAULT, DS_FAULT, DS_FAULT, DS_FAULT, DS_SOD, DS_FAULT, DS_FAULT },
    };
    const cia402_ds_t from[7] = { DS_SOD, DS_RTSO, DS_SO, DS_OE, DS_QSA, DS_QSA, DS_FAULT };
    const int16_t qopt[7] = { 2, 2, 2, 2, 2, 6, 2 };
    int bad = 0, total = 0;
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < NCMD; c++) {
            cia402_ds_t got = esc_cia402_next(from[r], cmd[c], 0x0000, qopt[r]);
            total++;
            if (got != want[r][c]) {
                bad++;
                printf("    %s + %s: got %s, spec %s\n", esc_cia402_ds_name(from[r]), cname[c],
                       esc_cia402_ds_name(got), esc_cia402_ds_name(want[r][c]));
            }
        }
    check("56 (state, command) pairs as the spec table", total - bad, 56);
    check("fault reset needs a rising edge (bit 7 already set: stays Fault)",
          esc_cia402_next(DS_FAULT, 0x80, 0x80, 2), DS_FAULT);
    check("Fault Reaction Active ignores every command", esc_cia402_next(DS_FRA, 0x80, 0x00, 2), DS_FRA);

    g_e = make_node(PROF_DIR "p1_draft.prof", 1);
    step(g_e);
    check("power-on -> Switch on disabled, statusword 0x0250 (SOD|VE|remote)", sw(g_e), 0x0250);
    out(g_e, 0x6040, 0x0F); steps(g_e, 5);
    check("S1 basis: 0x0F from Switch on disabled never enables (stays SOD)", swst(g_e), 0x40);
    out(g_e, 0x6040, 0x06); step(g_e);
    check("shutdown -> Ready to switch on (0x21)", swst(g_e), 0x21);
    out(g_e, 0x6040, 0x0F); step(g_e);
    check("0x0F from RTSO: Switched on after 1 cycle (0x23)", swst(g_e), 0x23);
    step(g_e);
    check("... Operation enabled after the 2nd (0x27)", swst(g_e), 0x27);
    out(g_e, 0x6040, 0x07); step(g_e);
    check("disable operation -> Switched on", swst(g_e), 0x23);
    out(g_e, 0x6040, 0x0F); step(g_e);
    out(g_e, 0x6040, 0x00); step(g_e);
    check("disable voltage from OE -> SOD", swst(g_e), 0x40);
    g_e->regs[REG_AL_STATUS] = ESM_SAFEOP;
    out(g_e, 0x6040, 0x06); steps(g_e, 3);
    check("SAFE-OP: outputs not valid, shutdown ignored (SOD)", swst(g_e), 0x40);
    g_e->regs[REG_AL_STATUS] = ESM_OP;
    step(g_e);
    check("back in OP: shutdown acts (RTSO)", swst(g_e), 0x21);
}

/* ---- A-02 -------------------------------------------------------------- */

static void a02(void)
{
    printf("[A-02 motion]\n");
    esc_t *e = g_e = make_node(PROF_DIR "p1_draft.prof", 1);
    step(e);
    out(e, 0x6060, MODE_CSP);
    enable(e);
    check("CSP: Operation enabled", swst(e), 0x27);
    check("CSP: mode display 8", in(e, 0x6061), 8);
    check("CSP: bit 12 (drive follows the target)", (sw(e) >> 12) & 1, 1);
    long maxlag = 0, worst = 0;
    int32_t prev = (int32_t)in(e, 0x6064);
    for (int k = 0; k < 2000; k++) {
        int32_t t = (int32_t)llround(10000.0 * sin(2 * M_PI * 1.0 * k * 1e-3));
        out(e, 0x607A, t);
        step(e);
        int32_t act = (int32_t)in(e, 0x6064);
        long d = labs((long)act - t);                 /* actual(k) vs target(k) */
        if (d > worst) worst = d;
        if (k > 0) { long lag = labs((long)prev - act); if (lag > maxlag) maxlag = lag; }
        prev = act;
    }
    check("CSP sine 1 Hz 10000 inc: actual == target after one drive cycle (max |diff|)", worst, 0);
    check_true("CSP: the master sees it one cycle later: step <= 2*pi*10000/1000 + 1", maxlag <= 64);

    out(e, 0x6060, MODE_CSV); out(e, 0x60FF, 10000); step(e);
    check("CSV: mode display 9", in(e, 0x6061), 9);
    int32_t p0 = (int32_t)in(e, 0x6064);
    steps(e, 100);
    check("CSV 10000 inc/s, unlimited accel: +1000 inc in 100 ms", (int32_t)in(e, 0x6064) - p0, 1000);
    check("CSV: velocity actual 10000", in(e, 0x606C), 10000);
    out(e, 0x60FF, 0); step(e);

    /* PP with the profile defaults (P1 has no 0x6081/0x6083/0x6084): 100000 inc/s, 1e6 inc/s^2 */
    out(e, 0x6060, MODE_PP); out(e, 0x6040, 0x0F); step(e);
    ctl("drv_pos 0 0 0");
    out(e, 0x607A, 50000); out(e, 0x6040, 0x1F); step(e);
    check("PP: set-point acknowledged (bit 12) while bit 4 high", (sw(e) >> 12) & 1, 1);
    out(e, 0x6040, 0x0F); step(e);
    check("PP: bit 12 drops with bit 4", (sw(e) >> 12) & 1, 0);
    int k = 2, reached = -1; long vpk = 0;
    for (; k < 2000; k++) {
        step(e);
        long v = labs((long)in(e, 0x606C));
        if (v > vpk) vpk = v;
        if (reached < 0 && (sw(e) & SW_TARGET)) reached = k;
    }
    check("PP: ends exactly on target 50000", in(e, 0x6064), 50000);
    check_true("PP: peak velocity = profile velocity 100000 (+-1)", labs(vpk - 100000) <= 1);
    check_true("PP: target reached at ~600 ms (0.5 s cruise + 0.1 s ramps, +-10)", reached >= 590 && reached <= 610);

    /* buffered set-point: second one while the first is moving (bit 5 = 0) */
    ctl("drv_pos 0 0 0");
    out(e, 0x607A, 20000); out(e, 0x6040, 0x1F); step(e);
    out(e, 0x6040, 0x0F); steps(e, 10);
    out(e, 0x607A, 30000); out(e, 0x6040, 0x1F); step(e);
    check("PP: buffered set-point acknowledged during the move", (sw(e) >> 12) & 1, 1);
    out(e, 0x6040, 0x0F); steps(e, 2000);
    check("PP: both set-points executed, ends on the second (30000)", in(e, 0x6064), 30000);

    /* relative */
    out(e, 0x607A, -5000); out(e, 0x6040, 0x5F); step(e);
    out(e, 0x6040, 0x4F); steps(e, 2000);
    check("PP relative (bit 6): 30000 - 5000 = 25000", in(e, 0x6064), 25000);

    /* PV */
    out(e, 0x6060, MODE_PV); out(e, 0x6040, 0x0F); out(e, 0x60FF, 50000); step(e);
    int tr = -1;
    for (k = 1; k < 200; k++) { step(e); if (tr < 0 && (sw(e) & SW_TARGET)) tr = k; }
    check_true("PV: 50000 inc/s reached after 50 ms at 1e6 inc/s^2 (+-2)", tr >= 48 && tr <= 52);
    out(e, 0x60FF, 0); steps(e, 100);
    check("PV: speed 0 indicator (bit 12) after stopping", (sw(e) >> 12) & 1, 1);

    /* HM 37 (P1 OD has no 0x6098/0x607C: method via the drive default would
     * be 0 -> homing error; set the method directly as an SDO would) */
    out(e, 0x6060, MODE_HM); out(e, 0x6040, 0x0F); step(e);
    e->drv->ax[0].hm_method = 37;    /* not in P1's OD; the 4-axis profile has it (A-06) */
    ctl("drv_pos 0 0 1234");
    out(e, 0x6040, 0x1F); step(e);
    check("HM 37: homing attained (bit 12) + target reached (bit 10)", (sw(e) >> 10) & 0x5, 0x5);
    check("HM 37: position = home offset 0", in(e, 0x6064), 0);
    out(e, 0x6040, 0x0F); step(e);
    e->drv->ax[0].hm_method = 33;    /* not offered here */
    out(e, 0x6040, 0x1F); step(e);
    check("HM 33 (not supported by the model): homing error bit 13", (sw(e) >> 13) & 1, 1);
    out(e, 0x6040, 0x0F); step(e);
    e->drv->ax[0].hm_method = 19;
    ctl("drv_pos 0 0 0");
    ctl("drv_home_switch 0 0 5000");
    out(e, 0x6040, 0x1F);
    int done = -1;
    for (k = 0; k < 2000; k++) { step(e); if (done < 0 && ((sw(e) >> 12) & 1)) done = k; }
    check_true("HM 19 with the virtual switch at 5000: attained", done > 0);
    check("HM 19: the switch edge is the new zero", in(e, 0x6064), 0);
}

/* ---- A-03 -------------------------------------------------------------- */

static void a03(void)
{
    printf("[A-03 injections]\n");
    esc_t *e = g_e = make_node(PROF_DIR "p1_draft.prof", 1);
    step(e);
    out(e, 0x6060, MODE_CSV);
    enable(e);
    out(e, 0x60FF, 20000); steps(e, 10);
    uint32_t emcy0 = e->fault.emcy_left;
    ctl("drv_fault 0 0 0x2310");
    step(e);
    check("fault while moving: Fault reaction active (0x0F)", swst(e), 0x0F);
    check("0x603F = 0x2310", in(e, 0x603F), 0x2310);
    check("one EMCY queued with that code", (long)(e->fault.emcy_left - emcy0) * 0x10000 + e->fault.emcy_code, 0x12310);
    steps(e, 50);
    check("after the fault reaction stop: Fault (0x08), velocity 0", swst(e) | (in(e, 0x606C) ? 0x100 : 0), 0x08);
    ctl("drv_clear 0 0");
    out(e, 0x6040, 0x80); step(e);
    check("fault reset (rising bit 7) -> Switch on disabled", swst(e), 0x40);
    check("0x603F cleared by the reset", in(e, 0x603F), 0);
    enable(e);
    out(e, 0x60FF, 0);
    ctl("drv_fault 0 0 0x5530");      /* not latched, but the cause stays until drv_clear */
    step(e);
    step(e);
    out(e, 0x6040, 0x80); step(e); steps(e, 5);
    check("cause still present: reset clears it once, drv_fault re-raises (Fault)", swst(e), 0x08);
    ctl("drv_clear 0 0");
    out(e, 0x6040, 0x00); step(e); out(e, 0x6040, 0x80); step(e);
    check("cause gone: the next rising edge resets", swst(e), 0x40);
    ctl("drv_fault 0 0 0x7500 latched");
    step(e);
    e->drv->ax[0].inj_fault_code = 0;          /* injection done, the fault itself latches */
    out(e, 0x6040, 0x00); step(e); out(e, 0x6040, 0x80); step(e);
    check("latched fault: reset refused, stays Fault", swst(e), 0x08);
    e->drv->ax[0].inj_latched = 0;
    out(e, 0x6040, 0x00); step(e); out(e, 0x6040, 0x80); step(e);
    check("latch released: reset works", swst(e), 0x40);

    ctl("drv_refuse_enable 0 0 1");
    out(e, 0x6040, 0x06); step(e); out(e, 0x6040, 0x07); step(e); out(e, 0x6040, 0x0F); steps(e, 20);
    check("drv_refuse_enable: stays Switched on", swst(e), 0x23);
    ctl("drv_refuse_enable 0 0 0");
    step(e);
    check("refusal lifted: Operation enabled", swst(e), 0x27);

    out(e, 0x6040, 0x00); step(e);
    ctl("drv_slow 0 0 5");
    out(e, 0x6040, 0x06);
    int k = 0;
    while (swst(e) != 0x21 && k < 50) { step(e); k++; }
    check("drv_slow 5 ms at 1 ms: shutdown takes 6 cycles (1 + 5)", k, 6);
    ctl("drv_slow 0 0 0");

    out(e, 0x6060, MODE_CSP); out(e, 0x6040, 0x07); step(e); out(e, 0x6040, 0x0F); step(e);
    e->drv->ax[0].ferr_window = 100;
    ctl("drv_ferr 0 0 500");
    step(e);
    check("following error 500 > window 100: fault 0x8611", in(e, 0x603F) & 0xFFFF, 0x8611);
    ctl("drv_clear 0 0");
    e->drv->ax[0].ferr_window = 0xFFFFFFFFu;
    out(e, 0x6040, 0x00); step(e); out(e, 0x6040, 0x80); step(e);
    enable(e);
    ctl("drv_quickstop 0 0");
    step(e);
    check("drv_quickstop at standstill: Quick stop active, then (option 2) SOD", swst(e), 0x40);

    out(e, 0x6040, 0x00); step(e);
    enable(e);
    e->regs[REG_AL_STATUS] = ESM_SAFEOP;
    step(e);
    check("ESM left OP while enabled: Fault, 0x603F = 0x8700 (default, R-07 calibrates)",
          swst(e) * 0x10000 + (in(e, 0x603F) & 0xFFFF), 0x08 * 0x10000 + 0x8700);
    e->regs[REG_AL_STATUS] = ESM_OP;
    out(e, 0x6040, 0x00); step(e); out(e, 0x6040, 0x80); step(e);
    out(e, 0x6040, 0x06); step(e);
    e->regs[REG_AL_STATUS] = ESM_SAFEOP; step(e);
    check("ESM left OP while Ready to switch on: SOD, no fault", swst(e), 0x40);

    e->regs[REG_AL_STATUS] = ESM_OP;
    out(e, 0x6060, 4);    /* torque mode: not in 0x6502 of the model */
    step(e);
    check("unsupported mode 4: mode display unchanged, warning bit 7", ((sw(e) >> 7) & 1) * 100 + in(e, 0x6061), 100 + MODE_CSP);

    e->fault.stale_frame = 1;
    uint8_t snap[64]; memcpy(snap, sm_area(e, 3), 21);
    out(e, 0x6040, 0x06); step(e);
    check("TxPDO frozen (stale injection): inputs untouched", memcmp(snap, sm_area(e, 3), 21), 0);
    e->fault.stale_frame = 0;
}

/* ---- A-06 / A-07 ------------------------------------------------------- */

static void a06(void)
{
    printf("[A-06 four axes, objects + 0x800 per axis]\n");
    esc_t *e = g_e = make_node(PROF_DIR "cia402_4ax.prof", 4);
    step(e);
    for (int a = 0; a < 4; a++) out(e, (uint16_t)(0x6060 + 0x800 * a), MODE_CSP);
    /* axis 0 and 2 enabled, 1 ready, 3 untouched */
    for (int a = 0; a < 3; a++) out(e, (uint16_t)(0x6040 + 0x800 * a), 0x06);
    step(e);
    out(e, 0x6040, 0x0F); out(e, 0x7040, 0x0F); steps(e, 2);
    check("axis 0 Operation enabled", in(e, 0x6041) & 0x6F, 0x27);
    check("axis 1 Ready to switch on", in(e, 0x6841) & 0x6F, 0x21);
    check("axis 2 Operation enabled", in(e, 0x7041) & 0x6F, 0x27);
    check("axis 3 Switch on disabled", in(e, 0x7841) & 0x6F, 0x40);
    out(e, 0x607A, 111); out(e, 0x707A, -222); out(e, 0x687A, 333); step(e);
    check("axis 0 follows its own target (111)", in(e, 0x6064), 111);
    check("axis 2 follows its own target (-222)", in(e, 0x7064), -222);
    check("axis 1 not enabled: no motion", in(e, 0x6864), 0);
    check("profile accel 0x6083 = 1e6 in the OD does not slow CSP (cyclic mode)", in(e, 0x6064) == 111, 1);
    ctl("drv_fault 0 2 0x2220");
    step(e);
    check("fault on axis 2 (moving) only: axis 2 Fault reaction active, axis 0 still enabled",
          (in(e, 0x7041) & 0x6F) * 0x100 + (in(e, 0x6041) & 0x6F), 0x0F27);
    steps(e, 30);
    check("axis 2 stopped by its fault reaction: Fault; axis 0 still enabled",
          (in(e, 0x7041) & 0x6F) * 0x100 + (in(e, 0x6041) & 0x6F), 0x0827);
    check("axis 2 0x703F = 0x2220", in(e, 0x703F), 0x2220);
    uint32_t v = 0;
    int g = esc_prof_sub(e, 0x7041, 0);
    const uint8_t *p = esc_prof_value(e, g); v = (uint32_t)(p[0] | p[1] << 8);
    check("SDO view (OD 0x7041) mirrors the statusword", (long)(v & 0x6F), 0x08);
}

static void a07(void)
{
    printf("[A-07 IS620N mapping: 0x1701 / 0x1B01, mode set like the ENI InitCmd]\n");
    esc_t *e = g_e = make_node(PROF_DIR "is620n_min.prof", 1);
    int g = esc_prof_sub(e, 0x6060, 0);
    esc_prof_value(e, g)[0] = MODE_CSP;           /* InitCmd PS 0x6060:00 = 8 */
    step(e);
    check("mode from the OD (no 0x6060 in 0x1701): mode display 8", e->drv->ax[0].mode, 8);
    out(e, 0x6040, 0x06); step(e); out(e, 0x6040, 0x07); step(e); out(e, 0x6040, 0x0F); step(e);
    check("IS620N mapping: Operation enabled through 0x1701/0x1B01", in(e, 0x6041) & 0x6F, 0x27);
    out(e, 0x607A, 4242); step(e);
    check("0x6064 at byte 4 of 0x1B01 follows 0x607A at byte 2 of 0x1701", in(e, 0x6064), 4242);
    check("0x603F (byte 0 of 0x1B01) = 0", in(e, 0x603F), 0);
}

/* ---- A-10 (10.2): calibration against the real IS620N ------------------- */

/* is620n_min.prof with the PDOs of eni_2servo_mm (0x1702/0x1B02, 19/25 byte):
 * 0x60FF and 0x6060 in the outputs, as on the real drives (X-05b) */
static esc_t *cal_node(void)
{
    char tmp[] = "/tmp/test_cia402_is620n_mm_XXXXXX";
    int fd = mkstemp(tmp);
    FILE *in = fopen(PROF_DIR "is620n_min.prof", "r"), *o = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!in || !o) { fprintf(stderr, "profile copy\n"); exit(2); }
    char ln[512];
    while (fgets(ln, sizeof(ln), in)) {
        if (!strncmp(ln, "sm 2 ", 5)) fputs("sm 2 start 0x1800 len 19 ctrl 0x64 en 1\n", o);
        else if (!strncmp(ln, "sm 3 ", 5)) fputs("sm 3 start 0x1C00 len 25 ctrl 0x20 en 1\n", o);
        else if (!strncmp(ln, "sub 0x1C12 1 ", 13)) fputs("sub 0x1C12 1 bits 16 rw_preop 0217\n", o);
        else if (!strncmp(ln, "sub 0x1C13 1 ", 13)) fputs("sub 0x1C13 1 bits 16 rw_preop 021b\n", o);
        else fputs(ln, o);
    }
    fclose(in); fclose(o);
    esc_t *e = g_e = make_node(tmp, 1);
    unlink(tmp);
    char err[256];
    if (esc_cia402_cal_load(&e->drv->cal, PROF_DIR "is620n.cal", err, sizeof(err))) { fprintf(stderr, "%s\n", err); exit(2); }
    esc_cia402_reset(e);
    out(e, 0x6060, MODE_CSP);
    out(e, 0x607F, 0);
    steps(e, 2);                                    /* power-on: Not ready -> Switch on disabled */
    return e;
}

static void enable_cal(esc_t *e)
{
    out(e, 0x6040, 0x06); steps(e, 3);
    out(e, 0x6040, 0x07); steps(e, 3);
    out(e, 0x6040, 0x0F); steps(e, 3);
}

static void a10(void)
{
    printf("[A-10 calibration: config/profiles/is620n.cal]\n");
    {
        char err[256];
        esc_cia402_cal_t c;
        esc_cia402_cal_default(&c);
        check("is620n.cal loads", esc_cia402_cal_load(&c, PROF_DIR "is620n.cal", err, sizeof(err)), 0);
        check("  code_lost_op 0x0E08 (hex accepted)", c.code_lost_op, 0x0E08);
        check("  SAFEOP->OP 300 ms", c.op_delay_ms, 300);
        FILE *f = fopen("/tmp/test_cia402_bad.cal", "w");
        fputs("csp_lag_ms 26\nbogus 1\n", f); fclose(f);
        check("unknown key refused", esc_cia402_cal_load(&c, "/tmp/test_cia402_bad.cal", err, sizeof(err)), -1);
        check_true("  names file:line and key", strstr(err, ":2: unknown key 'bogus'") != NULL);
        unlink("/tmp/test_cia402_bad.cal");
        esc_cia402_cal_default(&c);
        check("defaults unchanged: no CSP lag, lost-OP with EMCY", (long)(c.csp_lag_ms * 10) + c.lost_op_emcy, 1);
    }

    /* enable timing: 2 cycles per transition (X-05: 1.5-2.5 ms) */
    esc_t *e = cal_node();
    out(e, 0x6040, 0x06); step(e);
    check("trans_cycles 2: Ready to switch on not after 1 cycle", swst(e) == 0x21, 0);
    step(e);
    check("  after 2 cycles", swst(e), 0x21);
    out(e, 0x6040, 0x07); steps(e, 2); out(e, 0x6040, 0x0F); steps(e, 2);
    check("Operation enabled after 6 cycles", swst(e), 0x27);
    steps(e, 5);
    check("statusword standing in CSP OE = 0x1637 (X-05)", sw(e), 0x1637);

    /* CSP lag on the W-02 trajectory: (1 - cos) of 1 rev at 0.2 Hz, the
     * master's measure max |actual(k) - setpoint(k-1)|: 139 640 on the real drive */
    int32_t p0 = (int32_t)in(e, 0x6064), t = p0;
    long lag = 0, prev = p0;
    for (int k = 1; k <= 5000; k++) {
        t = p0 + (int32_t)llround(8388608.0 * 0.5 * (1.0 - cos(2.0 * M_PI * 0.2 * k * 1e-3)));
        out(e, 0x607A, t); step(e);
        long d = labs((long)in(e, 0x6064) - prev);
        if (k > 8 && d > lag) lag = d;
        prev = t;
        if (k == 1250) check_true("  following at peak velocity: bit 10 off, bit 12 on", (sw(e) & 0x1400) == 0x1000);
    }
    check_true("CSP W-02 trajectory: tracking max 139 640 +- 5 % (real IS620N)", labs(lag - 139640) <= 6982);
    printf("    (tracking max %ld inc)\n", lag);
    for (int k = 0; k < 300; k++) step(e);           /* setpoint stands: converges */
    check_true("  converges to the setpoint within 1 000 inc after 300 ms", labs((long)t - (long)in(e, 0x6064)) <= 1000);
    check("  standing again: 0x1637", sw(e), 0x1637);

    /* disable: bit 10 kept (0x0631 after OE -06-> RTSO in X-05) */
    out(e, 0x6040, 0x06); steps(e, 3);
    check("after Shutdown from OE: 0x0631 (bit 10 kept)", sw(e), 0x0631);
    enable_cal(e);
    check("enable again: OE", swst(e), 0x27);

    /* CSV velocity scale */
    out(e, 0x6060, MODE_CSV);
    out(e, 0x60FF, 1000000);
    steps(e, 3);
    int32_t c0 = (int32_t)in(e, 0x6064);
    steps(e, 1000);
    long mv = (long)in(e, 0x6064) - c0;
    check_true("CSV 1 000 000 inc/s for 1 s: 996 300 +- 1 000 (x0.9963, W-09csv)", labs(mv - 996300) <= 1000);

    /* Disable operation while moving: brakes in OE (W-07/W-08) */
    out(e, 0x60FF, 2000000);
    steps(e, 50);
    out(e, 0x6040, 0x07); steps(e, 50);
    check("Disable operation at 2e6 inc/s: still Operation enabled 50 ms later (braking)", swst(e), 0x27);
    check_true("  slower than 2e6 inc/s", e->drv->ax[0].vel < 1.6e6);
    steps(e, 250);
    check("  stopped (1e7 inc/s^2: 200 ms), then Switched on", swst(e), 0x23);
    check("  velocity 0", (long)e->drv->ax[0].vel, 0);

    /* process data lost in Ready to switch on: 0x0E08, no EMCY, self-clearing */
    out(e, 0x6040, 0x06); steps(e, 3);
    check("Ready to switch on", swst(e), 0x21);
    uint32_t emcy0 = e->fault.emcy_left;
    e->regs[REG_AL_STATUS] = ESM_SAFEOP | 0x10;
    e->regs[REG_AL_STATUS_CODE] = 0x1B; e->regs[REG_AL_STATUS_CODE + 1] = 0;
    step(e);
    check("SM watchdog (AL 0x001B) in RTSO: Fault", e->drv->ax[0].ds, DS_FAULT);
    check("  0x603F 0x0E08", e->drv->ax[0].err_code, 0x0E08);
    check("  no EMCY posted (IS620N)", (long)(e->fault.emcy_left - emcy0), 0);
    e->regs[REG_AL_STATUS] = ESM_OP; e->regs[REG_AL_STATUS_CODE] = 0;
    out(e, 0x6040, 0x06);                           /* the master keeps Shutdown, no bit 7 */
    step(e);
    check("back in OP: Not ready, sw 0x0210, 0x603F 0 (cleared by the drive)", sw(e), 0x0210);
    check("  0x603F 0", in(e, 0x603F), 0);
    steps(e, 45);
    check("  ~40 ms later Ready to switch on (cw 0x06 still there)", swst(e), 0x21);

    /* left OP while enabled: 0x0E08, no EMCY, self-clearing back in OP */
    out(e, 0x60FF, 0);
    out(e, 0x6060, MODE_CSP);
    out(e, 0x607A, in(e, 0x6064));                  /* S2: target = actual before enabling */
    enable_cal(e);
    steps(e, 3);
    check("enabled again, standing", swst(e), 0x27);
    emcy0 = e->fault.emcy_left;
    e->regs[REG_AL_STATUS] = ESM_SAFEOP | 0x10; e->regs[REG_AL_STATUS_CODE] = 0x1B;
    steps(e, 3);
    check("left OP in OE: Fault 0x0E08 (W-06)", e->drv->ax[0].err_code * 100 + e->drv->ax[0].ds, 0x0E08 * 100 + DS_FAULT);
    check("  no EMCY", (long)(e->fault.emcy_left - emcy0), 0);
    e->regs[REG_AL_STATUS] = ESM_OP; e->regs[REG_AL_STATUS_CODE] = 0;
    out(e, 0x6040, 0x00); steps(e, 45);
    check("  cleared by the drive, master sent no reset: Switch on disabled", swst(e) & 0x4F, 0x40);

    /* injected faults are not self-clearing */
    ctl("drv_fault 0 0 0x2310");
    steps(e, 3);
    check("drv_fault 0x2310: Fault, not cleared in OP", e->drv->ax[0].ds * 0x10000 + e->drv->ax[0].err_code, DS_FAULT * 0x10000 + 0x2310);
    ctl("drv_clear 0 0");

    /* SAFEOP -> OP takes 300 ms */
    e->regs[REG_AL_STATUS] = ESM_SAFEOP; e->regs[REG_AL_STATUS_CODE] = 0;
    e->got_valid_outputs = 1;
    e->regs[REG_AL_CONTROL] = ESM_OP; e->regs[REG_AL_CONTROL + 1] = 0;
    esc_al_control_write(e);
    check("AL Control OP: status stays SAFEOP at once", e->regs[REG_AL_STATUS] & 0x0F, ESM_SAFEOP);
    steps(e, 290);
    check("  still SAFEOP after 290 ms", e->regs[REG_AL_STATUS] & 0x0F, ESM_SAFEOP);
    steps(e, 15);
    check("  OP after 300 ms", e->regs[REG_AL_STATUS] & 0x0F, ESM_OP);
    e->regs[REG_AL_STATUS] = ESM_PREOP;
    e->regs[REG_AL_CONTROL] = ESM_PREOP;
    steps(e, 400);
    check("  no OP on its own when PREOP is asked", e->regs[REG_AL_STATUS] & 0x0F, ESM_PREOP);
}

int main(void)
{
    g_log = getenv("CIA402_VERBOSE") ? stdout : NULL;
    a01();
    a02();
    a03();
    a06();
    a07();
    a10();
    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
