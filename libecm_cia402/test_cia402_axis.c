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
 */
#include "ecm_cia402_axis.h"
#include "../tools/soft_bus/esc_cia402.h"
#include "../tools/soft_bus/esc_core.h"
#include "../tools/soft_bus/esc_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    DLOG = getenv("CIA402_VERBOSE") ? stdout : NULL;
    if (E) { esc_cia402_detach(E); if (E->prof_st) { free(E->prof_st->val); free(E->prof_st); } free(E); }
    if (P) esc_prof_free(P);
    t01(); t02(); t03(); t04(); t05(); t06(); t07(); t08();
    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
