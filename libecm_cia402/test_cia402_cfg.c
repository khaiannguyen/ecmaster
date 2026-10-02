/*
 * test_cia402_cfg.c -- Phase 10.3 axis configuration, offline.
 *
 *   K-01  object table per mode: each mode binds exactly the objects CiA 402
 *         needs; an axis that lacks one is refused naming the axis, the mode,
 *         the object and its direction (CSV without 0x60FF ...); a second
 *         mode needs 0x6060/0x6061 in the PDOs, one mode does not
 *   K-03o 0x6502 check: missing modes named with the 0x6502 value
 *   K-04o four axes on one slave (0x800 * n) bind the right bit offsets
 *   parse  "1:0,2:0", modes list, axes.cfg file (good and bad lines)
 */
#include "ecm_cia402_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
static void check(const char *name, long got, long want)
{
    int ok = got == want;
    printf("  [%s] %-70s = %ld%s\n", ok ? "PASS" : "FAIL", name, got, ok ? "" : " (unexpected)");
    if (ok) g_pass++; else g_fail++;
}
static void check_str(const char *name, const char *hay, const char *needle)
{
    int ok = strstr(hay, needle) != NULL;
    printf("  [%s] %-70s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : "");
    if (!ok) printf("         got: %s\n         want substring: %s\n", hay, needle);
    if (ok) g_pass++; else g_fail++;
}

static ecm_pdo_table_t T;
static ecm_pdo_loc_t L[ECM_PDO_MAX_SLAVES + 1];
static char err[1024];

/* slave 1 = P1 draft (0x1600/0x1A00), slave 2 = IS620N 0x1701/0x1B01,
 * slave 3 = 4-axis test drive (0x1600..0x1603 / 0x1A00..0x1A03) */
static void build(void)
{
    ecm_pdo_table_init(&T);
    ecm_pdo_add(&T, 1, ECM_PDO_OUT, 0x1600, 0x6040, 0, 16);
    ecm_pdo_add(&T, 1, ECM_PDO_OUT, 0x1600, 0x6060, 0, 8);
    ecm_pdo_add(&T, 1, ECM_PDO_OUT, 0x1600, 0x607A, 0, 32);
    ecm_pdo_add(&T, 1, ECM_PDO_OUT, 0x1600, 0x60FF, 0, 32);
    ecm_pdo_add(&T, 1, ECM_PDO_OUT, 0x1600, 0x6071, 0, 16);
    ecm_pdo_add(&T, 1, ECM_PDO_IN, 0x1A00, 0x6041, 0, 16);
    ecm_pdo_add(&T, 1, ECM_PDO_IN, 0x1A00, 0x6061, 0, 8);
    ecm_pdo_add(&T, 1, ECM_PDO_IN, 0x1A00, 0x6064, 0, 32);
    ecm_pdo_add(&T, 1, ECM_PDO_IN, 0x1A00, 0x606C, 0, 32);
    ecm_pdo_add(&T, 1, ECM_PDO_IN, 0x1A00, 0x6077, 0, 16);
    ecm_pdo_add(&T, 1, ECM_PDO_IN, 0x1A00, 0x603F, 0, 16);

    ecm_pdo_add(&T, 2, ECM_PDO_OUT, 0x1701, 0x6040, 0, 16);
    ecm_pdo_add(&T, 2, ECM_PDO_OUT, 0x1701, 0x607A, 0, 32);
    ecm_pdo_add(&T, 2, ECM_PDO_OUT, 0x1701, 0x60B8, 0, 16);
    ecm_pdo_add(&T, 2, ECM_PDO_OUT, 0x1701, 0x60FE, 1, 32);
    const uint16_t tx[9][2] = { { 0x603F, 16 }, { 0x6041, 16 }, { 0x6064, 32 }, { 0x6077, 16 }, { 0x60F4, 32 },
                                { 0x60B9, 16 }, { 0x60BA, 32 }, { 0x60BC, 32 }, { 0x60FD, 32 } };
    for (int k = 0; k < 9; k++) ecm_pdo_add(&T, 2, ECM_PDO_IN, 0x1B01, tx[k][0], 0, tx[k][1]);

    for (int a = 0; a < 4; a++) {
        uint16_t o = (uint16_t)(0x800 * a);
        ecm_pdo_add(&T, 3, ECM_PDO_OUT, (uint16_t)(0x1600 + a), (uint16_t)(0x6040 + o), 0, 16);
        ecm_pdo_add(&T, 3, ECM_PDO_OUT, (uint16_t)(0x1600 + a), (uint16_t)(0x6060 + o), 0, 8);
        ecm_pdo_add(&T, 3, ECM_PDO_OUT, (uint16_t)(0x1600 + a), (uint16_t)(0x607A + o), 0, 32);
        ecm_pdo_add(&T, 3, ECM_PDO_OUT, (uint16_t)(0x1600 + a), (uint16_t)(0x60FF + o), 0, 32);
    }
    for (int a = 0; a < 4; a++) {
        uint16_t o = (uint16_t)(0x800 * a);
        ecm_pdo_add(&T, 3, ECM_PDO_IN, (uint16_t)(0x1A00 + a), (uint16_t)(0x6041 + o), 0, 16);
        ecm_pdo_add(&T, 3, ECM_PDO_IN, (uint16_t)(0x1A00 + a), (uint16_t)(0x6061 + o), 0, 8);
        ecm_pdo_add(&T, 3, ECM_PDO_IN, (uint16_t)(0x1A00 + a), (uint16_t)(0x6064 + o), 0, 32);
        ecm_pdo_add(&T, 3, ECM_PDO_IN, (uint16_t)(0x1A00 + a), (uint16_t)(0x606C + o), 0, 32);
        ecm_pdo_add(&T, 3, ECM_PDO_IN, (uint16_t)(0x1A00 + a), (uint16_t)(0x603F + o), 0, 16);
    }
    /* all in one group; outputs/inputs one after another, like SOEM maps them */
    uint32_t ob = 0, ib = 0;
    for (int s = 1; s <= 3; s++) {
        L[s].group = 1; L[s].out_bit = ob; L[s].in_bit = ib;
        ob += T.bits[ECM_PDO_OUT][s]; ib += T.bits[ECM_PDO_IN][s];
    }
}

static ecm_axis_cfg_t ax1(int slave, int n, uint32_t modes)
{
    ecm_axis_cfg_t a;
    memset(&a, 0, sizeof(a));
    a.slave = (uint16_t)slave; a.n = (uint8_t)n; a.modes = modes;
    snprintf(a.name, sizeof(a.name), "%d:%d", slave, n);
    return a;
}

int main(void)
{
    build();
    ecm_axis_bind_t b;
    ecm_axis_cfg_t a;

    printf("[K-01 objects per mode]\n");
    a = ax1(1, 0, ECM_MODE_CSP);
    check("P1 CSP binds", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), 0);
    check("  cw 16 bit out at bit 0", b.cw.bits * 1000 + b.cw.dir * 100 + b.cw.bit, 16000);
    check("  target position at bit 24", b.tpos.bit, 24);
    check("  position actual (in) at bit 24", b.apos.bit, 24);
    check("  CSP does not bind 0x60FF", b.tvel.bits, 0);
    check("  0x6060 in the PDO -> mode by PDO", b.mode_by_sdo, 0);
    check("  0x603F (optional) bound", b.err.bits, 16);

    a = ax1(2, 0, ECM_MODE_CSP);
    check("IS620N 0x1701/0x1B01 CSP binds", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), 0);
    check("  no 0x6060 in 0x1701 -> mode set by SDO / InitCmd", b.mode_by_sdo, 1);
    check("  sw = 0x1B01 byte 2 (slave 2 inputs start after slave 1's 120 bits)", b.sw.bit, 120 + 16);

    a = ax1(2, 0, ECM_MODE_CSV);
    check("IS620N 0x1701 CSV refused", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), -1);
    check_str("  names axis, mode, object, direction", err, "axis 2:0 (slave 2): CSV needs 0x60FF:00 (output) in the process data");
    check_str("  lists what the slave maps", err, "slave 2 does not map 0x60FF:00; it maps:");

    a = ax1(2, 0, ECM_MODE_CSP | ECM_MODE_PP);
    check("IS620N 0x1701 CSP+PP (2 modes) refused: no 0x6060", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), -1);
    check_str("  says why 0x6060 is needed", err, "changing mode at run time (PP,CSP) needs 0x6060:00 (output)");

    a = ax1(1, 0, ECM_MODE_CSP | ECM_MODE_CSV | ECM_MODE_PP | ECM_MODE_PV | ECM_MODE_HM);
    check("P1 five modes bind (0x6060/0x6061 present)", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), 0);
    check("  0x6061 mode display bound", b.mode_disp.bits, 8);

    a = ax1(1, 0, ECM_MODE_CST);
    check("P1 CST binds 0x6071/0x6077", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)) == 0 && b.ttq.bits == 16 && b.atq.bits == 16, 1);

    a = ax1(2, 0, ECM_MODE_HM);
    check("IS620N HM only needs cw/sw/0x6064", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), 0);

    a = ax1(4, 0, ECM_MODE_CSP);
    check("slave without any PDO refused", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), -1);
    check_str("  on the controlword first", err, "every mode needs 0x6040:00 (output)");

    a = ax1(2, 1, ECM_MODE_CSP);
    check("IS620N axis 1 (0x6840) does not exist", ecm_axis_bind(&a, &T, L, &b, err, sizeof(err)), -1);
    check_str("  names 0x6840", err, "needs 0x6840:00");

    printf("[K-04o four axes on slave 3]\n");
    for (int n = 0; n < 4; n++) {
        char nm[96];
        a = ax1(3, n, ECM_MODE_CSP | ECM_MODE_CSV);
        int rc = ecm_axis_bind(&a, &T, L, &b, err, sizeof(err));
        uint32_t ob = L[3].out_bit + 88u * n, ib = L[3].in_bit + 104u * n;
        snprintf(nm, sizeof(nm), "axis 3:%d cw @%u, tpos @%u, sw @%u, avel @%u", n, ob, ob + 24, ib, ib + 56);
        check(nm, rc == 0 && b.cw.bit == ob && b.tpos.bit == ob + 24 && b.sw.bit == ib && b.avel.bit == ib + 56, 1);
    }
    a = ax1(3, 2, ECM_MODE_CSP);
    check("ecm_axis_index(3:2, 0x6041) = 0x7041", ecm_axis_index(&a, 0x6041), 0x7041);

    printf("[K-03o 0x6502]\n");
    a = ax1(3, 0, ECM_MODE_CSP | ECM_MODE_PP);
    check("0x1A5 supports CSP and PP", ecm_axis_check_modes(&a, 0x1A5, err, sizeof(err)), 0);
    check("0x1A4 (no PP) refused", ecm_axis_check_modes(&a, 0x1A4, err, sizeof(err)), -1);
    check_str("  names the missing mode, the object and its value", err,
              "axis 3:0 (slave 3): drive does not support PP (0x6502:00 supported drive modes = 0x000001A4: PV,HM,CSP,CSV)");
    a = ax1(3, 1, ECM_MODE_CSV);
    ecm_axis_check_modes(&a, 0x80, err, sizeof(err));
    check_str("  axis 1 reads 0x6D02", err, "0x6D02:00");

    printf("[parse]\n");
    uint32_t m = 0;
    check("modes 'csp,csv,PP'", ecm_axis_parse_modes("csp,csv,PP", &m, err, sizeof(err)) == 0 &&
          m == (ECM_MODE_CSP | ECM_MODE_CSV | ECM_MODE_PP), 1);
    check("mode 'cyclic' refused", ecm_axis_parse_modes("csp,cyclic", &m, err, sizeof(err)), -1);
    check_str("  named", err, "unknown mode 'cyclic'");
    ecm_axis_cfg_t ax[ECM_AXIS_MAX];
    int nax = 0;
    check("'1:0,2:0,3:3' -> 3 axes", ecm_axis_parse_list("1:0,2:0,3:3", ECM_MODE_CSP, ax, &nax, ECM_AXIS_MAX, err, sizeof(err)) == 0 && nax == 3, 1);
    check("  third is slave 3 axis 3, named '3:3'", ax[2].slave == 3 && ax[2].n == 3 && !strcmp(ax[2].name, "3:3"), 1);
    check("'2' -> slave 2 axis 0", ecm_axis_parse_list("2", ECM_MODE_CSP, ax, &nax, ECM_AXIS_MAX, err, sizeof(err)), -1);
    check_str("  (already given: refused as duplicate)", err, "axis 2:0 given twice");
    nax = 0;
    check("'1:8' refused (axis 0..7)", ecm_axis_parse_list("1:8", ECM_MODE_CSP, ax, &nax, ECM_AXIS_MAX, err, sizeof(err)), -1);
    check("'x:1' refused", ecm_axis_parse_list("x:1", ECM_MODE_CSP, ax, &nax, ECM_AXIS_MAX, err, sizeof(err)), -1);

    const char *cfg = "test_axes.cfg";
    FILE *f = fopen(cfg, "w");
    fprintf(f, "# test\naxis 1 modes csp,csv name left_wheel\n\naxis 2:0 modes csp   # IS620N\n");
    fclose(f);
    nax = 0;
    check("axes.cfg: 2 axes", ecm_axis_load_cfg(cfg, ax, &nax, ECM_AXIS_MAX, err, sizeof(err)) == 0 && nax == 2, 1);
    check("  name and modes of the first", !strcmp(ax[0].name, "left_wheel") && ax[0].modes == (ECM_MODE_CSP | ECM_MODE_CSV), 1);
    f = fopen(cfg, "w");
    fprintf(f, "axis 1 modes csp\naxis 2 mode csp\n");
    fclose(f);
    nax = 0;
    check("axes.cfg bad line refused", ecm_axis_load_cfg(cfg, ax, &nax, ECM_AXIS_MAX, err, sizeof(err)), -1);
    check_str("  with file:line", err, "test_axes.cfg:2:");
    remove(cfg);
    char ms[64];
    ecm_axis_modes_str(ECM_MODE_CSV | ECM_MODE_PP | ECM_MODE_HM, ms, sizeof(ms));
    check_str("modes string 'PP,HM,CSV'", ms, "PP,HM,CSV");

    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
