/* ==========================================================================
 * test_coe.c — GD9.3 C-01 / C-01-neg: the CoE/SDO server of soft_bus
 * (esc_coe.c) driven byte by byte, no network.
 *
 * Each request is built exactly as SOEM ecx_SDOread()/ecx_SDOwrite() build
 * it (ec_coe.c), written into SM0 of one node, and the response is read
 * back from SM1. Covers normal and segmented download, Complete Access
 * upload/download (SI0 padded to 16 bit, CA bit echoed), the SII General
 * category of --coe-ca, and the abort codes of the negative cases.
 *
 * Build & run:  make test_coe && ./test_coe
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_core.h"
#include "esc_coe.h"
#include "esc_fault.h"

static int g_pass = 0, g_fail = 0;

static void check(const char *name, long got, long want)
{
    if (got == want) {
        printf("  [PASS] %-60s = %ld\n", name, got);
        g_pass++;
    } else {
        printf("  [FAIL] %-60s = %ld (expected %ld)\n", name, got, want);
        g_fail++;
    }
}

static void check_hex(const char *name, unsigned long got, unsigned long want)
{
    if (got == want) {
        printf("  [PASS] %-60s = 0x%08lX\n", name, got);
        g_pass++;
    } else {
        printf("  [FAIL] %-60s = 0x%08lX (expected 0x%08lX)\n", name, got, want);
        g_fail++;
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static esc_t g_esc;
static uint8_t *req(void)  { return g_esc.regs + SII_SM0_OFFSET; }
static uint8_t *resp(void) { return g_esc.regs + SII_SM1_OFFSET; }

static void node_reset(int pdo_size, int pdo_od, int ca)
{
    memset(&g_esc, 0, sizeof(g_esc));
    esc_init(&g_esc, 0, (uint16_t)pdo_size);
    if (pdo_od || ca) esc_set_coe_features(&g_esc, pdo_od, ca);
    g_esc.regs[REG_AL_STATUS] = ESM_PREOP;
}

/* Mailbox + CoE header of a request, as ecx_SDOread/ecx_SDOwrite fill it. */
static void req_header(uint16_t mbx_len)
{
    memset(req(), 0, SII_SM0_SIZE);
    memset(resp(), 0, SII_SM1_SIZE);
    wr16(req() + 0, mbx_len);
    req()[5] = 0x03;               /* CoE, Cnt left 0 */
    wr16(req() + 6, 0x2000);       /* service SDO request */
}

static void post(void) { coe_on_mailbox_out_write(&g_esc); }

static uint8_t  r_cmd(void)   { return resp()[8]; }
static uint16_t r_index(void) { return rd16(resp() + 9); }
static uint8_t  r_sub(void)   { return resp()[11]; }
static int      r_service(void) { return rd16(resp() + 6) >> 12; }
static uint32_t r_abort(void) { return (r_service() == 2 && r_cmd() == 0x80) ? rd32(resp() + 12) : 0; }

/* Upload request (0x40 or CA 0x50). */
static void sdo_upload_req(uint16_t index, uint8_t sub, int ca)
{
    req_header(0x000a);
    req()[8] = ca ? 0x50 : 0x40;
    wr16(req() + 9, index);
    req()[11] = sub;
    post();
}

/* Expedited download (0x23 | n<<2, CA adds 0x10). len 1..4. */
static void sdo_download_exp(uint16_t index, uint8_t sub, int ca, const uint8_t *d, int len)
{
    req_header(0x000a);
    req()[8] = (uint8_t)(0x23 | ((4 - len) << 2) | (ca ? 0x10 : 0));
    wr16(req() + 9, index);
    req()[11] = sub;
    memcpy(req() + 12, d, (size_t)len);
    post();
}

/* Full SOEM-style download (normal init + segments, ecx_SDOwrite with a
 * 128 byte mailbox). Returns the abort code of the first failing frame, 0
 * if every response was the expected one. `bad_toggle_at` >= 1 flips the
 * toggle of that segment (C-01-neg). frames/last_cmd for inspection. */
static int g_frames;
static uint8_t g_seg_resp[8];
static uint32_t sdo_download_normal(uint16_t index, uint8_t sub, int ca,
                                    const uint8_t *d, int size, int bad_toggle_at)
{
    int maxdata = SII_SM0_SIZE - 0x10;
    int fds = size, notlast = 0;
    if (fds > maxdata) { fds = maxdata; notlast = 1; }

    g_frames = 0;
    req_header((uint16_t)(0x0a + fds));
    req()[8] = ca ? 0x31 : 0x21;
    wr16(req() + 9, index);
    req()[11] = sub;
    wr32(req() + 12, (uint32_t)size);
    memcpy(req() + 16, d, (size_t)fds);
    post();
    g_frames++;
    if (r_abort()) return r_abort();
    if (r_service() != 3 || (r_cmd() & 0xE0) != 0x60 || r_index() != index || r_sub() != sub)
        return 0xFFFFFFFFu;

    const uint8_t *hp = d + fds;
    int left = size - fds;
    uint8_t toggle = 0;
    int segno = 0;
    maxdata += 7;
    while (notlast) {
        segno++;
        fds = left;
        notlast = 0;
        uint8_t cmd = 0x01;
        if (fds > maxdata) { fds = maxdata; notlast = 1; cmd = 0x00; }
        uint16_t len;
        if (!notlast && fds < 7) { len = 0x0a; cmd = (uint8_t)(0x01 + ((7 - fds) << 1)); }
        else                     { len = (uint16_t)(fds + 3); }
        req_header(len);
        uint8_t t = toggle;
        if (segno == bad_toggle_at) t ^= 0x10;
        req()[8] = (uint8_t)(cmd + t);
        memcpy(req() + 9, hp, (size_t)fds);
        post();
        g_frames++;
        if (r_abort()) return r_abort();
        if (segno <= 8) g_seg_resp[segno - 1] = r_cmd();
        if (r_service() != 3 || (r_cmd() & 0xE0) != 0x20 || (r_cmd() & 0x10) != toggle)
            return 0xFFFFFFFEu;
        hp += fds;
        left -= fds;
        toggle ^= 0x10;
    }
    return 0;
}

/* Full SOEM-style upload (normal or CA), reassembled into out. Returns the
 * abort code, 0 on success; *size = bytes received. */
static uint32_t sdo_upload(uint16_t index, uint8_t sub, int ca, uint8_t *out, int *size)
{
    sdo_upload_req(index, sub, ca);
    g_frames = 1;
    if (r_abort()) return r_abort();
    if (r_service() != 3 || r_index() != index) return 0xFFFFFFFFu;
    if (r_cmd() & 0x02) {
        int n = 4 - ((r_cmd() >> 2) & 3);
        memcpy(out, resp() + 12, (size_t)n);
        *size = n;
        return 0;
    }
    int total = (int)rd32(resp() + 12);
    int fr = rd16(resp()) - 10;
    int got = fr < total ? fr : total;
    memcpy(out, resp() + 16, (size_t)got);
    uint8_t toggle = 0;
    while (got < total) {
        req_header(0x000a);
        req()[8] = (uint8_t)(0x60 + toggle);
        wr16(req() + 9, index);
        req()[11] = sub;
        post();
        g_frames++;
        if (r_abort()) return r_abort();
        if ((r_cmd() & 0xE0) != 0x00 || (r_cmd() & 0x10) != toggle) return 0xFFFFFFFEu;
        int n = rd16(resp()) - 3;
        if ((r_cmd() & 0x01) && n == 7) n -= (r_cmd() & 0x0e) >> 1;
        memcpy(out + got, resp() + 9, (size_t)n);
        got += n;
        if (r_cmd() & 0x01) break;
        toggle ^= 0x10;
    }
    *size = got;
    return 0;
}

/* ---- tests ---------------------------------------------------------- */

static void test_default_unchanged(void)
{
    printf("\n[C-01] default node (no --coe-pdo-od / --coe-ca): CA and PDO objects off\n");
    node_reset(4, 0, 0);
    int has_general = 0;
    for (size_t w = SII_CATEGORY_START_WORD; w < g_esc.sii_image_words; ) {
        uint16_t t = g_esc.sii_image_buf[w];
        if (t == SII_CAT_END) break;
        if (t == SII_CAT_GENERAL) has_general = 1;
        w += 2u + g_esc.sii_image_buf[w + 1];
    }
    check("SII has no General category", has_general, 0);
    check("SII category after word 64 is SyncM (41)", g_esc.sii_image_buf[SII_CATEGORY_START_WORD], SII_CAT_SYNCM);

    sdo_upload_req(0x1C12, 0, 1);
    check_hex("CA upload 0x1C12 -> abort 0x06010000 (as before)", r_abort(), 0x06010000u);
    check("  abort echoes index", r_index(), 0x1C12);
    sdo_upload_req(0x1C00, 0, 0);
    check_hex("upload 0x1C00:00 -> abort 0x06020000 (no PDO objects)", r_abort(), 0x06020000u);
    uint8_t d[4] = { 1, 0, 0, 0x16 };
    uint32_t a = sdo_download_normal(0x1C12, 0, 1, d, 4, 0);
    check_hex("CA download 0x1C12 -> abort 0x06010000", a, 0x06010000u);
    sdo_upload_req(0x9999, 0, 0);
    check_hex("upload 0x9999 -> abort 0x06020000 (L4-04 unchanged)", r_abort(), 0x06020000u);
}

static void test_normal_and_segmented_download(void)
{
    uint8_t d[COE_OCTET_RW_MAX], back[COE_XFER_BUF_MAX];
    int size = 0;
    uint32_t a;

    printf("\n[C-01] normal download (one frame) 0x8002, 16 byte\n");
    node_reset(4, 0, 0);
    for (int i = 0; i < 16; i++) d[i] = (uint8_t)(0xA0 + i);
    a = sdo_download_normal(0x8002, 0, 0, d, 16, 0);
    check_hex("download 16 byte -> no abort", a, 0);
    check("  frames used", g_frames, 1);
    check_hex("  response command 0x60", r_cmd(), 0x60);
    a = sdo_upload(0x8002, 0, 0, back, &size);
    check_hex("upload back -> no abort", a, 0);
    check("  size", size, 16);
    check("  content identical", memcmp(back, d, 16), 0);

    printf("\n[C-01] segmented download 0x8002, 400 byte = init 112 + 3 segments\n");
    for (int i = 0; i < 400; i++) d[i] = (uint8_t)(i * 7 + 3);
    a = sdo_download_normal(0x8002, 0, 0, d, 400, 0);
    check_hex("download 400 byte -> no abort", a, 0);
    check("  frames used (init + 3 segments)", g_frames, 4);
    check_hex("  segment 1 response 0x20 (toggle 0)", g_seg_resp[0], 0x20);
    check_hex("  segment 2 response 0x30 (toggle 1)", g_seg_resp[1], 0x30);
    check_hex("  segment 3 response 0x20 (toggle 0)", g_seg_resp[2], 0x20);
    check("  segment response Index/SubIndex bytes zero", rd16(resp() + 9) | resp()[11], 0);
    a = sdo_upload(0x8002, 0, 0, back, &size);
    check_hex("upload back (segmented) -> no abort", a, 0);
    check("  size", size, 400);
    check("  content identical", memcmp(back, d, 400), 0);

    printf("\n[C-01] segmented download, last segment < 7 byte (padded, n in cmd)\n");
    for (int i = 0; i < 115; i++) d[i] = (uint8_t)(0x55 ^ i);
    a = sdo_download_normal(0x8002, 0, 0, d, 115, 0);
    check_hex("download 115 byte (112 + 3) -> no abort", a, 0);
    check("  frames used", g_frames, 2);
    a = sdo_upload(0x8002, 0, 0, back, &size);
    check("  stored size 115 (padding not stored)", size, 115);
    check("  content identical", memcmp(back, d, 115), 0);

    printf("\n[C-01] expedited download of a short OCTET_STRING\n");
    uint8_t e3[3] = { 0x11, 0x22, 0x33 };
    sdo_download_exp(0x8002, 0, 0, e3, 3);
    check_hex("expedited 3 byte -> response 0x60", r_cmd(), 0x60);
    a = sdo_upload(0x8002, 0, 0, back, &size);
    check("  upload back size 3 (expedited)", size, 3);
    check("  content identical", memcmp(back, e3, 3), 0);
}

static void test_download_negative(void)
{
    uint8_t d[COE_XFER_BUF_MAX], back[COE_XFER_BUF_MAX];
    int size = 0;
    uint32_t a;

    printf("\n[C-01-neg] download aborts\n");
    node_reset(4, 0, 0);
    for (int i = 0; i < 16; i++) d[i] = (uint8_t)(0xC0 + i);
    sdo_download_normal(0x8002, 0, 0, d, 16, 0);  /* known content */

    for (int i = 0; i < 300; i++) d[i] = 0xEE;
    a = sdo_download_normal(0x8002, 0, 0, d, 300, 2);
    check_hex("repeated toggle in segment 2 -> abort 0x05030000", a, 0x05030000u);
    check("  abort echoes the transfer's index", r_index(), 0x8002);
    sdo_upload(0x8002, 0, 0, back, &size);
    check("  0x8002 untouched (size still 16)", size, 16);
    check("  0x8002 untouched (content)", back[0], 0xC0);

    req_header(0x000a);
    req()[8] = 0x01;   /* last download segment, no transfer in progress */
    post();
    check_hex("download segment without a session -> 0x05040001", r_abort(), 0x05040001u);

    a = sdo_download_normal(0x8000, 1, 0, d, 8, 0);
    check_hex("8 byte to 0x8000:01 (U32) -> 0x06070010", a, 0x06070010u);
    check("  refused at the init frame", g_frames, 1);
    uint8_t v4[4] = { 1, 2, 3, 4 };
    sdo_download_exp(0x1018, 1, 0, v4, 4);
    check_hex("write 0x1018:01 (read-only) -> 0x06010002", r_abort(), 0x06010002u);
    sdo_download_exp(0x8000, 9, 0, v4, 4);
    check_hex("write 0x8000:09 -> 0x06090011", r_abort(), 0x06090011u);
    sdo_upload_req(0x8000, 9, 0);
    check_hex("read 0x8000:09 -> 0x06090011", r_abort(), 0x06090011u);
    sdo_download_exp(0x8000, 1, 0, v4, 2);
    check_hex("2 byte expedited to 0x8000:01 -> 0x06070010", r_abort(), 0x06070010u);
    a = sdo_download_normal(0x8002, 0, 0, d, COE_OCTET_RW_MAX + 1, 0);
    check_hex("401 byte to 0x8002 -> 0x06070012", a, 0x06070012u);
    sdo_download_exp(0x8000, 1, 0, v4, 4);
    check_hex("expedited 4 byte to 0x8000:01 still ok (L4-02)", r_cmd(), 0x60);
    sdo_upload_req(0x8000, 1, 0);
    check_hex("  read back", rd32(resp() + 12), 0x04030201u);

    req_header(0x000a);
    req()[8] = 0x80;   /* abort from the master: no response */
    post();
    check("master abort request gets no response (SM1 empty)", resp()[8], 0);
}

static void test_ca(void)
{
    uint8_t out[COE_XFER_BUF_MAX];
    int size = 0;
    uint32_t a;

    printf("\n[C-01] --coe-ca: SII General category\n");
    node_reset(4, 0, 1);
    const uint16_t *w = g_esc.sii_image_buf;
    check("word 64 = General (30)", w[SII_CATEGORY_START_WORD], SII_CAT_GENERAL);
    check("word 65 = 16 words", w[SII_CATEGORY_START_WORD + 1], 16);
    const uint8_t *b = (const uint8_t *)w;   /* little-endian host, same as SII byte order */
    unsigned ssigen = (SII_CATEGORY_START_WORD + 1) * 2;   /* ecx_siifind(): size word */
    check_hex("SOEM ssigen+0x07 = CoE details SDO|PDOASSIGN|SDOCA", b[ssigen + 0x07], 0x25);
    check("SOEM ssigen+0x0d bit1 (blockLRW) clear", b[ssigen + 0x0d] & 0x02, 0);
    check("SOEM ssigen+0x0e/0f (E-bus current) 0", b[ssigen + 0x0e] | b[ssigen + 0x0f], 0);
    check("SyncM follows General", w[SII_CATEGORY_START_WORD + 18], SII_CAT_SYNCM);

    printf("\n[C-01] CA upload\n");
    a = sdo_upload(0x1C12, 0, 1, out, &size);
    check_hex("CA 0x1C12:00 -> no abort", a, 0);
    check_hex("  expedited, CA bit echoed (0x53)", r_cmd(), 0x53);
    check("  size 4 (SI0 U8 + pad + U16)", size, 4);
    check_hex("  bytes 01 00 00 16", rd32(out), 0x16000001u);
    a = sdo_upload(0x1C12, 1, 1, out, &size);
    check_hex("CA 0x1C12:01 (SI0 excluded) -> 2 byte, 0x5B", r_cmd(), 0x5B);
    check_hex("  value 0x1600", rd16(out), 0x1600);
    a = sdo_upload(0x1C00, 0, 1, out, &size);
    check_hex("CA 0x1C00:00 -> normal, CA bit (0x51)", r_cmd(), 0x51);
    check("  size 6", size, 6);
    check("  04 00 01 02 03 04", out[0] == 4 && out[1] == 0 && out[2] == 1 && out[3] == 2 &&
                                  out[4] == 3 && out[5] == 4, 1);
    a = sdo_upload(0x1A00, 0, 1, out, &size);
    check("CA 0x1A00:00 size 6 (n=1)", size, 6);
    check_hex("  entry 0x60000120 (0x6000:01, 32 bit)", rd32(out + 2), 0x60000120u);
    a = sdo_upload(0x1600, 0, 1, out, &size);
    check_hex("CA 0x1600 entry 0x70000120", rd32(out + 2), 0x70000120u);

    printf("\n[C-01] --coe-pdo-od, plain (non-CA) reads as ecx_readPDOmap does\n");
    sdo_upload_req(0x1C00, 0, 0);
    check_hex("0x1C00:00 expedited 1 byte (0x4F)", r_cmd(), 0x4F);
    check("  value 4", resp()[12], 4);
    sdo_upload_req(0x1C00, 3, 0);
    check("0x1C00:03 = 3 (outputs)", resp()[12], 3);
    sdo_upload_req(0x1C13, 1, 0);
    check_hex("0x1C13:01 = 0x1A00 (0x4B, 2 byte)", (unsigned long)r_cmd() << 16 | rd16(resp() + 12), 0x4B1A00u);
    sdo_upload_req(0x1A00, 1, 0);
    check_hex("0x1A00:01 = 0x60000120", rd32(resp() + 12), 0x60000120u);

    printf("\n[C-01] CA download\n");
    uint8_t asg[4] = { 1, 0, 0x00, 0x16 };
    a = sdo_download_normal(0x1C12, 0, 1, asg, 4, 0);
    check_hex("CA 0x1C12:00 = 01 00 00 16 (0x31) in PREOP -> ok", a, 0);
    check_hex("  response 0x70 (CA bit echoed)", r_cmd(), 0x70);
    sdo_download_exp(0x1C12, 0, 1, asg, 4);
    check_hex("CA expedited (0x33) -> 0x70", r_cmd(), 0x70);
    uint8_t pid[14] = { 3, 0, 0x11,0,0,0, 0x22,0,0,0, 0x33,0,0,0 };
    a = sdo_download_normal(0x8000, 0, 1, pid, 14, 0);
    check_hex("CA 0x8000:00 (SI0 RO skipped, 3 x U32) -> ok", a, 0);
    check("  kp/ki/kd stored", g_esc.coe_od.kp == 0x11 && g_esc.coe_od.ki == 0x22 &&
                                g_esc.coe_od.kd == 0x33, 1);
    a = sdo_download_normal(0x8000, 1, 1, pid + 2, 4, 0);
    check_hex("CA 0x8000:01, data ends after SI1 -> ok", a, 0);
    a = sdo_upload(0x8000, 0, 1, out, &size);
    check("CA upload 0x8000:00 size 14", size, 14);
    check("  matches", memcmp(out, pid, 14), 0);

    printf("\n[C-01] plain PDO assign sequence (TwinCAT style): SI0=0, SI1, SI0=1\n");
    uint8_t z = 0, one = 1, idx[2] = { 0x00, 0x16 };
    sdo_download_exp(0x1C12, 0, 0, &z, 1);   uint8_t c1 = r_cmd();
    sdo_download_exp(0x1C12, 1, 0, idx, 2);  uint8_t c2 = r_cmd();
    sdo_download_exp(0x1C12, 0, 0, &one, 1); uint8_t c3 = r_cmd();
    check("  three responses 0x60", c1 == 0x60 && c2 == 0x60 && c3 == 0x60, 1);
}

static void test_ca_negative(void)
{
    uint8_t out[COE_XFER_BUF_MAX];
    int size = 0;
    uint32_t a;

    printf("\n[C-01-neg] Complete Access aborts\n");
    node_reset(4, 0, 1);
    sdo_upload_req(0x1C12, 2, 1);
    check_hex("CA from subindex 2 -> 0x06010000", r_abort(), 0x06010000u);
    sdo_upload_req(0x8002, 0, 1);
    check_hex("CA upload 0x8002 (OCTET_STRING) -> 0x06010004", r_abort(), 0x06010004u);
    uint8_t d4[4] = { 1, 2, 3, 4 };
    a = sdo_download_normal(0x8001, 0, 1, d4, 4, 0);
    check_hex("CA download 0x8001 (OCTET_STRING) -> 0x06010004", a, 0x06010004u);
    uint8_t id[18] = { 4, 0 };
    a = sdo_download_normal(0x1018, 0, 1, id, 18, 0);
    check_hex("CA download 0x1018 (all read-only) -> 0x06010002", a, 0x06010002u);
    uint8_t big[8] = { 1, 0, 0x00, 0x16, 0, 0, 0, 0 };
    a = sdo_download_normal(0x1C12, 0, 1, big, 8, 0);
    check_hex("CA 0x1C12 with 8 byte (full = 4) -> 0x06070012", a, 0x06070012u);
    uint8_t cut[3] = { 1, 0, 0x00 };
    a = sdo_download_normal(0x1C12, 0, 1, cut, 3, 0);
    check_hex("CA 0x1C12 ending inside SI1 -> 0x06070010", a, 0x06070010u);
    uint8_t bad[4] = { 0, 0, 0x01, 0x16 };  /* SI0 = 0 AND SI1 = 0x1601 */
    a = sdo_download_normal(0x1C12, 0, 1, bad, 4, 0);
    check_hex("CA 0x1C12 SI1 = 0x1601 -> 0x06090030", a, 0x06090030u);
    check("  atomic: SI0 not changed to 0", g_esc.coe_od.pdo_assign_n[0], 1);
    uint8_t n2[4] = { 2, 0, 0x00, 0x16 };
    a = sdo_download_normal(0x1C12, 0, 1, n2, 4, 0);
    check_hex("CA 0x1C12 SI0 = 2 -> 0x06090030", a, 0x06090030u);

    g_esc.regs[REG_AL_STATUS] = ESM_SAFEOP;
    uint8_t ok[4] = { 1, 0, 0x00, 0x16 };
    a = sdo_download_normal(0x1C12, 0, 1, ok, 4, 0);
    check_hex("CA PDO assign in SAFEOP -> 0x08000022", a, 0x08000022u);
    g_esc.regs[REG_AL_STATUS] = ESM_OP;
    sdo_download_exp(0x1C13, 0, 1, ok, 4);
    check_hex("CA PDO assign in OP -> 0x08000022", r_abort(), 0x08000022u);
    uint8_t one = 1;
    sdo_download_exp(0x1C13, 0, 0, &one, 1);
    check_hex("plain 0x1C13:00 in OP -> 0x08000022", r_abort(), 0x08000022u);
    sdo_upload_req(0x1C13, 0, 1);
    check_hex("CA upload in OP still allowed (0x53)", r_cmd(), 0x53);

    printf("\n[C-01] CA upload segmented: --pdo-size 1920 -> 0x1600 with 62 entries\n");
    node_reset(1920, 0, 1);
    a = sdo_upload(0x1600, 0, 1, out, &size);
    check_hex("CA 0x1600 -> no abort", a, 0);
    check("  size 2 + 62*4", size, 2 + 62 * 4);
    check("  frames (init + 2 segments)", g_frames, 3);
    uint32_t bits = 0;
    int ok_entries = 1;
    for (int i = 0; i < out[0]; i++) {
        uint32_t e = rd32(out + 2 + 4 * i);
        if ((e >> 16) != 0x7000 || ((e >> 8) & 0xFF) != (uint32_t)(i + 1)) ok_entries = 0;
        bits += e & 0xFF;
    }
    check("  entries 0x7000:1..62", ok_entries, 1);
    check("  total bits = 1920*8", (long)bits, 1920L * 8);
}

/* GD9.6 E-09 (offline part): coe_delay holds a DOWNLOAD response, an
 * upload is answered at once, the held one appears at the first frame
 * after the release time. */
static void test_coe_delay(void)
{
    printf("\n[GD9.6] coe_delay: slow SDO download responses\n");
    static esc_fault_bus_t fb;
    esc_fault_init(&fb, NULL, 0);
    uint8_t frame[64] = { 0 };
    node_reset(4, 0, 0);
    g_esc.fault.coe_delay_ms = 5;
    esc_fault_frame_begin(&fb, &g_esc, 1, frame, 0, 1000000ull);      /* t = 1 ms */

    uint8_t d[4] = { 0x2a, 0, 0, 0 };
    sdo_download_exp(0x8000, 1, 0, d, 4);
    check("download: response bytes written", r_cmd(), 0x60);
    check("download: mailbox full NOT set yet",
          !!(g_esc.regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL), 0);
    check("download: held", g_esc.fault.mbx_held, 1);
    esc_fault_frame_begin(&fb, &g_esc, 1, frame, 0, 5900000ull);      /* t = 5.9 ms */
    check("4.9 ms later: still held", !!(g_esc.regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL), 0);
    esc_fault_frame_begin(&fb, &g_esc, 1, frame, 0, 6000000ull);      /* t = 6 ms */
    check("5 ms later: mailbox full", !!(g_esc.regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL), 1);
    check("5 ms later: released", g_esc.fault.mbx_held, 0);

    g_esc.regs[REG_SM1_STATUS] &= (uint8_t)~SM_STATUS_MAILBOX_FULL;   /* master read it */
    sdo_upload_req(0x8000, 1, 0);
    check("upload: answered at once (not delayed)",
          !!(g_esc.regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL), 1);

    esc_fault_command(&fb, &g_esc, 1, "coe_delay 0 0", 7000000ull);
    g_esc.regs[REG_SM1_STATUS] &= (uint8_t)~SM_STATUS_MAILBOX_FULL;
    sdo_download_exp(0x8000, 1, 0, d, 4);
    check("ctl 'coe_delay 0 0': download answered at once",
          !!(g_esc.regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL), 1);
}

int main(void)
{
    printf("=========================================================\n");
    printf(" test_coe — CoE/SDO server (GD9.3 C-01), NO network\n");
    printf("=========================================================\n");

    test_default_unchanged();
    test_normal_and_segmented_download();
    test_download_negative();
    test_ca();
    test_ca_negative();
    test_coe_delay();

    printf("\n=========================================================\n");
    printf(" RESULT: %d pass, %d fail\n", g_pass, g_fail);
    printf("=========================================================\n");
    return g_fail == 0 ? 0 : 1;
}
