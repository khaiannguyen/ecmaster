/* ==========================================================================
 * test_offline.c — Verifies ESC logic WITHOUT any network.
 *
 * Builds synthetic EtherCAT frames in memory, pushes them through
 * process_frame(), and compares results against expectations drawn from the
 * datasheets and from SOEM's own source (for the SII/PDO category format).
 * Runs on any machine with gcc: no veth, no socket, no root, no Jetson.
 *
 * Purpose: catch logic bugs BEFORE running against real SOEM/slaveinfo, so
 * that when an L1 case fails on real hardware, the failure can be
 * confidently attributed to the integration layer (socket/veth/SOEM)
 * rather than the datagram-processing logic.
 *
 * Build & run:  make test && ./test_offline
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_core.h"

static int g_pass = 0, g_fail = 0;

static void check(const char *name, long got, long want)
{
    if (got == want) {
        printf("  [PASS] %-52s = %ld\n", name, got);
        g_pass++;
    } else {
        printf("  [FAIL] %-52s = %ld (expected %ld)\n", name, got, want);
        g_fail++;
    }
}

static void check_hex(const char *name, unsigned long got, unsigned long want)
{
    if (got == want) {
        printf("  [PASS] %-52s = 0x%04lX\n", name, got);
        g_pass++;
    } else {
        printf("  [FAIL] %-52s = 0x%04lX (expected 0x%04lX)\n", name, got, want);
        g_fail++;
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static void     wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void     wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* Builds a frame containing exactly one datagram. Returns total frame length.
 * Layout: [Eth 14][EC hdr 2][DG hdr 10][data dlen][WKC 2] */
static size_t build_frame(uint8_t *buf, uint8_t cmd, uint16_t adp, uint16_t ado,
                          const uint8_t *data, uint16_t dlen)
{
    memset(buf, 0, 1600);
    memset(buf, 0xFF, 6);             /* destination: broadcast */
    buf[12] = 0x88; buf[13] = 0xA4;   /* EtherType: EtherCAT */

    uint16_t ec_len = (uint16_t)(DG_HDR_LEN + dlen + DG_WKC_LEN);
    wr16(buf + ETH_HDR_LEN, (uint16_t)(ec_len | (0x1 << 12))); /* Type=1 */

    uint8_t *dg = buf + ETH_HDR_LEN + EC_HDR_LEN;
    dg[0] = cmd;
    dg[1] = 0x00;                     /* Idx */
    wr16(dg + 2, adp);
    wr16(dg + 4, ado);
    wr16(dg + 6, (uint16_t)(dlen & 0x07FF)); /* Len, M=0 */
    wr16(dg + 8, 0x0000);             /* IRQ */

    if (data) memcpy(dg + DG_HDR_LEN, data, dlen);
    wr16(dg + DG_HDR_LEN + dlen, 0x0000); /* WKC starts at 0 */

    return ETH_HDR_LEN + EC_HDR_LEN + ec_len;
}

static uint8_t *dg_data(uint8_t *buf)  { return buf + ETH_HDR_LEN + EC_HDR_LEN + DG_HDR_LEN; }
static uint16_t dg_wkc(uint8_t *buf, uint16_t dlen) {
    return rd16(buf + ETH_HDR_LEN + EC_HDR_LEN + DG_HDR_LEN + dlen);
}

static esc_t *make_chain(int n, uint16_t pdo_size)
{
    esc_t *c = calloc((size_t)n, sizeof(esc_t));
    for (int i = 0; i < n; i++)
        esc_init(&c[i], (uint8_t)i, pdo_size);
    esc_chain_wire(c, n);
    return c;
}

/* ====================================================================== */
static void test_poweron_values(void)
{
    printf("\n[T1] Power-on values (Section II §2.1/§2.5)\n");
    esc_t *c = make_chain(1, 4);

    check_hex("0x0000 Type", c[0].regs[REG_TYPE], ESC_TYPE_CUSTOM_IPCORE);
    check("0x0004 FMMU supported", c[0].regs[REG_FMMU_SUPPORTED], 3);
    check("0x0005 SM supported",   c[0].regs[REG_SM_SUPPORTED], 4);
    check("0x0006 RAM size (KB)",  c[0].regs[REG_RAM_SIZE], 4);
    check_hex("0x0007 Port descriptor", c[0].regs[REG_PORT_DESCRIPTOR], ESC_PORTDESC_2ETH);
    check_hex("0x0008 ESC features (bit0=byte-wise FMMU)",
              rd16(c[0].regs + REG_ESC_FEATURES), ESC_FEATURE_FMMU_BYTEWISE);
    check_hex("0x0130 AL Status = INIT (not 0)",
              rd16(c[0].regs + REG_AL_STATUS), ESM_INIT);
    check_hex("0x0010 Station addr not yet configured", rd16(c[0].regs + REG_STATION_ADDR), 0x0000);
    free(c);
}

static void test_dl_status_topology(void)
{
    printf("\n[T2] DL Status by topology (Section II Table 2)\n");

    esc_t *c1 = make_chain(1, 4);
    /* Table 2: high byte 0x56 = only port0 linked, other ports no-link/closed */
    check_hex("N=1: 0x0111 (high byte), single node", c1[0].regs[REG_DL_STATUS + 1], 0x56);
    free(c1);

    esc_t *c4 = make_chain(4, 4);
    /* Table 2: 0x5A = port0 and port1 both linked, open */
    check_hex("N=4: node[0] is a middle-of-chain node", c4[0].regs[REG_DL_STATUS + 1], 0x5A);
    check_hex("N=4: node[2] still middle-of-chain",      c4[2].regs[REG_DL_STATUS + 1], 0x5A);
    check_hex("N=4: node[3] is the LAST node",            c4[3].regs[REG_DL_STATUS + 1], 0x56);
    free(c4);
}

static void test_brd_counts_all_nodes(void)
{
    printf("\n[T3] BRD: WKC = node count (SOEM's slave-counting mechanism)\n");
    uint8_t buf[1600];

    for (int n = 1; n <= 32; n *= 2) {
        esc_t *c = make_chain(n, 4);
        size_t flen = build_frame(buf, CMD_BRD, 0x0000, REG_TYPE, NULL, 2);
        process_frame(c, n, buf, flen);

        char name[80];
        snprintf(name, sizeof(name), "N=%-2d BRD 0x0000 -> WKC", n);
        check(name, dg_wkc(buf, 2), n);
        free(c);
    }
}

static void test_auto_increment_direction(void)
{
    printf("\n[T4] Auto-increment: ADP INCREMENTS through each node (Section I Table 7)\n");
    printf("     (old bug: decrementing ADP -> N=1 still passed, N>1 completely wrong)\n");
    uint8_t buf[1600];
    const int N = 4;

    /* Master assigns a station address to each node via APWR with
     * Position = -i (two's complement). Node i sees ADP == 0 when the
     * frame reaches it. */
    for (int i = 0; i < N; i++) {
        esc_t *c = make_chain(N, 4);
        uint16_t want_addr = (uint16_t)(0x1001 + i);
        uint8_t payload[2]; wr16(payload, want_addr);

        uint16_t adp = (uint16_t)(0 - i);   /* -i in two's complement */
        size_t flen = build_frame(buf, CMD_APWR, adp, REG_STATION_ADDR, payload, 2);
        process_frame(c, N, buf, flen);

        char name[80];
        snprintf(name, sizeof(name), "APWR ADP=-%d -> only node[%d] gets addressed", i, i);
        check_hex(name, rd16(c[i].regs + REG_STATION_ADDR), want_addr);

        int others_clean = 1;
        for (int k = 0; k < N; k++)
            if (k != i && rd16(c[k].regs + REG_STATION_ADDR) != 0) others_clean = 0;
        check("   other nodes were not written", others_clean, 1);

        check("   WKC = 1 (exactly one node addressed)", dg_wkc(buf, 2), 1);
        free(c);
    }
}

static void test_fprd_after_addressing(void)
{
    printf("\n[T5] FPRD after addressing (Section I §2.3)\n");
    uint8_t buf[1600];
    const int N = 4;
    esc_t *c = make_chain(N, 4);

    for (int i = 0; i < N; i++) {
        uint8_t payload[2]; wr16(payload, (uint16_t)(0x1001 + i));
        size_t flen = build_frame(buf, CMD_APWR, (uint16_t)(0 - i), REG_STATION_ADDR, payload, 2);
        process_frame(c, N, buf, flen);
    }

    size_t flen = build_frame(buf, CMD_FPRD, 0x1003, REG_TYPE, NULL, 1);
    process_frame(c, N, buf, flen);
    check("FPRD 0x1003 -> WKC", dg_wkc(buf, 1), 1);
    check_hex("FPRD 0x1003 -> reads back Type", dg_data(buf)[0], ESC_TYPE_CUSTOM_IPCORE);

    flen = build_frame(buf, CMD_FPRD, 0x9999, REG_TYPE, NULL, 1);
    process_frame(c, N, buf, flen);
    check("FPRD non-existent address -> WKC", dg_wkc(buf, 1), 0);
    free(c);
}

static void test_wkc_rules(void)
{
    printf("\n[T6] WKC rules (Section I Table 5)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    uint8_t payload[2] = {0xAA, 0xBB};
    size_t flen = build_frame(buf, CMD_BWR, 0x0000, REG_DPRAM_BASE, payload, 2);
    process_frame(c, 1, buf, flen);
    check("BWR (write)      -> WKC +1", dg_wkc(buf, 2), 1);

    flen = build_frame(buf, CMD_BRD, 0x0000, REG_DPRAM_BASE, NULL, 2);
    process_frame(c, 1, buf, flen);
    check("BRD (read)       -> WKC +1", dg_wkc(buf, 2), 1);

    flen = build_frame(buf, CMD_APRW, 0x0000, REG_DPRAM_BASE, payload, 2);
    process_frame(c, 1, buf, flen);
    check("APRW (read+write)-> WKC +3", dg_wkc(buf, 2), 3);
    free(c);
}

static void test_readwrite_data_direction(void)
{
    printf("\n[T7] RW commands: Data Out = OLD value, Data In = NEW value\n");
    printf("     (old bug: read/write shared a buffer -> the write became a no-op)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    uint8_t old_val[2]; wr16(old_val, 0x1111);
    size_t flen = build_frame(buf, CMD_BWR, 0x0000, REG_DPRAM_BASE, old_val, 2);
    process_frame(c, 1, buf, flen);

    uint8_t new_val[2]; wr16(new_val, 0x2222);
    flen = build_frame(buf, CMD_APRW, 0x0000, REG_DPRAM_BASE, new_val, 2);
    process_frame(c, 1, buf, flen);

    check_hex("Data Out returned to master = OLD value", rd16(dg_data(buf)), 0x1111);
    check_hex("Value stored in the ESC = NEW value from master",
              rd16(c[0].regs + REG_DPRAM_BASE), 0x2222);
    free(c);
}

static void test_sii_read(void)
{
    printf("\n[T8] Reading the SII EEPROM (Section II §2.11)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    uint8_t addr[2]; wr16(addr, 8);
    size_t flen = build_frame(buf, CMD_BWR, 0x0000, REG_SII_ADDRESS, addr, 2);
    process_frame(c, 1, buf, flen);

    flen = build_frame(buf, CMD_BRD, 0x0000, REG_SII_DATA, NULL, 2);
    process_frame(c, 1, buf, flen);
    check_hex("SII word 8 = Vendor ID (low)", rd16(dg_data(buf)),
              (unsigned)(SII_VENDOR_ID & 0xFFFF));

    wr16(addr, 10);
    flen = build_frame(buf, CMD_BWR, 0x0000, REG_SII_ADDRESS, addr, 2);
    process_frame(c, 1, buf, flen);
    flen = build_frame(buf, CMD_BRD, 0x0000, REG_SII_DATA, NULL, 2);
    process_frame(c, 1, buf, flen);
    check_hex("SII word 10 = Product Code (low)", rd16(dg_data(buf)),
              (unsigned)(SII_PRODUCT_CODE & 0xFFFF));
    free(c);
}

static void test_fmmu_logical(void)
{
    printf("\n[T9] FMMU + logical LRD/LWR commands (Section I §7)\n");
    uint8_t buf[1600];
    const int N = 2;
    esc_t *c = make_chain(N, 4);

    for (int i = 0; i < N; i++) {
        uint8_t *f = c[i].regs + REG_FMMU_BASE;
        wr32(f + FMMU_OFF_LOG_START, 0x00010000u + (uint32_t)(i * 4));
        wr16(f + FMMU_OFF_LENGTH, 4);
        f[FMMU_OFF_LOG_START_BIT] = 0;
        f[FMMU_OFF_LOG_STOP_BIT]  = 7;
        wr16(f + FMMU_OFF_PHYS_START, REG_DPRAM_BASE);
        f[FMMU_OFF_PHYS_START_BIT] = 0;
        f[FMMU_OFF_TYPE]     = 0x03; /* read + write */
        f[FMMU_OFF_ACTIVATE] = 0x01;
    }

    uint8_t pd[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint16_t adp = (uint16_t)(0x00010000u & 0xFFFF);
    uint16_t ado = (uint16_t)(0x00010000u >> 16);
    size_t flen = build_frame(buf, CMD_LWR, adp, ado, pd, 4);
    process_frame(c, N, buf, flen);

    check("LWR logical 0x10000 -> WKC", dg_wkc(buf, 4), 1);
    check_hex("node[0] DPRAM received the data", rd16(c[0].regs + REG_DPRAM_BASE), 0xADDE);
    check_hex("node[1] DPRAM was NOT touched", rd16(c[1].regs + REG_DPRAM_BASE), 0x0000);

    flen = build_frame(buf, CMD_LWR, 0x0000, 0x0099, pd, 4);
    process_frame(c, N, buf, flen);
    check("LWR address outside any FMMU -> WKC", dg_wkc(buf, 4), 0);
    free(c);
}

static void test_sii_pdo_category(void)
{
    printf("\n[T10] SII PDO category — reflects pdo_size_bytes (verified against SOEM src)\n");
    printf("      (this is the fix for L1-05/L1-06: Obits/Ibits used to read 0)\n");

    /* Categories must start exactly at word 64 (SOEM ec_type.h ECT_SII_START) */
    esc_t *c4 = make_chain(1, 4);
    check("Category start word == 64", SII_CATEGORY_START_WORD, 0x0040);
    check_hex("word[64] = TxPDO category type (50)",
              c4[0].sii_image_buf[64], SII_CAT_TXPDO);
    /* 1 entry (4 bytes = 32 bits fits in one entry) -> size = 4+4*1 = 8 words */
    check("word[65] = TxPDO category size (words)", c4[0].sii_image_buf[65], 8);
    check("word[74] = RxPDO category type (51)", c4[0].sii_image_buf[74], SII_CAT_RXPDO);
    check("word[75] = RxPDO category size (words)", c4[0].sii_image_buf[75], 8);
    /* [Phase 7] SyncManager category (41) follows the PDO categories:
     * 4 SMs x 4 words. SM2 = outputs at 0x1100 with watchdog trigger. */
    size_t smc = 74 + 2 + 8;
    check("SyncManager category type (41) after PDO categories",
          c4[0].sii_image_buf[smc], SII_CAT_SYNCM);
    check("SyncManager category size (words) = 16", c4[0].sii_image_buf[smc + 1], 16);
    check_hex("SM2 start = 0x1100", c4[0].sii_image_buf[smc + 2 + 2 * 4], SII_SM2_OFFSET);
    check("SM2 length = pdo_size", c4[0].sii_image_buf[smc + 2 + 2 * 4 + 1], 4);
    check_hex("SM2 control 0x64 (write, WD trigger), activate 1",
              c4[0].sii_image_buf[smc + 2 + 2 * 4 + 2] | (c4[0].sii_image_buf[smc + 2 + 2 * 4 + 3] << 8),
              0x0164);
    check_hex("SM3 start = 0x1108 (after SM2, 8-byte aligned)",
              c4[0].sii_image_buf[smc + 2 + 3 * 4], 0x1108);
    check_hex("End marker present after all categories",
              c4[0].sii_image_buf[smc + 2 + 16], SII_CAT_END);
    free(c4);

    /* pdo_size=64 needs 3 entries per direction (31+31+2 bytes) ->
     * size = 4 + 4*3 = 16 words each. This is exactly the case
     * run_l1_tests.sh CASE D relies on to force SOEM to split the frame. */
    esc_t *c64 = make_chain(1, 64);
    check("pdo_size=64: TxPDO size (words), 3 entries", c64[0].sii_image_buf[65], 16);
    check_hex("pdo_size=64: RxPDO category type still correct",
              c64[0].sii_image_buf[64 + 2 + 16], SII_CAT_RXPDO);
    free(c64);

    /* Phase 5 added a CoE/SDO server (esc_coe.c), so the SII now
     * ADVERTISES CoE. This assertion expected the bit cleared until Phase 5
     * and stayed stale (68/69) until Phase 7. */
    esc_t *c1 = make_chain(1, 4);
    check("Mailbox protocol word: CoE bit set (CoE server present)",
          c1[0].sii_image_buf[28] & SII_MBX_PROTOCOL_COE, SII_MBX_PROTOCOL_COE);
    free(c1);
}

/* Small local helper: build+process one AL Control request through the
 * whole chain, addressed via FPWR at the given station address. */
static void req_al_state(esc_t *c, int n, uint8_t buf[1600],
                          uint16_t station_addr, uint16_t creq)
{
    uint8_t payload[2];
    wr16(payload, creq);
    size_t flen = build_frame(buf, CMD_FPWR, station_addr, REG_AL_CONTROL, payload, 2);
    process_frame(c, n, buf, flen);
}

static void test_esm_sequential_valid_transitions(void)
{
    printf("\n[T11] ESM: INIT -> PREOP -> SAFEOP -> OP (L2-01..03)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    /* Assign station address 0x1001 (same convention as test_fprd_after_addressing) */
    uint8_t addr_payload[2];
    wr16(addr_payload, 0x1001);
    size_t flen = build_frame(buf, CMD_APWR, 0x0000, REG_STATION_ADDR, addr_payload, 2);
    process_frame(c, 1, buf, flen);

    check_hex("power-on AL status = INIT", rd16(c[0].regs + REG_AL_STATUS), ESM_INIT);

    req_al_state(c, 1, buf, 0x1001, ESM_PREOP);
    check_hex("AL status after INIT->PREOP", rd16(c[0].regs + REG_AL_STATUS), ESM_PREOP);
    check_hex("AL status code after INIT->PREOP", rd16(c[0].regs + REG_AL_STATUS_CODE), ALSTATUSCODE_NOERROR);

    req_al_state(c, 1, buf, 0x1001, ESM_SAFEOP);
    check_hex("AL status after PREOP->SAFEOP", rd16(c[0].regs + REG_AL_STATUS), ESM_SAFEOP);

    /* Feed one valid output write into this node's FMMU range before
     * requesting OP — otherwise SAFEOP->OP must be rejected (that's T12). */
    uint8_t *f = c[0].regs + REG_FMMU_BASE;
    wr32(f + FMMU_OFF_LOG_START, 0x00020000u);
    wr16(f + FMMU_OFF_LENGTH, 4);
    f[FMMU_OFF_LOG_START_BIT] = 0;
    f[FMMU_OFF_LOG_STOP_BIT]  = 7;
    wr16(f + FMMU_OFF_PHYS_START, REG_DPRAM_BASE);
    f[FMMU_OFF_TYPE]     = 0x02; /* write only */
    f[FMMU_OFF_ACTIVATE] = 0x01;

    uint8_t pd[4] = {0, 0, 0, 0};
    flen = build_frame(buf, CMD_LWR, 0x0000, 0x0002, pd, 4);
    process_frame(c, 1, buf, flen);
    check("got_valid_outputs set after LWR into active output FMMU", c[0].got_valid_outputs, 1);

    req_al_state(c, 1, buf, 0x1001, ESM_OP);
    check_hex("AL status after SAFEOP->OP (with valid outputs)", rd16(c[0].regs + REG_AL_STATUS), ESM_OP);
    check_hex("AL status code after SAFEOP->OP", rd16(c[0].regs + REG_AL_STATUS_CODE), ALSTATUSCODE_NOERROR);

    free(c);
}

static void test_esm_safeop_to_op_without_outputs(void)
{
    printf("\n[T12] ESM: SAFEOP -> OP without valid outputs first (L2-04)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    uint8_t addr_payload[2];
    wr16(addr_payload, 0x1001);
    size_t flen = build_frame(buf, CMD_APWR, 0x0000, REG_STATION_ADDR, addr_payload, 2);
    process_frame(c, 1, buf, flen);

    req_al_state(c, 1, buf, 0x1001, ESM_PREOP);
    req_al_state(c, 1, buf, 0x1001, ESM_SAFEOP);
    check("got_valid_outputs is 0 right after entering SAFEOP", c[0].got_valid_outputs, 0);

    req_al_state(c, 1, buf, 0x1001, ESM_OP);
    check_hex("AL status stays SAFEOP+ERROR (rejected)", rd16(c[0].regs + REG_AL_STATUS), (ESM_SAFEOP | 0x10));
    check_hex("AL status code = NOVALIDOUTPUTS", rd16(c[0].regs + REG_AL_STATUS_CODE), ALSTATUSCODE_NOVALIDOUTPUTS);

    free(c);
}

static void test_esm_forced_reject(void)
{
    printf("\n[T13] ESM: soft_bus forces rejection of OP request (L2-05)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    uint8_t addr_payload[2];
    wr16(addr_payload, 0x1001);
    size_t flen = build_frame(buf, CMD_APWR, 0x0000, REG_STATION_ADDR, addr_payload, 2);
    process_frame(c, 1, buf, flen);

    req_al_state(c, 1, buf, 0x1001, ESM_PREOP);
    req_al_state(c, 1, buf, 0x1001, ESM_SAFEOP);

    c[0].force_reject_al = 1;
    req_al_state(c, 1, buf, 0x1001, ESM_OP);
    check_hex("AL status stays SAFEOP+ERROR (forced reject)", rd16(c[0].regs + REG_AL_STATUS), (ESM_SAFEOP | 0x10));
    check_hex("AL status code = UNKNOWNALCONTROL", rd16(c[0].regs + REG_AL_STATUS_CODE), ALSTATUSCODE_UNKNOWNALCONTROL);
    check("force_reject_al is one-shot (cleared after use)", c[0].force_reject_al, 0);

    free(c);
}

static void test_esm_invalid_direct_jump(void)
{
    printf("\n[T14] ESM: direct INIT -> SAFEOP jump is rejected (L2-06)\n");
    uint8_t buf[1600];
    esc_t *c = make_chain(1, 4);

    uint8_t addr_payload[2];
    wr16(addr_payload, 0x1001);
    size_t flen = build_frame(buf, CMD_APWR, 0x0000, REG_STATION_ADDR, addr_payload, 2);
    process_frame(c, 1, buf, flen);

    check_hex("starts at INIT", rd16(c[0].regs + REG_AL_STATUS), ESM_INIT);

    req_al_state(c, 1, buf, 0x1001, ESM_SAFEOP);
    check_hex("AL status stays INIT+ERROR (skip rejected)", rd16(c[0].regs + REG_AL_STATUS), (ESM_INIT | 0x10));
    check_hex("AL status code = INVALIDALCONTROL", rd16(c[0].regs + REG_AL_STATUS_CODE), ALSTATUSCODE_INVALIDALCONTROL);

    free(c);
}

int main(void)
{
    printf("=========================================================\n");
    printf(" test_offline — verifies ESC logic, NO network required\n");
    printf("=========================================================\n");

    test_poweron_values();
    test_dl_status_topology();
    test_brd_counts_all_nodes();
    test_auto_increment_direction();
    test_fprd_after_addressing();
    test_wkc_rules();
    test_readwrite_data_direction();
    test_sii_read();
    test_fmmu_logical();
    test_sii_pdo_category();

    test_esm_sequential_valid_transitions();
    test_esm_safeop_to_op_without_outputs();
    test_esm_forced_reject();
    test_esm_invalid_direct_jump();

    printf("\n=========================================================\n");
    printf(" RESULT: %d pass, %d fail\n", g_pass, g_fail);
    printf("=========================================================\n");
    return g_fail == 0 ? 0 : 1;
}
