/* ==========================================================================
 * test_profile.c — Phase 9.9: soft_bus profile nodes (esc_profile.c), offline.
 *
 * A node takes a profile generated from an ESI (config/profiles/NAME.prof) and
 * must then look like that slave: SII identity / mailbox / PDO categories,
 * the ESI's CoE dictionary over SDO, 0x1C12/0x1C13 rules (direction, SI0 = 0
 * before SIk, ESI Exclude, PREOP only, Complete Access as a whole), and
 * PREOP -> SAFEOP refused with 0x001D / 0x001E when the SM2/SM3 sizes the
 * master wrote do not match the assigned PDOs.
 *
 * Build & run:  make test_profile && ./test_profile [PROFILE_DIR [FULL_IS620N_PROFILE]]
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_core.h"
#include "esc_coe.h"
#include "esc_fault.h"
#include "esc_profile.h"

static int g_pass = 0, g_fail = 0;

static void check(const char *name, long got, long want)
{
    if (got == want) { printf("  [PASS] %-62s = %ld\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-62s = %ld (expected %ld)\n", name, got, want); g_fail++; }
}

static void check_hex(const char *name, unsigned long got, unsigned long want)
{
    if (got == want) { printf("  [PASS] %-62s = 0x%08lX\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-62s = 0x%08lX (expected 0x%08lX)\n", name, got, want); g_fail++; }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static esc_t g_esc;
static uint8_t *req(void)  { return g_esc.regs + g_esc.mbx_out; }
static uint8_t *resp(void) { return g_esc.regs + g_esc.mbx_in; }

static const esc_profile_t *load(const char *dir, const char *name)
{
    char path[512], err[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    esc_profile_t *p = esc_prof_load(path, err, sizeof(err));
    if (!p) { printf("  [FAIL] load %s: %s\n", path, err); g_fail++; }
    return p;
}

static void node(const esc_profile_t *p)
{
    free(g_esc.prof_st ? g_esc.prof_st->val : NULL);
    free(g_esc.prof_st);
    memset(&g_esc, 0, sizeof(g_esc));
    esc_init(&g_esc, 0, 4);
    esc_prof_attach(&g_esc, p);
    g_esc.regs[REG_AL_STATUS] = ESM_PREOP;
}

static void req_header(uint16_t mbx_len)
{
    memset(req(), 0, g_esc.mbx_out_len);
    memset(resp(), 0, g_esc.mbx_in_len);
    g_esc.regs[REG_SM1_STATUS] &= (uint8_t)~SM_STATUS_MAILBOX_FULL;
    wr16(req() + 0, mbx_len);
    req()[5] = 0x03;
    wr16(req() + 6, 0x2000);
}

static uint8_t  r_cmd(void)     { return resp()[8]; }
static int      r_service(void) { return rd16(resp() + 6) >> 12; }
static uint32_t r_abort(void)   { return (r_service() == 2 && r_cmd() == 0x80) ? rd32(resp() + 12) : 0; }

/* expedited upload (<= 4 byte) or normal upload in one frame; value in out */
static uint32_t upload(uint16_t index, uint8_t sub, int ca, uint8_t *out, int *n)
{
    req_header(0x000a);
    req()[8] = ca ? 0x50 : 0x40;
    wr16(req() + 9, index);
    req()[11] = sub;
    coe_on_mailbox_out_write(&g_esc);
    if (r_abort()) return r_abort();
    if (r_cmd() & 0x02) {
        *n = 4 - ((r_cmd() >> 2) & 3);
        memcpy(out, resp() + 12, (size_t)*n);
    } else {
        *n = (int)rd32(resp() + 12);
        memcpy(out, resp() + 16, (size_t)*n);   /* fits one frame in these tests */
    }
    return 0;
}

static uint32_t up32(uint16_t index, uint8_t sub)
{
    uint8_t b[256] = { 0 };
    int n = 0;
    if (upload(index, sub, 0, b, &n)) return 0xFFFFFFFFu;
    return rd32(b);
}

/* expedited (len <= 4) or normal download in one frame */
static uint32_t download(uint16_t index, uint8_t sub, int ca, const uint8_t *d, int len)
{
    if (len <= 4) {
        req_header(0x000a);
        req()[8] = (uint8_t)(0x23 | ((4 - len) << 2) | (ca ? 0x10 : 0));
        wr16(req() + 9, index);
        req()[11] = sub;
        memcpy(req() + 12, d, (size_t)len);
    } else {
        req_header((uint16_t)(0x0a + len));
        req()[8] = ca ? 0x31 : 0x21;
        wr16(req() + 9, index);
        req()[11] = sub;
        req()[12] = (uint8_t)len;
        memcpy(req() + 16, d, (size_t)len);
    }
    coe_on_mailbox_out_write(&g_esc);
    return r_abort();
}

static uint32_t dl8(uint16_t i, uint8_t s, uint8_t v) { return download(i, s, 0, &v, 1); }
static uint32_t dl16(uint16_t i, uint8_t s, uint16_t v)
{
    uint8_t b[2];
    wr16(b, v);
    return download(i, s, 0, b, 2);
}

static uint16_t sii(size_t w) { return g_esc.sii_image_buf[w]; }

/* the master's SM2/SM3 configuration (length, enable) */
static void sm_cfg(int sm, uint16_t len)
{
    uint8_t *r = g_esc.regs + REG_SM_BASE + sm * REG_SM_ENTRY_SIZE;
    wr16(r + SM_OFF_LENGTH, len);
    r[SM_OFF_ACTIVATE] = len ? SM_ACT_ENABLE : 0;
}

static void al_request(uint8_t state)
{
    wr16(g_esc.regs + REG_AL_CONTROL, state);
    esc_al_control_write(&g_esc);
}
static uint16_t al_status(void) { return rd16(g_esc.regs + REG_AL_STATUS); }
static uint16_t al_code(void)   { return rd16(g_esc.regs + REG_AL_STATUS_CODE); }

/* ------------------------------------------------------------------------ */
static void test_is620n(const esc_profile_t *p)
{
    printf("\n[P-01] IS620N profile: SII, dictionary, 0x1C12/0x1C13 rules\n");
    node(p);
    check_hex("SII vendor (words 8..9)", (unsigned long)sii(8) | (unsigned long)sii(9) << 16, 0x00100000);
    check_hex("SII product (words 10..11)", (unsigned long)sii(10) | (unsigned long)sii(11) << 16, 0x000C0108);
    check_hex("SII mailbox out 0x1000 / in 0x1400 (words 24, 26)", (unsigned long)sii(24) << 16 | sii(26), 0x10001400);
    check("node mailbox in is at 0x1400", g_esc.mbx_in, 0x1400);
    check_hex("first category = Strings (10)", sii(SII_CATEGORY_START_WORD), SII_CAT_STRINGS);
    check("  string 1 = \"IS620N\" (SOEM slave name)",
          !memcmp((const uint8_t *)&g_esc.sii_image_buf[SII_CATEGORY_START_WORD + 2], "\x01\x06IS620N", 8), 1);
    check("no Complete Access (ESI CompleteAccess absent)", g_esc.coe_ca, 0);
    check_hex("SDO 0x1018:01 = vendor", up32(0x1018, 1), 0x00100000);
    check_hex("SDO 0x1018:02 = product", up32(0x1018, 2), 0x000C0108);
    check_hex("SDO 0x1C12:01 = 0x1701 (ESI default)", up32(0x1C12, 1) & 0xFFFF, 0x1701);
    check_hex("SDO 0x1C13:01 = 0x1B01", up32(0x1C13, 1) & 0xFFFF, 0x1B01);
    check_hex("object that does not exist (0x5FFF)", up32(0x5FFF, 0), 0xFFFFFFFF);
    check_hex("  its abort 0x06020000", r_abort(), 0x06020000);
    check_hex("write read-only 0x1018:01 -> 0x06010002", dl16(0x1018, 1, 1) ? r_abort() : 0, 0x06010002);
    check_hex("write 0x1C12:01 while SI0 = 1 -> 0x08000022", dl16(0x1C12, 1, 0x1702), 0x08000022);
    check_hex("0x1C12:00 = 0 ok", dl8(0x1C12, 0, 0), 0);
    check_hex("0x1C12:01 = 0x1B02 (a TxPDO) -> 0x06090030", dl16(0x1C12, 1, 0x1B02), 0x06090030);
    check_hex("0x1C12:01 = 0x1799 (no such PDO) -> 0x06090030", dl16(0x1C12, 1, 0x1799), 0x06090030);
    check_hex("0x1C12:01 = 0x1702 ok", dl16(0x1C12, 1, 0x1702), 0);
    check_hex("0x1C12:00 = 2 (> 1 entry) -> 0x06090030", dl8(0x1C12, 0, 2), 0x06090030);
    check_hex("0x1C12:00 = 1 ok", dl8(0x1C12, 0, 1), 0);
    check("assigned output bits now 0x1702 = 152", (long)esc_prof_assigned_bits(&g_esc, 0), 152);
    check_hex("CA upload refused (no CA) -> 0x06010000", ({ uint8_t b[64]; int n; upload(0x1C12, 0, 1, b, &n); }), 0x06010000);

    printf("\n[P-02] IS620N: PREOP -> SAFEOP checks SM2/SM3 against the assignment\n");
    node(p);
    sm_cfg(2, 12); sm_cfg(3, 25);                  /* 0x1B01 is 28 byte */
    al_request(ESM_SAFEOP);
    check_hex("SM3 25 byte vs 0x1B01 (28): PREOP+ERR", al_status(), ESM_PREOP | 0x10);
    check_hex("  AL status code 0x001E (invalid input configuration)", al_code(), 0x001E);
    node(p);
    sm_cfg(2, 19); sm_cfg(3, 28);
    al_request(ESM_SAFEOP);
    check_hex("SM2 19 byte vs 0x1701 (12): 0x001D", al_code(), 0x001D);
    node(p);
    sm_cfg(2, 12); sm_cfg(3, 28);
    al_request(ESM_SAFEOP);
    check_hex("SM2 12 / SM3 28 = ESI default: SAFEOP", al_status(), ESM_SAFEOP);
    check_hex("write 0x1C12:00 in SAFEOP -> 0x08000022", dl8(0x1C12, 0, 0), 0x08000022);
    node(p);
    dl8(0x1C13, 0, 0); dl16(0x1C13, 1, 0x1B02); dl8(0x1C13, 0, 1);
    sm_cfg(2, 12); sm_cfg(3, 25);                  /* 0x1B02 is 25 byte */
    al_request(ESM_SAFEOP);
    check_hex("after 0x1C13 = 0x1B02, SM3 25 byte: SAFEOP", al_status(), ESM_SAFEOP);

    printf("\n[P-03] IS620N: power cycle brings the ESI defaults back\n");
    node(p);
    dl8(0x1C13, 0, 0); dl16(0x1C13, 1, 0x1B02);
    esc_t *chain = &g_esc;
    esc_fault_bus_t *f = calloc(1, sizeof(*f));
    FILE *devnull = fopen("/dev/null", "w");
    esc_fault_init(f, devnull, -1);
    esc_fault_command(f, chain, 1, "drop_node 0", 0);
    esc_fault_command(f, chain, 1, "restore_node 0", 0);
    if (devnull) fclose(devnull);
    free(f);
    check("node keeps its profile after power-on", g_esc.prof == p, 1);
    g_esc.regs[REG_AL_STATUS] = ESM_PREOP;
    check_hex("0x1C13:01 back to 0x1B01", up32(0x1C13, 1) & 0xFFFF, 0x1B01);
    check_hex("SII vendor still IS620N", (unsigned long)sii(8) | (unsigned long)sii(9) << 16, 0x00100000);
}

static void test_p1(const esc_profile_t *p)
{
    printf("\n[P-04] P1 draft profile: Complete Access, two TxPDOs\n");
    node(p);
    check("Complete Access on (ESI CompleteAccess)", g_esc.coe_ca, 1);
    check_hex("SII mailbox in 0x1080 (word 26)", sii(26), 0x1080);
    uint8_t b[64]; int n = 0;
    check_hex("CA upload 0x1C13:00", upload(0x1C13, 0, 1, b, &n), 0);
    check("  6 byte: 02 00 00 1A 01 1A", n == 6 && b[0] == 2 && rd16(b + 2) == 0x1A00 && rd16(b + 4) == 0x1A01, 1);
    uint8_t ok1[] = { 1, 0, 0x00, 0x1A };
    check_hex("CA download 0x1C13 = {0x1A00} ok", download(0x1C13, 0, 1, ok1, 4), 0);
    check("  input bits now 0x1A00 = 120", (long)esc_prof_assigned_bits(&g_esc, 1), 120);
    uint8_t dup[] = { 2, 0, 0x00, 0x1A, 0x00, 0x1A };
    check_hex("CA download {0x1A00, 0x1A00} -> 0x06090030", download(0x1C13, 0, 1, dup, 6), 0x06090030);
    check("  refused CA left SI0 = 1", (long)(up32(0x1C13, 0) & 0xFF), 1);
    uint8_t back[] = { 2, 0, 0x00, 0x1A, 0x01, 0x1A };
    check_hex("CA download {0x1A00, 0x1A01} ok", download(0x1C13, 0, 1, back, 6), 0);
    check("  input bits 168 (21 byte)", (long)esc_prof_assigned_bits(&g_esc, 1), 168);
    check_hex("CA upload of mapping 0x1A01:00", upload(0x1A01, 0, 1, b, &n), 0);
    check("  2 + 3 x 4 byte, entry 1 = 0x2F000110", n == 14 && rd32(b + 2) == 0x2F000110, 1);
    check_hex("mapping 0x1600 read-only (no PdoConfig)", dl8(0x1600, 0, 0), 0x06010002);
}

static void test_exclude(const char *tmpdir)
{
    printf("\n[P-05] ESI Exclude: two TxPDOs that exclude each other\n");
    char path[512];
    snprintf(path, sizeof(path), "%s/test_exclude.prof", tmpdir);
    FILE *f = fopen(path, "w");
    if (!f) { check("write temp profile", 0, 1); return; }
    fprintf(f,
        "profile 1\nname \"exclude test\"\n"
        "identity vendor 0x1 product 0x2 rev 0x3 serial 0\n"
        "mailbox out 0x1000 128 in 0x1080 128 coe 1\n"
        "coe sdoinfo 0 pdoassign 1 pdoconfig 0 ca 0\n"
        "sm 0 start 0x1000 len 128 ctrl 0x26 en 1\nsm 1 start 0x1080 len 128 ctrl 0x22 en 1\n"
        "sm 2 start 0x1100 len 0 ctrl 0x64 en 1\nsm 3 start 0x1180 len 2 ctrl 0x20 en 1\n"
        "dc 0 assign 0\n"
        "pdo tx 0x1A00 sm 3 fixed 1 entries 0x6000:01:16 excludes 0x1A01\n"
        "pdo tx 0x1A01 sm 255 fixed 1 entries 0x6000:02:16 excludes 0x1A00\n"
        "pdo tx 0x1A02 sm 255 fixed 1 entries 0x6000:03:16 excludes -\n"
        "obj 0x1A00 record 1\nsub 0x1A00 0 bits 8 ro 01\nsub 0x1A00 1 bits 32 ro 10010060\n"
        "obj 0x1A01 record 1\nsub 0x1A01 0 bits 8 ro 01\nsub 0x1A01 1 bits 32 ro 10020060\n"
        "obj 0x1A02 record 1\nsub 0x1A02 0 bits 8 ro 01\nsub 0x1A02 1 bits 32 ro 10030060\n"
        "obj 0x1C13 array 2\nsub 0x1C13 0 bits 8 rw_preop 01\nsub 0x1C13 1 bits 16 rw_preop 001a\n"
        "sub 0x1C13 2 bits 16 rw_preop 0000\n");
    fclose(f);
    char err[256];
    esc_profile_t *p = esc_prof_load(path, err, sizeof(err));
    check("synthetic profile loads", p != NULL, 1);
    if (!p) return;
    node(p);
    dl8(0x1C13, 0, 0);
    dl16(0x1C13, 2, 0x1A01);
    check_hex("SI0 = 2 with {0x1A00, 0x1A01} (excluded) -> 0x06090030", dl8(0x1C13, 0, 2), 0x06090030);
    dl16(0x1C13, 2, 0x1A02);
    check_hex("SI0 = 2 with {0x1A00, 0x1A02} ok", dl8(0x1C13, 0, 2), 0);
    check("dc 0 -> profile without DC", p->dc, 0);
    free(g_esc.prof_st->val); free(g_esc.prof_st); g_esc.prof_st = NULL;
    esc_prof_free(p);
    remove(path);
}

static void test_load_errors(const char *tmpdir)
{
    printf("\n[P-06] profile parser refuses broken files\n");
    char path[512], err[256];
    snprintf(path, sizeof(path), "%s/test_bad.prof", tmpdir);
    const char *bad[] = {
        "name \"x\"\n",                                         /* no header */
        "profile 2\n",                                          /* version */
        "profile 1\nidentity vendor 0x1 product 0x2 rev 0x3 serial 0\n", /* incomplete */
        "profile 1\nfoo 1\n",                                   /* unknown record */
        "profile 1\nobj 0x1000 var 1\nsub 0x1001 0 bits 32 ro 00000000\n", /* sub of another obj */
    };
    const char *want[] = { "missing 'profile 1'", "unsupported profile version", "incomplete profile",
                           "unknown record", "bad sub line" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        FILE *f = fopen(path, "w");
        if (!f) return;
        fputs(bad[i], f);
        fclose(f);
        err[0] = 0;
        esc_profile_t *p = esc_prof_load(path, err, sizeof(err));
        char name[96];
        snprintf(name, sizeof(name), "refused: %s", want[i]);
        check(name, p == NULL && strstr(err, want[i]) != NULL, 1);
        esc_prof_free(p);
    }
    remove(path);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../config/profiles";
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    printf("test_profile: profiles from %s\n", dir);
    /* the reduced hand-written IS620N profile (the vendor ESI is not in the
     * repo); a profile generated from the ESI can be given as argv[2] and
     * gets the same IS620N tests */
    const esc_profile_t *is = load(dir, "is620n_min.prof");
    const esc_profile_t *p1 = load(dir, "p1_draft.prof");
    if (is) test_is620n(is);
    if (argc > 2) {
        char err[512];
        const esc_profile_t *full = esc_prof_load(argv[2], err, sizeof(err));
        printf("\n--- the IS620N tests again on %s\n", argv[2]);
        if (!full) { printf("  [FAIL] %s\n", err); g_fail++; }
        else test_is620n(full);
    }
    if (p1) test_p1(p1);
    test_exclude(tmp);
    test_load_errors(tmp);
    printf("\n=========================================================\n");
    printf(" RESULT: %d pass, %d fail\n", g_pass, g_fail);
    printf("=========================================================\n");
    return g_fail ? 1 : 0;
}
