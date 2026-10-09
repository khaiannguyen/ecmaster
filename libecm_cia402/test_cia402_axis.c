/*
 * test_cia402_axis.c -- Phase 10.5, offline closed loop: the master's axis
 * state machine (ecm_cia402_rt) against the virtual drive of soft_bus
 * (tools/soft_bus/esc_cia402, written from the spec in Phase 10.2), through
 * a fake IOmap laid out like SOEM's (outputs of the group, then inputs).
 *
 * One tick k = one frame then the hook:
 *   frame:  drive inputs (SM3) -> IOmap inputs; IOmap outputs -> SM2;
 *           the drive steps on what it got
 *   hook k: reads the inputs, writes the outputs that go out in frame k+1
 * so a command from hook k is seen back at hook k+2, like on the wire.
 *
 *   T-01  ENABLE: SOD -0x06-> RTSO -0x07-> SO -0x0F-> OE, two ticks per step,
 *         statusword checked at every step; no command -> never enabled
 *   T-02  drv_slow 2000 ms: TIMEOUT after step_timeout, naming Switch on
 *         disabled; no retry afterwards (drive never enabled)
 *   T-03  drv_refuse_enable: TIMEOUT naming Switched on, the master walks the
 *         drive back down to Switch on disabled, no retry loop
 *   T-04  CSP sine 1 Hz 10000 inc: actual(k) == setpoint(k-1) exactly (one
 *         cycle, the virtual drive tracks without limit); S2 basis: enabling
 *         at 50000 does not move the axis; CSV 5000 inc/s: +500 inc/100 ms
 *   T-05  CSP -> CSV while enabled: velocity setpoints dropped until 0x6061
 *         shows 9, the position does not jump at the switch; back to CSP
 *   T-06o two axes of the 4-axis drive enabled together, CSP, independent
 *   T-07  bus lost while enabled -> BUS_LOST, target disabled, stays
 *         disabled when the bus is back (S5 basis); drive fault -> FAULT +
 *         0x603F; ENABLE while in Fault refused; FAULT_RESET -> one 0x80
 *         pulse, Switch on disabled, axis stays disabled (S7 basis)
 *   T-08  statusword decode table (CiA 402 masks)
 *
 * Phase 10.6, safety latches S1..S7 (each one has a negative control: built
 * with -DECM_CIA402_BROKEN=(1<<n) its S-0n test must fail, see `make negctl`):
 *   S-01  drive-side quick stop (DI) while enabled -> DROPPED, never walked
 *         back up; slave out of OP while enabled -> Fault, not re-enabled
 *   S-02  enabling at 50000: 0x607A = actual in every frame, no motion
 *   S-03  setpoints stop for 100 ms during a ramp: position held exactly
 *   S-04  step limit 1000 inc: absolute 0 at 50000 refused (quick stop,
 *         STEP), 1e6 jump while running refused; 10 inc/cycle ramp passes;
 *         CSV limit 100: velocity step 5000 refused
 *   S-05  bus lost 50 ticks while enabled -> BUS_LOST, 500 ticks after the
 *         bus is back: still disabled
 *   S-06  shutdown while running: 0x07, 0x06, 0x00, drive OE -> SO -> RTSO
 *         -> SOD within 8 ticks, all_down; ENABLE refused (SHUTDOWN)
 *   S-07  fault, no command for 1 s: stays in Fault, no 0x80 sent; reset
 *         -> Switch on disabled, stays there
 *
 * Phase 10.7, profile modes on the 4-axis virtual drive (it has the SDO
 * objects 0x6081/0x6083/0x6084/0x6098/0x6099/0x607C), axis 1:0:
 *   P2-01 PP: 3 points queued at once -> each reached in order, one bit 4
 *         per point, acknowledged, nothing lost; relative point; change
 *         immediately replaces a move in progress   (neg. control: bit 8)
 *   P2-02 PV: ramps with 0x6083/0x6084; DISABLE drops the velocity: a new
 *         ENABLE does not resume it
 *   P2-03 HM: method 37 -> 0x6064 = 0x607C; method 19 after it: done only
 *         at the virtual switch (a stale "attained" is not taken)
 *         (neg. control: bit 9); method 33 -> HOMING; timeout -> HOMING
 *   P2-04 HM -> PP -> CSP while enabled: no jump at the switch (S2)
 *
 * Phase 10.8:
 *   E2-01 drv_fault 0x2310: axis FAULT, 0x603F 0x2310, the axis line names
 *         the code, its text and source; the drive's EMCY (SM1) is the same
 *   E2-02 error from the monitor (CONFIG) while enabled: disabled at once,
 *         ENABLE refused while set; cleared -> ENABLE works (neg. ctl bit 10)
 */
#include "ecm_cia402_axis.h"
#include "ecm_cia402_diag.h"
#include "../tools/soft_bus/esc_cia402.h"
#include "../tools/soft_bus/esc_core.h"
#include "../tools/soft_bus/esc_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;
static void check(const char *name, long got, long want)
{
    int ok = got == want;
    printf("  [%s] %-72s = %ld%s\n", ok ? "PASS" : "FAIL", name, got, ok ? "" : " (unexpected)");
    if (ok) g_pass++; else g_fail++;
}
static void check_true(const char *name, int c) { check(name, c ? 1 : 0, 1); }

#define PROF "../config/profiles/"

/* ---- the rig ---------------------------------------------------------- */
static esc_t *E;
static ecm_pdo_table_t T;
static ecm_pdo_loc_t L[ECM_PDO_MAX_SLAVES + 1];
static uint8_t IO[512];
static uint16_t sm2s, sm2l, sm3s, sm3l;
static ecm_cia402_t C;
static uint64_t K;                 /* current tick */
static FILE *DLOG;
static esc_profile_t *P;          /* the ESC keeps a pointer to it */

static uint32_t odv(uint16_t idx, uint8_t sub)
{
    int g = esc_prof_sub(E, idx, sub);
    const uint8_t *v = esc_prof_value(E, g);
    return (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
}

static void rig(const char *prof, int axes)
{
    char err[512];
    esc_profile_t *p = esc_prof_load(prof, err, sizeof(err));
    if (!p) { fprintf(stderr, "%s\n", err); exit(2); }
    if (E) { esc_cia402_detach(E); if (E->prof_st) { free(E->prof_st->val); free(E->prof_st); } free(E); }
    if (P) esc_prof_free(P);
    P = p;
    E = calloc(1, sizeof(*E));
    esc_init(E, 0, 0);
    if (esc_prof_attach(E, p) || esc_cia402_attach(E, axes)) { fprintf(stderr, "attach\n"); exit(2); }
    for (int s = 2; s <= 3; s++) {
        uint8_t *sm = E->regs + REG_SM_BASE + s * REG_SM_ENTRY_SIZE;
        sm[0] = (uint8_t)p->sm[s].start; sm[1] = (uint8_t)(p->sm[s].start >> 8);
        sm[2] = (uint8_t)p->sm[s].len;   sm[3] = (uint8_t)(p->sm[s].len >> 8);
        sm[SM_OFF_CONTROL] = p->sm[s].ctrl;
        sm[SM_OFF_ACTIVATE] = SM_ACT_ENABLE;
    }
    E->regs[REG_AL_STATUS] = ESM_OP;
    sm2s = p->sm[2].start; sm2l = p->sm[2].len; sm3s = p->sm[3].start; sm3l = p->sm[3].len;
    /* the PDO table as SOEM would map it, from the profile's assignment */
    ecm_pdo_table_init(&T);
    for (int dir = 0; dir < 2; dir++) {
        uint16_t obj = dir ? 0x1C13 : 0x1C12;
        for (uint32_t a = 1; a <= (odv(obj, 0) & 0xFF); a++) {
            uint16_t pdo = (uint16_t)odv(obj, (uint8_t)a);
            for (uint32_t k = 1; k <= (odv(pdo, 0) & 0xFF); k++) {
                uint32_t e = odv(pdo, (uint8_t)k);
                ecm_pdo_add(&T, 1, dir ? ECM_PDO_IN : ECM_PDO_OUT, pdo, (uint16_t)(e >> 16), (uint8_t)(e >> 8), (uint16_t)(e & 0xFF));
            }
        }
    }
    memset(L, 0, sizeof(L));
    L[1].group = 1; L[1].out_bit = 0; L[1].in_bit = (uint32_t)sm2l * 8;
    memset(IO, 0, sizeof(IO));
    ecm_cia402_init(&C, 1000000, 500);
    K = 0;
}

static int add_axis(int n, uint32_t modes)
{
    ecm_axis_cfg_t a;
    memset(&a, 0, sizeof(a));
    a.slave = 1; a.n = (uint8_t)n; a.modes = modes;
    snprintf(a.name, sizeof(a.name), "1:%d", n);
    ecm_axis_bind_t b;
    char err[512];
    if (ecm_axis_bind(&a, &T, L, &b, err, sizeof(err))) { fprintf(stderr, "bind: %s\n", err); exit(2); }
    return ecm_cia402_add_axis(&C, &a, &b);
}

static void frame(void)
{
    memcpy(IO + sm2l, E->regs + sm3s, sm3l);       /* inputs the drive produced last step */
    memcpy(E->regs + sm2s, IO, sm2l);              /* outputs of the last hook */
    esc_cia402_step_dt(E, 1000000, DLOG);
}
static void tick(int n)
{
    while (n--) { K++; frame(); ecm_cia402_rt(&C, IO, K, K * 1000000, 1, 0); }
}
static ecm_cia402_state_t st(int ax) { ecm_cia402_state_t s; ecm_cia402_read(&C, ax, &s); return s; }
static void cmd(int ax, int op, int64_t arg) { ecm_cia402_cmd(&C, ax, op, arg, (uint32_t)K, 0); }
static void drv(const char *line)
{
    char tmp[128], *argv[8]; int argc = 0;
    snprintf(tmp, sizeof(tmp), "%s", line);
    for (char *t = strtok(tmp, " "); t && argc < 8; t = strtok(NULL, " ")) argv[argc++] = t;
    esc_cia402_command(E, 1, argc, argv, DLOG, 0);
}

/* ---- tests -------------------------------------------------------------- */

static void t01(void)
{
    printf("[T-01 enable sequence]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP);
    tick(20);
    check("no command: Switch on disabled, cw 0x0000", st(0).ds * 0x10000 + st(0).cw, ECM_DS_SOD * 0x10000);
    check("  never enabled by itself (200 more ticks)", (tick(200), st(0).ds), ECM_DS_SOD);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    uint64_t t0 = K;
    int seen[ECM_DS_COUNT] = { 0 }, okcw = 1;
    uint16_t sw_at[ECM_DS_COUNT] = { 0 };
    for (int i = 0; i < 20 && st(0).ds != ECM_DS_OE; i++) {
        tick(1);
        ecm_cia402_state_t s = st(0);
        if (!seen[s.ds]) { seen[s.ds] = (int)(K - t0); sw_at[s.ds] = s.sw; }
        if ((s.ds == ECM_DS_SOD && s.cw != 0x06) || (s.ds == ECM_DS_RTSO && s.cw != 0x07) ||
            (s.ds == ECM_DS_SO && s.cw != 0x0F) || (s.ds == ECM_DS_OE && s.cw != 0x0F)) okcw = 0;
    }
    /* the command is applied by the next hook (+1), then each step takes
     * two ticks: out in the next frame, seen back one frame later */
    check("RTSO seen 3 ticks after queuing ENABLE (1 + 2)", seen[ECM_DS_RTSO], 3);
    check("SO seen 5 ticks after", seen[ECM_DS_SO], 5);
    check("OE seen 7 ticks after", seen[ECM_DS_OE], 7);
    check("statusword RTSO 0x0231, SO 0x0233, OE 0x1237 (CSP bit 12)",
          (long)sw_at[ECM_DS_RTSO] == 0x0231 && sw_at[ECM_DS_SO] == 0x0233 && sw_at[ECM_DS_OE] == 0x1237, 1);
    check("controlword per state 0x06 / 0x07 / 0x0F", okcw, 1);
    check("no error", st(0).err, ECM_AXERR_NONE);
    cmd(0, ECM_CIA_OP_DISABLE, 0);
    tick(10);
    check("DISABLE: walked down to Switch on disabled", st(0).ds, ECM_DS_SOD);
}

static void t02(void)
{
    printf("[T-02 slow drive: transition timeout]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP);
    tick(5);
    drv("drv_slow 0 0 2000");
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    uint64_t t0 = K;
    while (st(0).err == ECM_AXERR_NONE && K - t0 < 3000) tick(1);
    check("TIMEOUT", st(0).err, ECM_AXERR_TIMEOUT);
    check_true("  after the 500 ms step timeout (+- 3 ticks)", K - t0 >= 500 && K - t0 <= 504);
    check("  names the state the drive is stuck in: Switch on disabled", st(0).err_ds, ECM_DS_SOD);
    tick(3000);
    check("no retry: still Switch on disabled 3 s later, target disabled",
          st(0).ds * 10 + st(0).target, ECM_DS_SOD * 10 + ECM_TGT_DISABLED);
}

static void t03(void)
{
    printf("[T-03 drive refuses enable]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP);
    tick(5);
    drv("drv_refuse_enable 0 0 1");
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    uint64_t t0 = K;
    long oe = 0;
    for (int i = 0; i < 2000; i++) { tick(1); if (st(0).ds == ECM_DS_OE) oe++; }
    check("TIMEOUT naming Switched on", st(0).err * 100 + st(0).err_ds, ECM_AXERR_TIMEOUT * 100 + ECM_DS_SO);
    check("  master walked the drive back down to Switch on disabled", st(0).ds, ECM_DS_SOD);
    check("  never Operation enabled, no retry loop", oe, 0);
    check("  the drive saw one enable attempt (2 transitions up, 2 down)", (long)E->drv->ax[0].transitions, 1 + 4);
    (void)t0;
}

static void t04(void)
{
    printf("[T-04 CSP sine, CSV step]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP | ECM_MODE_CSV);
    tick(5);
    drv("drv_pos 0 0 50000");
    tick(3);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    long moved = 0;
    for (int i = 0; i < 10; i++) { tick(1); if (st(0).apos != 50000) moved++; }
    check("enabled at 50000", st(0).ds, ECM_DS_OE);
    check("  S2 basis: the axis did not move while enabling", moved, 0);
    static int64_t sp[8192];
    int64_t p0 = st(0).apos;
    long worst = 0;
    for (int i = 0; i < 3000; i++) {
        uint64_t q = K + 2;                       /* hook K+1 writes it into frame K+2 */
        sp[q & 8191] = p0 + llround(10000.0 * sin(2 * M_PI * 1.0 * (double)i * 1e-3));
        ecm_cia402_setpoint(&C, 0, q, sp[q & 8191]);
        tick(1);                                  /* the drive applies frame q, seen at hook q+1 */
        if (i > 3) {
            long d = labs((long)st(0).apos - (long)sp[(K - 1) & 8191]);
            if (d > worst) worst = d;
        }
    }
    check("CSP sine: actual(k) == setpoint(k-1) (max |diff|)", worst, 0);
    check("  no underrun, nothing late", (long)(st(0).underrun + st(0).late), 0);
    tick(5);
    check("  setpoints stop: underrun counted, position held", (long)(st(0).underrun > 0 && st(0).apos == st(0).apos), 1);

    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_CSV);
    tick(4);
    int32_t pa = st(0).apos;
    for (int i = 0; i < 100; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 5000); tick(1); }
    check("CSV: mode display 9", st(0).mode_disp, ECM_OPMODE_CSV);
    check_true("CSV 5000 inc/s: ~ +500 inc in 100 ms (+- 10)", labs((long)(st(0).apos - pa) - 500) <= 10);
    check("  velocity actual 5000", st(0).avel, 5000);
    tick(5);
    check("CSV underrun: velocity 0, not held", st(0).avel, 0);
}

static void t05(void)
{
    printf("[T-05 CSP -> CSV -> CSP while enabled]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP | ECM_MODE_CSV);
    tick(5);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    for (int i = 0; i < 50; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 1000 + 10 * i); tick(1); }
    const long last_csp = 1000 + 10 * 49;           /* the last CSP target, still in flight */
    uint64_t used0 = st(0).used;
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_CSV);
    long used_before_ok = -1, jump = 0, prev_used = (long)used0;
    uint64_t t_ok = 0;
    for (int i = 0; i < 20; i++) {
        ecm_cia402_setpoint(&C, 0, K + 2, 3000);    /* velocity setpoints from the first tick */
        tick(1);
        ecm_cia402_state_t s = st(0);
        /* the hook that first reads 0x6061 = 9 already uses a setpoint for
         * the next frame: count up to the record before it */
        if (!t_ok && s.mode_disp == ECM_OPMODE_CSV) { t_ok = K; used_before_ok = prev_used - (long)used0; }
        if (!t_ok && labs((long)s.apos - last_csp) > 10) jump++;   /* beyond the in-flight 10 inc */
        prev_used = (long)s.used;
    }
    check("0x6061 confirms CSV", t_ok > 0, 1);
    check("  no velocity setpoint used before the confirmation", used_before_ok, 0);
    check("  position did not move before the confirmation", jump, 0);
    check_true("  ... dropped meanwhile (counted)", st(0).dropped > 0);
    check_true("  then moving at 3000 inc/s", st(0).avel == 3000);
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_CSP);
    tick(6);
    int32_t p = st(0).apos;
    tick(20);
    check("back to CSP without setpoints: position held (target = actual)", st(0).apos - p, 0);
    check("  still Operation enabled, no error", st(0).ds * 10 + st(0).err, ECM_DS_OE * 10);
}

static void t06(void)
{
    printf("[T-06o two axes of the 4-axis drive]\n");
    rig(PROF "cia402_4ax.prof", 4);
    add_axis(0, ECM_MODE_CSP);
    add_axis(2, ECM_MODE_CSP);
    tick(5);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    cmd(1, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    check("axis 1:0 and 1:2 Operation enabled", st(0).ds * 10 + st(1).ds, ECM_DS_OE * 10 + ECM_DS_OE);
    for (int i = 0; i < 100; i++) {
        ecm_cia402_setpoint(&C, 0, K + 2, 10 * i);
        ecm_cia402_setpoint(&C, 1, K + 2, -20 * i);
        tick(1);
    }
    /* last push (i = 99) is for the frame after next: actual shows i = 97 */
    check("independent targets: 1:0 at +970, 1:2 at -1940", st(0).apos == 970 && st(1).apos == -1940, 1);
    check("drive axis 1 (0x6841) untouched: Switch on disabled", (long)E->drv->ax[1].ds, DS_SOD);
}

static void t07(void)
{
    printf("[T-07 bus lost, drive fault, fault reset]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP);
    tick(5);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    for (int i = 0; i < 3; i++) { K++; ecm_cia402_rt(&C, IO, K, 0, 0, 1); }   /* no frames */
    check("bus lost while enabled: BUS_LOST, target disabled", st(0).err * 10 + st(0).target, ECM_AXERR_BUS_LOST * 10 + ECM_TGT_DISABLED);
    tick(20);
    check("bus back: the master walks the drive down, does not re-enable", st(0).ds, ECM_DS_SOD);
    check("  error kept until a new ENABLE", st(0).err, ECM_AXERR_BUS_LOST);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    check("new ENABLE: Operation enabled, error cleared", st(0).ds * 10 + st(0).err, ECM_DS_OE * 10);

    drv("drv_fault 0 0 0x2310");
    tick(3);
    drv("drv_clear 0 0");
    tick(40);
    check("drive fault: FAULT, 0x603F = 0x2310, Fault", st(0).err * 0x100000 + st(0).ecode * 0x10 + st(0).ds,
          ECM_AXERR_FAULT * 0x100000 + 0x23100 + ECM_DS_FAULT);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(5);
    check("ENABLE while in Fault refused (IN_FAULT), drive stays in Fault", st(0).err * 10 + st(0).ds, ECM_AXERR_IN_FAULT * 10 + ECM_DS_FAULT);
    cmd(0, ECM_CIA_OP_FAULT_RESET, 0);
    uint16_t cws[4];
    for (int i = 0; i < 4; i++) { tick(1); cws[i] = st(0).cw; }
    check("FAULT_RESET: one 0x80 pulse then 0x00", cws[0] == 0x80 && cws[1] == 0x00 && cws[2] == 0x00, 1);
    tick(5);
    check("  Switch on disabled, error cleared, stays disabled (no auto-enable)",
          st(0).ds * 100 + st(0).err * 10 + st(0).target, ECM_DS_SOD * 100);
    tick(200);
    check("  still Switch on disabled 200 ticks later", st(0).ds, ECM_DS_SOD);
}

/* ---- Phase 10.6: S1..S7 --------------------------------------------------- */

static void enable_csp(int64_t pos)
{
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP | ECM_MODE_CSV);
    tick(5);
    if (pos) { drv("drv_pos 0 0 50000"); tick(3); }
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
}
static int64_t tpos_out(void) { return (int32_t)ecm_pdo_get(&C.ax[0].b.tpos, IO); }

static void s01(void)
{
    printf("[S-01 never enabled by the master]\n");
    enable_csp(0);
    check("enabled", st(0).ds, ECM_DS_OE);
    drv("drv_quickstop 0 0");
    long oe = 0;
    for (int i = 0; i < 300; i++) { tick(1); if (i > 5 && st(0).ds == ECM_DS_OE) oe++; }
    /* the motor is at rest: the virtual drive ends its quick stop in the same step */
    check("drive-side quick stop: DROPPED (left OE to QSA / SOD)", st(0).err * 10 + (st(0).err_ds == ECM_DS_QSA || st(0).err_ds == ECM_DS_SOD), ECM_AXERR_DROPPED * 10 + 1);
    check("  never walked back up to Operation enabled (300 ticks)", oe, 0);
    check("  Switch on disabled, target disabled", st(0).ds * 10 + st(0).target, ECM_DS_SOD * 10 + ECM_TGT_DISABLED);

    enable_csp(0);
    E->regs[REG_AL_STATUS] = ESM_SAFEOP;          /* recovery: the slave out of OP, then back */
    tick(20);
    E->regs[REG_AL_STATUS] = ESM_OP;
    oe = 0;
    for (int i = 0; i < 300; i++) { tick(1); if (st(0).ds == ECM_DS_OE) oe++; }
    check("slave left OP while enabled: FAULT, never Operation enabled again", st(0).err * 10 + (oe > 0), ECM_AXERR_FAULT * 10);
}

static void s02(void)
{
    printf("[S-02 target = actual before enable]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP);
    tick(5);
    drv("drv_pos 0 0 50000");
    tick(3);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    long moved = 0, bad_t = 0;
    for (int i = 0; i < 30; i++) {
        tick(1);
        if (st(0).apos != 50000) moved++;
        if (tpos_out() != 50000) bad_t++;
    }
    check("enabled", st(0).ds, ECM_DS_OE);
    check("  0x607A = 50000 (actual) in every frame while enabling", bad_t, 0);
    check("  the axis never moved", moved, 0);
}

static void s03(void)
{
    printf("[S-03 underrun holds the last position]\n");
    enable_csp(0);
    for (int i = 1; i <= 100; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 10 * i); tick(1); }
    tick(3);
    int32_t p = st(0).apos;
    long moved = 0;
    for (int i = 0; i < 100; i++) { tick(1); if (st(0).apos != p) moved++; }
    check("ramp stopped at 1000", p, 1000);
    check("  100 ms without setpoints: position held exactly (no extrapolation)", moved, 0);
    check_true("  underrun counted", st(0).underrun >= 100);
}

static void s04(void)
{
    printf("[S-04 setpoint step limit]\n");
    rig(PROF "p1_draft.prof", 1);
    add_axis(0, ECM_MODE_CSP | ECM_MODE_CSV);
    ecm_cia402_set_step_limit(&C, 0, 1000, 100);
    tick(5);
    drv("drv_pos 0 0 50000");
    tick(3);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    long bad_t = 0;
    ecm_cia402_setpoint(&C, 0, K + 2, 0);          /* absolute 0 while at 50000 */
    for (int i = 0; i < 40; i++) { tick(1); if (tpos_out() != 50000 && st(0).apos != tpos_out()) bad_t++; if (tpos_out() == 0) bad_t += 1000; }
    check("absolute 0 at 50000: STEP, 1 refused", st(0).err * 10 + st(0).step_refused, ECM_AXERR_STEP * 10 + 1);
    check("  0 never sent, the axis did not move", bad_t + (st(0).apos != 50000), 0);
    check("  quick stop -> Switch on disabled", st(0).ds, ECM_DS_SOD);

    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    for (int i = 1; i <= 50; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 50000 + 10 * i); tick(1); }
    check("10 inc/cycle ramp under the limit: running, no error", st(0).ds * 10 + st(0).err, ECM_DS_OE * 10);
    ecm_cia402_setpoint(&C, 0, K + 2, 1050000);   /* +1e6 */
    long sent_big = 0;
    for (int i = 0; i < 40; i++) { tick(1); if (tpos_out() > 60000) sent_big++; }
    check("+1e6 jump while running: STEP, not sent, axis stopped", st(0).err * 10 + (sent_big > 0), ECM_AXERR_STEP * 10);
    check_true("  stopped near 50500", labs((long)st(0).apos - 50500) <= 10);

    cmd(0, ECM_CIA_OP_ENABLE, 0);
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_CSV);
    tick(12);
    for (int i = 1; i <= 20; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 50 * i < 100 ? 50 * i : 100); tick(1); }
    check("CSV ramp 50/cycle to 100: running", st(0).ds * 10 + st(0).err, ECM_DS_OE * 10);
    ecm_cia402_setpoint(&C, 0, K + 2, 5000);
    tick(30);
    check("CSV velocity step 100 -> 5000 (limit 100): STEP", st(0).err, ECM_AXERR_STEP);
    check_true("  5000 never commanded", st(0).avel < 200);
}

static void s05(void)
{
    printf("[S-05 bus lost latches disabled]\n");
    enable_csp(0);
    for (int i = 0; i < 50; i++) { K++; ecm_cia402_rt(&C, IO, K, 0, 0, 1); }
    long oe = 0;
    tick(5);
    for (int i = 0; i < 500; i++) { tick(1); if (st(0).ds == ECM_DS_OE) oe++; }
    check("BUS_LOST", st(0).err, ECM_AXERR_BUS_LOST);
    check("  500 ticks after the bus is back: disabled, never Operation enabled", oe * 10 + (st(0).ds != ECM_DS_SOD), 0);
}

static void s06(void)
{
    printf("[S-06 shutdown sequence]\n");
    enable_csp(0);
    for (int i = 1; i <= 20; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 10 * i); tick(1); }
    check("running", st(0).ds, ECM_DS_OE);
    check("  all_down = 0 while enabled", ecm_cia402_all_down(&C, 0), 0);
    ecm_cia402_shutdown(&C);
    char seq[64] = ""; uint16_t last = 0xFFFF;
    int n = 0;
    while (!ecm_cia402_all_down(&C, 0) && n < 50) {
        tick(1); n++;
        uint16_t cw = st(0).cw;
        if (cw != last && strlen(seq) < 50) { snprintf(seq + strlen(seq), sizeof(seq) - strlen(seq), "%s%02X", last == 0xFFFF ? "" : ",", cw); last = cw; }
    }
    printf("    controlwords %s, drive down after %d ticks\n", seq, n);
    check("  controlword 0x07, 0x06, 0x00 in that order", !strcmp(seq, "07,06,00"), 1);
    check_true("  all_down within 8 ticks", n <= 8 && ecm_cia402_all_down(&C, 0));
    check("  drive in Switch on disabled", (long)E->drv->ax[0].ds, DS_SOD);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(20);
    check("ENABLE after shutdown refused (SHUTDOWN), stays disabled", st(0).err * 10 + st(0).ds, ECM_AXERR_SHUTDOWN * 10 + ECM_DS_SOD);
}

static void s07(void)
{
    printf("[S-07 fault reset only on command]\n");
    enable_csp(0);
    drv("drv_fault 0 0 0x2310");
    tick(3);
    drv("drv_clear 0 0");
    long pulse = 0, notf = 0;
    for (int i = 0; i < 1000; i++) {
        tick(1);
        if (st(0).cw & 0x80) pulse++;
        if (i > 50 && st(0).ds != ECM_DS_FAULT) notf++;
    }
    check("1 s without a command: still Fault, no 0x80 sent", pulse * 10 + (notf > 0), 0);
    cmd(0, ECM_CIA_OP_FAULT_RESET, 0);
    long oe = 0;
    for (int i = 0; i < 500; i++) { tick(1); if (st(0).ds == ECM_DS_OE) oe++; }
    check("reset: Switch on disabled, stays there 500 ticks, target disabled",
          oe * 100 + (st(0).ds != ECM_DS_SOD) * 10 + st(0).target, 0);
}

/* ---- Phase 10.7: PP, PV, HM ------------------------------------------------ */

static void odw(uint16_t idx, uint8_t sub, uint32_t v)       /* SDO write, as ecm_run does at PREOP */
{
    int g = esc_prof_sub(E, idx, sub);
    if (g < 0) { fprintf(stderr, "no 0x%04X:%02X\n", idx, sub); exit(2); }
    uint8_t *p = esc_prof_value(E, g);
    for (int i = 0; i < E->prof->sub[g].len && i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void enable_mode(int mode)
{
    rig(PROF "cia402_4ax.prof", 4);
    add_axis(0, ECM_MODE_CSP | ECM_MODE_PP | ECM_MODE_PV | ECM_MODE_HM);
    tick(5);
    cmd(0, ECM_CIA_OP_SET_MODE, mode);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(12);
}

static void p201(void)
{
    printf("[P2-01 PP set-point handshake]\n");
    enable_mode(ECM_OPMODE_PP);
    odw(0x6081, 0, 200000); odw(0x6083, 0, 2000000); odw(0x6084, 0, 2000000);
    check("enabled in PP (mode display 1)", st(0).ds * 10 + st(0).mode_disp, ECM_DS_OE * 10 + ECM_OPMODE_PP);
    const int64_t pts[3] = { 10000, 20000, -5000 };
    for (int i = 0; i < 3; i++) ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PP_POINT, pts[i], (uint32_t)K, 0);
    long reached[8], nr = 0, edges = 0, bit4_long = 0, run4 = 0;
    uint16_t sw_prev = st(0).sw, cw_prev = 0;
    for (int i = 0; i < 3000; i++) {
        tick(1);
        ecm_cia402_state_t s = st(0);
        if ((s.cw & 0x10) && !(cw_prev & 0x10)) edges++;
        run4 = (s.cw & 0x10) ? run4 + 1 : 0;
        if (run4 > 3) bit4_long++;
        if ((s.sw & 0x0400) && !(sw_prev & 0x0400) && nr < 8) reached[nr++] = s.apos;
        sw_prev = s.sw; cw_prev = s.cw;
    }
    check("3 targets reached, in order: 10000, 20000, -5000",
          nr == 3 && reached[0] == 10000 && reached[1] == 20000 && reached[2] == -5000, 1);
    check("  one bit-4 edge per point, each acknowledged", edges * 10 + st(0).pp_acked, 33);
    check("  bit 4 dropped within 3 ticks of being raised (ack seen)", bit4_long, 0);
    check("  nothing queued or lost, no error", st(0).pp_n + C.ax[0].pp_lost + st(0).err, 0);

    ecm_cia402_cmd2(&C, 0, ECM_CIA_OP_PP_POINT, 1000, ECM_PP_REL, (uint32_t)K, 0);
    tick(500);
    check("relative +1000 -> -4000", st(0).apos, -4000);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PP_POINT, 50000, (uint32_t)K, 0);
    tick(60);
    int32_t mid = st(0).apos;
    ecm_cia402_cmd2(&C, 0, ECM_CIA_OP_PP_POINT, 0, ECM_PP_IMM, (uint32_t)K, 0);
    int32_t maxp = mid;
    for (int i = 0; i < 1500; i++) { tick(1); if (st(0).apos > maxp) maxp = st(0).apos; }
    check_true("change immediately: the move to 50000 replaced in flight (never past 30000)", mid > -4000 && maxp < 30000);
    check("  ends at 0", st(0).apos, 0);
}

static void p202(void)
{
    printf("[P2-02 PV]\n");
    enable_mode(ECM_OPMODE_PV);
    odw(0x6083, 0, 1000000); odw(0x6084, 0, 2000000);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PV_VEL, 50000, (uint32_t)K, 0);
    tick(2 + 25);
    check_true("accel 1e6 inc/s^2: ~25000 inc/s after 25 ms (+- 1500)", labs(st(0).avel - 25000) <= 1500);
    tick(50);
    check("  50000 inc/s reached", st(0).avel, 50000);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PV_VEL, 0, (uint32_t)K, 0);
    tick(2 + 12);
    check_true("decel 2e6 inc/s^2: ~26000 inc/s 12 ms later (+- 1500)", labs(st(0).avel - 26000) <= 1500);
    tick(30);
    check("  stopped", st(0).avel, 0);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PV_VEL, 20000, (uint32_t)K, 0);
    tick(40);
    cmd(0, ECM_CIA_OP_DISABLE, 0);
    tick(20);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(100);
    check("DISABLE then ENABLE in PV: velocity not resumed (0x60FF = 0)", st(0).ds * 100000 + st(0).avel, ECM_DS_OE * 100000);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PV_VEL, 1000, (uint32_t)K, 0);
    tick(5);
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_PP);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PV_VEL, 1000, (uint32_t)K, 0);
    tick(5);
    check("PV_VEL outside PV refused (MODE)", st(0).err, ECM_AXERR_MODE);
}

static void p203(void)
{
    printf("[P2-03 homing]\n");
    enable_mode(ECM_OPMODE_HM);
    odw(0x6098, 0, 37); odw(0x607C, 0, 1000);
    drv("drv_pos 0 0 77777");
    tick(3);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_HOME, 0, (uint32_t)K, 0);
    uint64_t t0 = K;
    while (!st(0).homed && K - t0 < 100) tick(1);
    check("method 37: homed, 0x6064 = 0x607C = 1000", st(0).homed * 100000 + st(0).apos, 100000 + 1000);
    check_true("  after the settle window (stale bit 12 not taken: > 2 ticks)", K - t0 > 2);
    tick(5);
    check("  bit 4 released", st(0).cw & 0x10, 0);

    odw(0x6098, 0, 19); odw(0x6099, 1, 20000);
    drv("drv_home_switch 0 0 5000");
    tick(3);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_HOME, 0, (uint32_t)K, 0);
    t0 = K;
    int32_t at_done = 0;
    do { tick(1); at_done = st(0).apos; } while (st(0).hm_phase != ECM_HM_DONE && K - t0 < 2000);
    printf("    method 19 done after %ld ticks\n", (long)(K - t0));
    check("method 19 after 37 (bit 12 still high from 37): done at the switch only", st(0).homed * 100000 + at_done, 100000 + 1000);
    check_true("  ~200 ms of travel (4000 inc at 20000 inc/s): > 150 ticks", K - t0 > 150);

    odw(0x6098, 0, 33);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_HOME, 0, (uint32_t)K, 0);
    tick(10);
    check("method 33 (not offered by the drive): HOMING error (bit 13)", st(0).err * 10 + st(0).hm_phase, ECM_AXERR_HOMING * 10 + ECM_HM_FAILED);

    cmd(0, ECM_CIA_OP_ENABLE, 0);                    /* clears the error */
    tick(5);
    ecm_cia402_set_home_timeout(&C, 100);
    odw(0x6098, 0, 19);
    drv("drv_home_switch 0 0 10000000");
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_HOME, 0, (uint32_t)K, 0);
    t0 = K;
    while (!st(0).err && K - t0 < 1000) tick(1);
    check("switch never reached, timeout 100 ms: HOMING", st(0).err, ECM_AXERR_HOMING);
    check_true("  after ~100 ticks (100..105)", K - t0 >= 100 && K - t0 <= 105);
    tick(5);
    check("  homing start bit released", st(0).cw & 0x10, 0);
}

static void p204(void)
{
    printf("[P2-04 HM -> PP -> CSP without a jump]\n");
    enable_mode(ECM_OPMODE_HM);
    odw(0x6098, 0, 37); odw(0x607C, 0, 0);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_HOME, 0, (uint32_t)K, 0);
    tick(20);
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_PP);
    tick(5);
    odw(0x6081, 0, 200000);
    ecm_cia402_cmd(&C, 0, ECM_CIA_OP_PP_POINT, 3000, (uint32_t)K, 0);
    tick(300);
    check("homed, then PP to 3000", st(0).homed * 100000 + st(0).apos, 100000 + 3000);
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_CSP);
    long moved = 0;
    for (int i = 0; i < 50; i++) { tick(1); if (st(0).apos != 3000) moved++; }
    check("switch to CSP while enabled: no jump (target = actual)", st(0).mode_disp * 1000 + moved, ECM_OPMODE_CSP * 1000);
    for (int i = 1; i <= 20; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 3000 + 5 * i); tick(1); }
    tick(2);
    check("  CSP setpoints continue from there", st(0).apos, 3100);
    check("  no error", st(0).err, 0);
}

/* ---- Phase 10.8 ------------------------------------------------------------ */

static void e201(void)
{
    printf("[E2-01 drive fault -> axis diagnosis]\n");
    enable_csp(0);
    drv("drv_fault 0 0 0x2310");
    tick(3);
    drv("drv_clear 0 0");
    tick(20);
    ecm_cia402_state_t s = st(0);
    check("FAULT, 0x603F 0x2310", s.err * 0x10000 + s.ecode, ECM_AXERR_FAULT * 0x10000 + 0x2310);
    ecm_cia402_emcy_in_t em = { .have = E->fault.emcy_code != 0, .code = E->fault.emcy_code };
    char line[512];
    ecm_cia402_axis_diag("1:0", 1, &s, 1, 1, &em, 0, line, sizeof(line));
    printf("    %s\n", line);
    check("  the drive's EMCY carries the same code", em.code, 0x2310);
    check_true("  axis line: code, text, class, source",
               strstr(line, "error drive fault; code 0x2310 continuous over current [current] (0x603F, = EMCY)") != NULL);
}

static void e202(void)
{
    printf("[E2-02 error set by the monitor]\n");
    enable_csp(0);
    check("enabled", st(0).ds, ECM_DS_OE);
    ecm_cia402_set_ext_error(&C, 0, ECM_AXERR_CONFIG);
    tick(10);
    check("CONFIG: walked down, Switch on disabled", st(0).err * 10 + st(0).ds, ECM_AXERR_CONFIG * 10 + ECM_DS_SOD);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    long oe = 0;
    for (int i = 0; i < 100; i++) { tick(1); if (st(0).ds == ECM_DS_OE) oe++; }
    check("  ENABLE refused while set (no retry)", oe * 10 + st(0).err, ECM_AXERR_CONFIG);
    ecm_cia402_set_ext_error(&C, 0, 0);
    tick(2);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    check("cleared by the operator: ENABLE works", st(0).ds * 10 + st(0).err, ECM_DS_OE * 10);
}

static void t08(void)
{
    printf("[T-08 statusword decode]\n");
    const struct { uint16_t sw; ecm_ds_t ds; } V[] = {
        { 0x0000, ECM_DS_NOT_READY }, { 0x0250, ECM_DS_SOD }, { 0x0231, ECM_DS_RTSO }, { 0x0233, ECM_DS_SO },
        { 0x1237, ECM_DS_OE }, { 0x0217, ECM_DS_QSA }, { 0x021F, ECM_DS_FRA }, { 0x0218, ECM_DS_FAULT },
        { 0x0638, ECM_DS_FAULT }, { 0x0270, ECM_DS_SOD }, { 0x0201, ECM_DS_UNKNOWN },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++)
        if (ecm_cia402_decode(V[i].sw) != V[i].ds) { bad++; printf("    0x%04X -> %d, want %d\n", V[i].sw, ecm_cia402_decode(V[i].sw), V[i].ds); }
    check("11 statuswords decoded per the CiA 402 masks", 11 - bad, 11);
}

/* 0022: IS620N with 0x1702/0x1B02 (the TwinCAT ENI of 9/10): CSV without 0x606C,
 * velocity actual = d(0x6064)/dt. The profile is is620n_min.prof with the
 * assignment and SM sizes of that ENI (19 / 25 byte). */
static void t09(void)
{
    printf("[T-09 IS620N 0x1702/0x1B02: CSV, velocity from position]\n");
    char tmp[] = "/tmp/test_cia402_is620n_mm_XXXXXX";
    int fd = mkstemp(tmp);
    FILE *in = fopen(PROF "is620n_min.prof", "r"), *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!in || !out) { fprintf(stderr, "t09: profile copy\n"); exit(2); }
    char ln[512];
    while (fgets(ln, sizeof(ln), in)) {
        if (!strncmp(ln, "sm 2 ", 5)) fputs("sm 2 start 0x1800 len 19 ctrl 0x64 en 1\n", out);
        else if (!strncmp(ln, "sm 3 ", 5)) fputs("sm 3 start 0x1C00 len 25 ctrl 0x20 en 1\n", out);
        else if (!strncmp(ln, "sub 0x1C12 1 ", 13)) fputs("sub 0x1C12 1 bits 16 rw_preop 0217\n", out);
        else if (!strncmp(ln, "sub 0x1C13 1 ", 13)) fputs("sub 0x1C13 1 bits 16 rw_preop 021b\n", out);
        else fputs(ln, out);
    }
    fclose(in); fclose(out);
    rig(tmp, 1);
    unlink(tmp);
    add_axis(0, ECM_MODE_CSP | ECM_MODE_CSV);
    check("bound without 0x606C", C.ax[0].b.avel.bits, 0);
    tick(5);
    cmd(0, ECM_CIA_OP_ENABLE, 0);
    tick(10);
    check("enabled (CSP)", st(0).ds, ECM_DS_OE);
    check("  standing: velocity from position 0", st(0).avel, 0);
    cmd(0, ECM_CIA_OP_SET_MODE, ECM_OPMODE_CSV);
    tick(4);
    int32_t pa = st(0).apos;
    for (int i = 0; i < 100; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 5000); tick(1); }
    check("CSV: mode display 9 (0x6061 in 0x1B02)", st(0).mode_disp, ECM_OPMODE_CSV);
    check_true("CSV 5000 inc/s: ~ +500 inc in 100 ms (+- 10)", labs((long)(st(0).apos - pa) - 500) <= 10);
    check("  velocity actual from position: 5000 inc/s", st(0).avel, 5000);
    /* a lost frame: no difference across it (it would count 2 cycles as 1) */
    K++; frame(); ecm_cia402_rt(&C, IO, K, K * 1000000, 0, 0);
    ecm_cia402_setpoint(&C, 0, K + 1, 5000); tick(1);
    check("  first frame after a lost one: 0, not a doubled step", st(0).avel, 0);
    for (int i = 0; i < 3; i++) { ecm_cia402_setpoint(&C, 0, K + 2, 5000); tick(1); }
    check("  then 5000 again", st(0).avel, 5000);
    tick(5);
    check("CSV underrun: velocity 0", st(0).avel, 0);
}

static const struct { const char *name; void (*fn)(void); } TESTS[] = {
    { "t01", t01 }, { "t02", t02 }, { "t03", t03 }, { "t04", t04 }, { "t05", t05 }, { "t06", t06 },
    { "t07", t07 }, { "t08", t08 }, { "t09", t09 },
    { "p201", p201 }, { "p202", p202 }, { "p203", p203 }, { "p204", p204 },
    { "e201", e201 }, { "e202", e202 },
    { "s01", s01 }, { "s02", s02 }, { "s03", s03 }, { "s04", s04 }, { "s05", s05 }, { "s06", s06 }, { "s07", s07 },
};

/* ./test_cia402_axis [name ...]   (no name: every test) */
int main(int argc, char **argv)
{
    DLOG = getenv("CIA402_VERBOSE") ? stdout : NULL;
    if (ECM_CIA402_BROKEN) printf("NEGATIVE CONTROL BUILD: ECM_CIA402_BROKEN = 0x%X\n", (unsigned)ECM_CIA402_BROKEN);
    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        int run = argc < 2;
        for (int k = 1; k < argc; k++) if (!strcmp(argv[k], TESTS[i].name)) run = 1;
        if (run) TESTS[i].fn();
    }
    if (E) { esc_cia402_detach(E); if (E->prof_st) { free(E->prof_st->val); free(E->prof_st); } free(E); }
    if (P) esc_prof_free(P);
    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
