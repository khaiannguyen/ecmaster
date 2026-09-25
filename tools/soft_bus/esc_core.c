/* ==========================================================================
 * esc_core.c — ESC logic layer. Does NOT touch sockets/network: receives a
 * frame buffer in memory, mutates it in place, done. This is what makes it
 * testable fully offline (test_offline.c), with no veth, no root, no
 * physical hardware involved.
 *
 * Known limitations of this implementation (stated, not hidden):
 *  - FMMU only handles byte-aligned mappings (ignores LogicalStartBit/
 *    StopBit, PhysicalStartBit). Sufficient for the current L1 test suite
 *    since PDOs are byte-sized. Declared officially via ESC features
 *    register 0x0008 bit0=1 (byte-oriented FMMU) — see esc_init().
 *  - AL Control/Status (ESM state transitions) not implemented — that is
 *    Phase 3 scope (state machine).
 *  - Distributed Clocks (0x0900+) not implemented — Phase 4 scope.
 *  - SM watchdog: [RESOLVED, Giai doan 7] process data watchdog modelled
 *    (0x0400/0x0420/0x0440/0x0442, trigger on complete writes to SMs with
 *    control bit6) — see esc_wd_check(). PDI watchdog is not modelled.
 *  - Error counters 0x0300..0x0313: [Giai doan 7] clear-on-write groups and
 *    saturation modelled; they are only incremented by fault injection
 *    (esc_fault.c), since a software bus has no physical layer errors.
 *  - No CoE SDO mailbox server: [RESOLVED, Giai doan 5] esc_coe.c now
 *    implements one on SM0/SM1 (see esc_coe.h for scope: expedited +
 *    segmented upload/download, Abort for unknown objects; no SDO Info,
 *    no Emergency, no Complete Access).
 *  - Reserved register regions accept writes freely (a real ESC would
 *    reject/ignore them) — accepted trade-off, see project notes.
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_core.h"
#include "esc_coe.h"
#include "esc_dc.h"

/* ---- Little-endian helpers, independent of host endianness ---- */
static inline uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ==========================================================================
 * esc_build_sii — generates a PER-NODE SII image reflecting pdo_size_bytes.
 *
 * Verified directly against SOEM 2.0.0 source (src/ec_main.c ecx_siiPDO(),
 * ecx_siifind()) rather than assumed from the datasheet alone:
 *  - ECT_SII_START = 0x0040 (word 64): categories MUST start exactly there.
 *  - A category is: Type (word) + SizeInWords (word) + body.
 *  - A PDO category body is: Index(2) NumEntries(1) SyncM(1) Sync(1)
 *    NameIdx(1) Flags(2) [8 bytes], followed by NumEntries x
 *    { Index(2) SubIndex(1) NameIdx(1) DataType(1) BitLen(1) Flags(2) }
 *    [8 bytes each]. SizeInWords must equal the TRUE word count of this
 *    body (both SOEM's outer category-skip and its inner PDO-loop
 *    termination depend on it being exact — verified by hand-tracing
 *    ecx_siiPDO()'s "c" counter against the byte layout it consumes).
 *  - End of category list is a single word 0xFFFF, no body.
 *
 * KNOWN LIMITATION: SOEM's ecx_lookup_mapping() copies the PDO mapping from
 * an earlier slave with the SAME Vendor/Product/Revision instead of
 * re-reading SII for every slave. That is harmless today (every node uses
 * the same --pdo-size), but if a future scenario needs GENUINELY different
 * PDO sizes across nodes while keeping them "the same slave model", the
 * Revision field (SII word 12-13) must differ per distinct configuration,
 * or SOEM will silently reuse node[0]'s size for all of them.
 * ========================================================================== */
static size_t append_pdo_category(uint16_t *out, size_t pos, size_t cap,
                                   uint16_t cat_type, uint16_t pdo_index,
                                   uint8_t sm_index, uint16_t total_bytes)
{
    if (total_bytes == 0) return pos;

    uint8_t  chunks[64];
    int      n_entries = 0;
    uint16_t remaining  = total_bytes;
    while (remaining > 0 && n_entries < 64) {
        uint8_t chunk = remaining > SII_PDO_ENTRY_MAX_BYTES
                       ? SII_PDO_ENTRY_MAX_BYTES : (uint8_t)remaining;
        chunks[n_entries++] = chunk;
        remaining = (uint16_t)(remaining - chunk);
    }

    size_t body_bytes  = 8 + 8 * (size_t)n_entries;
    size_t size_words  = body_bytes / 2;
    size_t words_needed = 2 + size_words; /* Type + Size + body */
    if (pos + words_needed > cap) return pos; /* out of room, skip safely */

    uint8_t body[8 + 8 * 64];
    size_t  b = 0;
    body[b++] = (uint8_t)(pdo_index & 0xFF);
    body[b++] = (uint8_t)(pdo_index >> 8);
    body[b++] = (uint8_t)n_entries;
    body[b++] = sm_index;
    body[b++] = 0; /* Synchronization (DC-related, unused here) */
    body[b++] = 0; /* Name index */
    body[b++] = 0; body[b++] = 0; /* Object flags */
    for (int i = 0; i < n_entries; i++) {
        body[b++] = 0; body[b++] = 0;          /* Entry index (unused here) */
        body[b++] = 0;                          /* SubIndex */
        body[b++] = 0;                          /* Name index */
        body[b++] = 0x05;                       /* DataType = UNSIGNED8 */
        body[b++] = (uint8_t)(chunks[i] * 8);   /* BitLen */
        body[b++] = 0; body[b++] = 0;           /* Flags */
    }

    out[pos++] = cat_type;
    out[pos++] = (uint16_t)size_words;
    for (size_t i = 0; i < b; i += 2)
        out[pos++] = (uint16_t)(body[i] | (body[i + 1] << 8));

    return pos;
}

/* SII SyncManager category (type 41): 8 bytes per SM, layout as parsed by
 * SOEM ecx_siiSM()/ecx_siiSMnext(): PhStart(2) Length(2) Control(1)
 * Status(1) Activate(1) PDIControl(1). nSM = SizeInWords / 4. */
static size_t append_sm_category(uint16_t *out, size_t pos, size_t cap,
                                 uint16_t pdo_size_bytes)
{
    uint16_t sm3_start = (uint16_t)(SII_SM2_OFFSET +
        ((pdo_size_bytes + SII_PD_SM_ALIGN - 1) / SII_PD_SM_ALIGN) * SII_PD_SM_ALIGN);
    struct { uint16_t start, len; uint8_t ctrl, act; } sm[4] = {
        { SII_SM0_OFFSET, SII_SM0_SIZE,   SII_SM0_CONTROL, 1 },
        { SII_SM1_OFFSET, SII_SM1_SIZE,   SII_SM1_CONTROL, 1 },
        { SII_SM2_OFFSET, pdo_size_bytes, SII_SM2_CONTROL, pdo_size_bytes ? 1 : 0 },
        { sm3_start,      pdo_size_bytes, SII_SM3_CONTROL, pdo_size_bytes ? 1 : 0 },
    };
    if (pos + 2 + 16 > cap) return pos;
    out[pos++] = SII_CAT_SYNCM;
    out[pos++] = 16;                       /* 4 SM x 8 bytes = 16 words */
    for (int i = 0; i < 4; i++) {
        out[pos++] = sm[i].start;
        out[pos++] = sm[i].len;
        out[pos++] = (uint16_t)(sm[i].ctrl | (0u << 8));   /* control | status */
        out[pos++] = (uint16_t)(sm[i].act  | (0u << 8));   /* activate | PDI ctrl */
    }
    return pos;
}

static void esc_build_sii(esc_t *esc, uint16_t pdo_size_bytes)
{
    size_t words = 0;

    for (size_t i = 0; i < SII_IMAGE_DEFAULT_WORDS && i < ESC_SII_IMAGE_MAX_WORDS; i++)
        esc->sii_image_buf[i] = g_sii_image_default[i];
    words = SII_IMAGE_DEFAULT_WORDS;

    /* CoE bit is left SET (as g_sii_image_default already has it) now that
     * esc_coe.c implements a real CoE/SDO server on SM0/SM1 — previously
     * cleared here on purpose (see esc_sii.h's word-28 comment, updated
     * to match). */

    /* Pad reserved words up to the mandatory category start at word 64. */
    while (words < SII_CATEGORY_START_WORD && words < ESC_SII_IMAGE_MAX_WORDS)
        esc->sii_image_buf[words++] = 0x0000;

    /* Symmetric in/out size: one --pdo-size CLI value covers both
     * directions. TxPDO (0x1A00) = slave->master (Input, SM3);
     * RxPDO (0x1600) = master->slave (Output, SM2). */
    words = append_pdo_category(esc->sii_image_buf, words, ESC_SII_IMAGE_MAX_WORDS,
                                 SII_CAT_TXPDO, 0x1A00, 3, pdo_size_bytes);
    words = append_pdo_category(esc->sii_image_buf, words, ESC_SII_IMAGE_MAX_WORDS,
                                 SII_CAT_RXPDO, 0x1600, 2, pdo_size_bytes);
    /* After the PDO categories so the word-64 layout checked by T10 is
     * unchanged; SOEM finds categories by type, not by position. */
    words = append_sm_category(esc->sii_image_buf, words, ESC_SII_IMAGE_MAX_WORDS,
                               pdo_size_bytes);

    if (words < ESC_SII_IMAGE_MAX_WORDS)
        esc->sii_image_buf[words++] = SII_CAT_END;

    esc->sii_image_words = words;
}

/* ==========================================================================
 * esc_init — initializes everything INDEPENDENT of topology. Power-on
 * values checked against Section II §2.1/§2.5.
 * ========================================================================== */
void esc_init(esc_t *esc, uint8_t position_in_chain, uint16_t pdo_size_bytes)
{
    memset(esc->regs, 0, ESC_REG_SPACE_SIZE);

    esc->position_in_chain = position_in_chain;
    esc->pdo_size_bytes    = pdo_size_bytes;

    /* Station address defaults to 0 (matches real unconfigured hardware) —
     * NOT baked in as 0x1000+i, so L1-03 is a real test of the master
     * assigning the address via APWR, not a tautology. */
    esc->station_address = 0x0000;
    esc->station_alias   = 0x0000;
    esc->alias_enabled   = 0;

    esc->regs[REG_TYPE]            = ESC_TYPE_CUSTOM_IPCORE;
    esc->regs[REG_REVISION]        = 0x00;
    esc->regs[REG_FMMU_SUPPORTED]  = REG_FMMU_COUNT;
    esc->regs[REG_SM_SUPPORTED]    = REG_SM_COUNT;
    esc->regs[REG_RAM_SIZE]        = 4; /* KB, matches LAN9252 */
    esc->regs[REG_PORT_DESCRIPTOR] = ESC_PORTDESC_2ETH;

    /* Officially declare the byte-oriented-FMMU limitation via the protocol
     * itself, rather than leaving it as an implicit comment. */
    wr_le16(esc->regs + REG_ESC_FEATURES, ESC_FEATURE_FMMU_BYTEWISE);

    /* Reset value = 1 (INIT), NOT 0 — 0 is not a valid ESM state. */
    wr_le16(esc->regs + REG_AL_CONTROL, ESM_INIT);
    wr_le16(esc->regs + REG_AL_STATUS,  ESM_INIT);

    /* Watchdog reset values (Section II §2.10.1/§2.10.4): 100 us tick x
     * 1000 = 100 ms. Status 0x0440 resets to 0 ("expired") until the first
     * trigger. The slave-application reaction (OP -> SAFEOP+ERR 0x001B) is
     * on by default, like a real SSC-based slave; soft_bus --no-sm-wd
     * turns it off. */
    wr_le16(esc->regs + REG_WD_DIVIDER, WD_DIVIDER_RESET);
    wr_le16(esc->regs + REG_WD_TIME_PDI0, WD_TIME_RESET);
    wr_le16(esc->regs + REG_WD_TIME_PROCDATA, WD_TIME_RESET);
    memset(&esc->wd, 0, sizeof(esc->wd));
    esc->wd.react = 1;
    memset(&esc->fault, 0, sizeof(esc->fault));

    esc_build_sii(esc, pdo_size_bytes);

    coe_od_init(&esc->coe_od);
    /* esc->coe_session is already all-zero (esc_t instances come from
     * calloc() in soft_bus_main.c), so coe_session.active starts at 0
     * without needing an explicit reset here. */

    /* DL Status is filled in by esc_chain_wire() — depends on position. */
}

/* ==========================================================================
 * esc_chain_wire — link state is a relationship BETWEEN nodes, so it must
 * be set after the whole chain is known. Model: a straight chain, each node
 * uses port 0 (upstream) and port 1 (downstream).
 * ========================================================================== */
void esc_chain_wire(esc_t *chain, int n)
{
    /* [Phase 7] A node that is powered off (drop_node) takes its links down:
     * the neighbour's port facing it loses link and its loop closes, which
     * is what makes the frame turn around early. With no node powered off
     * this gives exactly the Phase 1 values. */
    for (int i = 0; i < n; i++) {
        uint16_t dl = DLSTAT_PDI_OPERATIONAL | DLSTAT_PDI_WD_OK;
        int link0 = (i == 0) || !chain[i - 1].fault.powered_off;   /* node 0: master */
        int link1 = (i < n - 1) && !chain[i + 1].fault.powered_off;

        if (link0) dl |= DLSTAT_LINK_PORT0 | DLSTAT_COMM_PORT0;
        else       dl |= DLSTAT_LOOP_PORT0;

        if (link1) {
            dl |= DLSTAT_LINK_PORT1 | DLSTAT_COMM_PORT1; /* more nodes downstream */
        } else {
            /* LAST node: port1 not linked, loop CLOSED — this is the
             * physical mechanism that turns the frame around at chain end. */
            dl |= DLSTAT_LOOP_PORT1;
        }
        dl |= DLSTAT_LOOP_PORT2 | DLSTAT_LOOP_PORT3; /* ports 2/3 don't exist */

        wr_le16(chain[i].regs + REG_DL_STATUS, dl);
    }
}

/* ==========================================================================
 * Physical register access (bounds-checked) + SII side effects
 * ========================================================================== */
static void esc_sii_refresh_data(esc_t *esc)
{
    uint16_t word_addr = rd_le16(esc->regs + REG_SII_ADDRESS);
    uint16_t v0 = 0, v1 = 0;
    if (word_addr < esc->sii_image_words)
        v0 = esc->sii_image_buf[word_addr];
    if ((size_t)(word_addr + 1) < esc->sii_image_words)
        v1 = esc->sii_image_buf[word_addr + 1];
    wr_le16(esc->regs + REG_SII_DATA, v0);
    wr_le16(esc->regs + REG_SII_DATA + 2, v1);
}

/* Plain read (no OR) — used for APRD/FPRD/L*RD, overwrites `data` directly. */
static void esc_phys_read(esc_t *esc, uint16_t phys_offset, uint8_t *data, uint16_t len)
{
    if (phys_offset + (uint32_t)len > ESC_REG_SPACE_SIZE) return;
    esc_dc_before_read(esc, phys_offset, len);

    if (phys_offset < REG_SII_DATA + 4 && phys_offset + len > REG_SII_DATA) {
        esc_sii_refresh_data(esc);
    }
    memcpy(data, esc->regs + phys_offset, len);

    /* Reading (any part of) SM1's mailbox-in DPRAM clears "mailbox full" —
     * mirrors real ESC hardware auto-clearing the flag once the master
     * has fetched the response. Master always reads the WHOLE mbx_l=128
     * byte SM1 buffer in one FPRD (ecx_mbxinhandler's ecx_FPRD call),
     * confirmed by reading ec_main.c, so a single "touches this range"
     * check is enough — no partial-read bookkeeping needed. */
    if (phys_offset < SII_SM1_OFFSET + SII_SM1_SIZE && phys_offset + len > SII_SM1_OFFSET) {
        if (esc->regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL)
            esc->fault.sm1_consumed = 1;   /* a response was fetched (fault hooks) */
        esc->regs[REG_SM1_STATUS] &= (uint8_t)~SM_STATUS_MAILBOX_FULL;
    }
}

/* OR-accumulating read — REQUIRED for BRD/BRW per spec: multiple slaves OR
 * their data into the frame as it passes through each of them in turn. */
static void esc_phys_read_or(esc_t *esc, uint16_t phys_offset, uint8_t *data, uint16_t len)
{
    if (phys_offset + (uint32_t)len > ESC_REG_SPACE_SIZE) return;
    esc_dc_before_read(esc, phys_offset, len);
    if (phys_offset < REG_SII_DATA + 4 && phys_offset + len > REG_SII_DATA) {
        esc_sii_refresh_data(esc);
    }
    for (uint16_t k = 0; k < len; k++) {
        data[k] |= esc->regs[phys_offset + k];
    }

    if (phys_offset < SII_SM1_OFFSET + SII_SM1_SIZE && phys_offset + len > SII_SM1_OFFSET) {
        if (esc->regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL)
            esc->fault.sm1_consumed = 1;   /* a response was fetched (fault hooks) */
        esc->regs[REG_SM1_STATUS] &= (uint8_t)~SM_STATUS_MAILBOX_FULL;
    }
}

static inline int range_hits(uint16_t off, uint16_t len, uint16_t lo, uint16_t hi)
{
    return off <= hi && (uint32_t)off + len > lo;   /* [off, off+len) meets [lo, hi] */
}

void esc_cnt_inc(esc_t *esc, uint16_t reg)
{
    if (esc->regs[reg] < 0xFF) esc->regs[reg]++;   /* all ESC counters saturate */
}

/* 1 if an active, ECAT-write SyncManager with watchdog trigger enable exists. */
static int esc_has_wd_trigger_sm(const esc_t *esc)
{
    for (int k = 0; k < REG_SM_COUNT; k++) {
        const uint8_t *sm = esc->regs + REG_SM_BASE + k * REG_SM_ENTRY_SIZE;
        if ((sm[SM_OFF_ACTIVATE] & SM_ACT_ENABLE) && (sm[SM_OFF_CONTROL] & SM_CTRL_WD_TRIGGER)
            && (sm[SM_OFF_CONTROL] & SM_CTRL_DIR_MASK) == SM_CTRL_DIR_WRITE
            && rd_le16(sm + SM_OFF_LENGTH) > 0)
            return 1;
    }
    return 0;
}

/* Watchdog trigger (Section I §13.1): "generated after the buffer was
 * completely and successfully written" -> modelled as a write that covers
 * the last byte of the SM buffer. */
static void esc_wd_on_write(esc_t *esc, uint16_t off, uint16_t len)
{
    for (int k = 0; k < REG_SM_COUNT; k++) {
        const uint8_t *sm = esc->regs + REG_SM_BASE + k * REG_SM_ENTRY_SIZE;
        uint16_t start = rd_le16(sm + SM_OFF_PHYS_START);
        uint16_t smlen = rd_le16(sm + SM_OFF_LENGTH);
        if (!(sm[SM_OFF_ACTIVATE] & SM_ACT_ENABLE) || !(sm[SM_OFF_CONTROL] & SM_CTRL_WD_TRIGGER)
            || (sm[SM_OFF_CONTROL] & SM_CTRL_DIR_MASK) != SM_CTRL_DIR_WRITE || smlen == 0)
            continue;
        uint16_t last = (uint16_t)(start + smlen - 1);
        if (range_hits(off, len, last, last)) {
            esc->wd.last_trigger_ns = esc->wd.now_ns;
            esc->wd.running = 1;
            esc->regs[REG_WD_STATUS_PD] |= 0x01;
        }
    }
}

int esc_wd_check(esc_t *esc, uint64_t now_ns)
{
    esc->wd.now_ns = now_ns;
    uint16_t wd_time = rd_le16(esc->regs + REG_WD_TIME_PROCDATA);
    if (wd_time == 0) {                       /* disabled -> "active or disabled" */
        esc->regs[REG_WD_STATUS_PD] |= 0x01;
        return 0;
    }
    if (esc->wd.running) {
        uint64_t tick_ns = ((uint64_t)rd_le16(esc->regs + REG_WD_DIVIDER) + 2u) * 40u;
        uint64_t timeout = tick_ns * wd_time;
        if (now_ns - esc->wd.last_trigger_ns > timeout) {
            esc->wd.running = 0;
            esc->regs[REG_WD_STATUS_PD] &= (uint8_t)~0x01;
            esc_cnt_inc(esc, REG_WD_COUNTER_PD);
            esc->wd.expire_events++;
        }
    }
    /* Slave application part (what an SSC-based slave does): in OP, an
     * expired process data watchdog forces SAFEOP + ERR, code 0x001B. */
    uint8_t state = esc->regs[REG_AL_STATUS] & 0x0F;
    if (esc->wd.react && state == ESM_OP && !(esc->regs[REG_WD_STATUS_PD] & 0x01)
        && esc_has_wd_trigger_sm(esc)) {
        wr_le16(esc->regs + REG_AL_STATUS, (uint16_t)(ESM_SAFEOP | 0x10));
        wr_le16(esc->regs + REG_AL_STATUS_CODE, ALSTATUSCODE_SYNCMANWATCHDOG);
        esc->got_valid_outputs = 0;
        return 1;
    }
    return 0;
}

static void esc_phys_write(esc_t *esc, uint16_t phys_offset, const uint8_t *data, uint16_t len)
{
    if (phys_offset + (uint32_t)len > ESC_REG_SPACE_SIZE) return;

    /* Registers that are read-only for ECAT: SM status (+5) and SM PDI
     * control (+7) of every SM, watchdog status 0x0440:0x0441. SOEM writes
     * whole 8-byte SM entries (and 0x080D:0x080E for a mailbox repeat
     * request), so without this the repeat-ack bit in 0x080F and the
     * mailbox-full flag would be overwritten by the master. */
    uint8_t ro_save[REG_SM_COUNT][2], wd_st_save[2], sm1_act_old;
    for (int k = 0; k < REG_SM_COUNT; k++) {
        ro_save[k][0] = esc->regs[REG_SM_BASE + k * REG_SM_ENTRY_SIZE + SM_OFF_STATUS];
        ro_save[k][1] = esc->regs[REG_SM_BASE + k * REG_SM_ENTRY_SIZE + SM_OFF_PDI_CONTROL];
    }
    wd_st_save[0] = esc->regs[REG_WD_STATUS_PD];
    wd_st_save[1] = esc->regs[REG_WD_STATUS_PD + 1];
    sm1_act_old   = esc->regs[REG_SM_BASE + REG_SM_ENTRY_SIZE + SM_OFF_ACTIVATE];

    memcpy(esc->regs + phys_offset, data, len);
    esc_dc_after_write(esc, phys_offset, data, len);

    for (int k = 0; k < REG_SM_COUNT; k++) {
        esc->regs[REG_SM_BASE + k * REG_SM_ENTRY_SIZE + SM_OFF_STATUS]      = ro_save[k][0];
        esc->regs[REG_SM_BASE + k * REG_SM_ENTRY_SIZE + SM_OFF_PDI_CONTROL] = ro_save[k][1];
    }
    esc->regs[REG_WD_STATUS_PD]     = wd_st_save[0];
    esc->regs[REG_WD_STATUS_PD + 1] = wd_st_save[1];

    /* Error counters are "w(clr)": the written value is ignored and a
     * whole group is cleared (Section II §2.9.1/2.9.2/2.9.6, §2.10.6). */
    if (range_hits(phys_offset, len, 0x0300, 0x030B)) {
        memset(esc->regs + 0x0300, 0, 0x030C - 0x0300);
        memset(esc->regs + 0x0314, 0, 4);
        memset(esc->regs + 0x0320, 0, 8);
    }
    if (range_hits(phys_offset, len, REG_ERR_ECAT_PU, REG_ERR_ECAT_PU))
        esc->regs[REG_ERR_ECAT_PU] = 0;
    if (range_hits(phys_offset, len, REG_ERR_PDI, REG_ERR_PDI))
        memset(esc->regs + REG_ERR_PDI, 0, 3);          /* counter + error code */
    if (range_hits(phys_offset, len, 0x0310, 0x0313))
        memset(esc->regs + 0x0310, 0, 4);
    if (range_hits(phys_offset, len, 0x0442, 0x0444))
        memset(esc->regs + 0x0442, 0, 3);

    /* Mailbox repeat (ETG.1000.4 robust mailbox, SOEM ec_main.c): the master
     * toggles SM1 activate bit1; the slave puts its last response back into
     * SM1 (DPRAM still holds it) and mirrors the toggle into PDI control
     * bit1 as the acknowledge. */
    {
        uint16_t act1 = REG_SM_BASE + REG_SM_ENTRY_SIZE + SM_OFF_ACTIVATE;       /* 0x080E */
        uint16_t pdi1 = REG_SM_BASE + REG_SM_ENTRY_SIZE + SM_OFF_PDI_CONTROL;    /* 0x080F */
        if (range_hits(phys_offset, len, act1, act1) &&
            ((esc->regs[act1] ^ sm1_act_old) & SM_ACT_REPEAT_REQ)) {
            esc->regs[REG_SM1_STATUS] |= SM_STATUS_MAILBOX_FULL;
            esc->regs[pdi1] = (uint8_t)((esc->regs[pdi1] & ~SM_PDI_REPEAT_ACK)
                                        | (esc->regs[act1] & SM_PDI_REPEAT_ACK));
            esc->fault.mbx_repeats_served++;
        }
    }

    esc_wd_on_write(esc, phys_offset, len);

    /* Force the busy bit low right after writing control/status — models
     * "completes instantly", since this simulator has no real EEPROM delay. */
    if (phys_offset <= REG_SII_CONTROL_STATUS + 1 &&
        phys_offset + len > REG_SII_CONTROL_STATUS + 1) {
        esc->regs[REG_SII_CONTROL_STATUS + 1] &= (uint8_t)~SII_BUSY_BIT_MASK;
    }

    if (phys_offset <= REG_STATION_ADDR &&
        phys_offset + len >= REG_STATION_ADDR + 2) {
        esc->station_address = rd_le16(esc->regs + REG_STATION_ADDR);
    }

    if (phys_offset <= REG_STATION_ALIAS &&
        phys_offset + len >= REG_STATION_ALIAS + 2) {
        esc->station_alias = rd_le16(esc->regs + REG_STATION_ALIAS);
    }

    if (phys_offset <= REG_DL_CONTROL_ALIAS_BYTE &&
        phys_offset + len > REG_DL_CONTROL_ALIAS_BYTE) {
        esc->alias_enabled =
            (esc->regs[REG_DL_CONTROL_ALIAS_BYTE] & DLCTRL_ALIAS_ENABLE_BIT) ? 1 : 0;
    }

    /* Giai doan 5: a write landing anywhere in SM0's mailbox-out DPRAM
     * range is a fresh CoE/SDO request from the master. Processed
     * synchronously right here, still inside this same FPWR/FPRW's
     * datagram handling -- soft_bus has no separate slave-side polling
     * loop, so there is no reason to defer it. The response is ready in
     * SM1 for the master's OWN next cyclic poll (ecx_mbxhandler on the
     * RT thread), matching real hardware's "at least one cycle later"
     * timing without needing to simulate it. */
    if (phys_offset < SII_SM0_OFFSET + SII_SM0_SIZE && phys_offset + len > SII_SM0_OFFSET) {
        coe_on_mailbox_out_write(esc);
    }
}

/* Section I §2.3: FP* commands match if the address equals the configured
 * station address OR the configured station alias (once the master has
 * enabled alias matching). */
static int esc_addr_match(const esc_t *esc, uint16_t adp)
{
    if (adp == esc->station_address) return 1;
    if (esc->alias_enabled && adp == esc->station_alias) return 1;
    return 0;
}

/* ==========================================================================
 * fmmu_apply — applies a logical (L*) command across EVERY active FMMU that
 * overlaps it, not just the first one found. A single LRW commonly spans
 * multiple FMMU regions at once (e.g. one write-only FMMU for outputs, one
 * read-only FMMU for inputs, packed by SOEM into one combined datagram) —
 * the old fmmu_lookup() required FULL containment in a SINGLE FMMU, so any
 * datagram spanning more than one FMMU silently matched nothing at all.
 *
 * Handles WKC accounting itself (Section I Table 5): +1 if any FMMU
 * accepted the read direction, +1 (LWR) or +2 (LRW) if any FMMU accepted
 * the write direction — matching a single ESC's contribution regardless of
 * how many of its own FMMUs the datagram happened to touch.
 * ========================================================================== */
static void fmmu_apply(esc_t *esc, uint32_t log_addr, uint8_t *data,
                        uint16_t dlen, uint8_t cmd, uint16_t *wkc)
{
    int any_read = 0, any_write = 0;
    uint32_t dg_end = log_addr + dlen;

    for (int f = 0; f < REG_FMMU_COUNT; f++) {
        uint8_t *e = esc->regs + REG_FMMU_BASE + f * REG_FMMU_ENTRY_SIZE;
        if (!(e[FMMU_OFF_ACTIVATE] & 0x01)) continue;

        uint32_t log_start  = rd_le32(e + FMMU_OFF_LOG_START);
        uint16_t length     = rd_le16(e + FMMU_OFF_LENGTH);
        uint16_t phys_start = rd_le16(e + FMMU_OFF_PHYS_START);
        uint8_t  type_op    = e[FMMU_OFF_TYPE];
        uint32_t fmmu_end   = log_start + length;

        uint32_t ov_start = (log_addr > log_start) ? log_addr : log_start;
        uint32_t ov_end   = (dg_end < fmmu_end) ? dg_end : fmmu_end;
        if (ov_start >= ov_end) continue; /* no overlap with this FMMU */

        uint16_t ov_len      = (uint16_t)(ov_end - ov_start);
        uint16_t data_offset = (uint16_t)(ov_start - log_addr);
        uint16_t phys_offset = (uint16_t)(phys_start + (ov_start - log_start));

        int seg_read  = (type_op & 0x01) && (cmd == CMD_LRD || cmd == CMD_LRW);
        int seg_write = (type_op & 0x02) && (cmd == CMD_LWR || cmd == CMD_LRW);

        if (seg_read && seg_write) {
            /* Same shared-buffer subtlety as Table 7: save the write value
             * BEFORE the read overwrites it. */
            uint8_t *write_src = malloc(ov_len);
            if (write_src) {
                memcpy(write_src, data + data_offset, ov_len);
                esc_phys_read(esc, phys_offset, data + data_offset, ov_len);
                esc_phys_write(esc, phys_offset, write_src, ov_len);
                free(write_src);
                any_read = 1; any_write = 1;
            }
        } else if (seg_read) {
            esc_phys_read(esc, phys_offset, data + data_offset, ov_len);
            any_read = 1;
        } else if (seg_write) {
            esc_phys_write(esc, phys_offset, data + data_offset, ov_len);
            any_write = 1;
            esc->got_valid_outputs = 1; /* this segment is a write-enabled FMMU — an output */
        }
    }

    if (cmd == CMD_LRD) {
        if (any_read) *wkc += 1;
    } else if (cmd == CMD_LWR) {
        if (any_write) *wkc += 1;
    } else { /* CMD_LRW */
        if (any_read)  *wkc += 1;
        if (any_write) *wkc += 2;
    }

    //fprintf(stderr, "  fmmu_apply result: any_read=%d any_write=%d final_wkc=%u\n",any_read, any_write, *wkc);
}

/* ==========================================================================
 * esc_al_control_write — ESM (Phase 3). Called after any physical write
 * that lands on AL Control (0x0120). Reads back the raw value just written,
 * applies the state-transition graph (Section I Figure 40), updates AL
 * Status (0x0130) / AL Status Code (0x0134) in regs[] directly.
 * ========================================================================== */
void esc_al_control_write(esc_t *esc)
{
    uint16_t al_control = rd_le16(esc->regs + REG_AL_CONTROL);
    uint8_t  requested   = al_control & 0x0F;      /* bits 3:0 */
    uint8_t  error_ack   = (al_control >> 4) & 0x1; /* bit 4 */

    uint16_t al_status  = rd_le16(esc->regs + REG_AL_STATUS);
    uint8_t  current    = al_status & 0x0F;
    uint8_t  had_error  = (al_status >> 4) & 0x1;

    /* Error ack: master acknowledges — clear error flag, state unchanged.
     * (No new state requested this write — SOEM sends ack alone.) */
    if (error_ack && had_error && requested == current) {
        wr_le16(esc->regs + REG_AL_STATUS, current); /* bit4 cleared implicitly */
        wr_le16(esc->regs + REG_AL_STATUS_CODE, ALSTATUSCODE_NOERROR);
        return;
    }

    /* [Phase 7] Requesting the state the slave is already in is not a
     * transition: nothing changes, and an existing error indication stays
     * until it is acknowledged. SOEM's ecx_recover_slave() writes INIT to a
     * slave that has just powered up in INIT; the old code answered that
     * with INIT+ERR 0x0011. */
    if (requested == current && !error_ack) {
        return;
    }

    if (esc->force_reject_al && requested != current) {
        esc->force_reject_al = 0; /* one-shot */
        wr_le16(esc->regs + REG_AL_STATUS, (uint16_t)(current | 0x10));
        wr_le16(esc->regs + REG_AL_STATUS_CODE, ALSTATUSCODE_UNKNOWNALCONTROL);
        return;
    }

    int valid_transition = 0;
    switch (current) {
        case ESM_INIT:   valid_transition = (requested == ESM_PREOP); break;
        case ESM_PREOP:  valid_transition = (requested == ESM_INIT || requested == ESM_SAFEOP); break;
        case ESM_SAFEOP: valid_transition = (requested == ESM_PREOP || requested == ESM_INIT
                                             || requested == ESM_OP); break;
        case ESM_OP:     valid_transition = (requested == ESM_SAFEOP || requested == ESM_INIT); break;
        default: break;
    }

    if (!valid_transition) {
        wr_le16(esc->regs + REG_AL_STATUS, (uint16_t)(current | 0x10)); /* retain the old state, set the error bit */
        wr_le16(esc->regs + REG_AL_STATUS_CODE, ALSTATUSCODE_INVALIDALCONTROL); /* 0x0011 — L2-06 */
        return;
    }

    /* Specifically for SAFEOP to OP: valid outputs must have been received since entering SAFEOP. */
    if (current == ESM_SAFEOP && requested == ESM_OP && !esc->got_valid_outputs) {
        wr_le16(esc->regs + REG_AL_STATUS, (uint16_t)(current | 0x10));
        wr_le16(esc->regs + REG_AL_STATUS_CODE, ALSTATUSCODE_NOVALIDOUTPUTS); /* 0x0019 — L2-04 */
        return;
    }

    /* Hợp lệ — chuyển state, xoá cờ lỗi. */
    wr_le16(esc->regs + REG_AL_STATUS, requested);
    wr_le16(esc->regs + REG_AL_STATUS_CODE, ALSTATUSCODE_NOERROR);

    if (requested == ESM_SAFEOP) {
        esc->got_valid_outputs = 0; /* Every time re-enter SAFEOP, must accept the new output */
    }
}

/* ==========================================================================
 * process_datagram — processes ONE datagram through the ENTIRE chain
 * esc[0..n-1], in physical order (processing-on-the-fly).
 * ========================================================================== */
void process_datagram(esc_t *chain, int n, uint8_t cmd,
                      uint16_t adp, uint16_t ado,
                      uint8_t *data, uint16_t dlen, uint16_t *wkc)
{
    uint16_t local_adp = adp; /* increments as it passes each node — AP* only */
    uint32_t log_addr = (uint32_t)adp | ((uint32_t)ado << 16); /* logical addr convention */

    for (int i = 0; i < n; i++) {
        esc_t *esc = &chain[i];
        int matched = 0, do_read = 0, do_write = 0;
        uint16_t phys_offset = ado; /* default for AP, FP, B commands — direct register offset */

        switch (cmd) {
        case CMD_APRD: matched = (local_adp == 0); do_read = 1; break;
        case CMD_APWR: matched = (local_adp == 0); do_write = 1; break;
        case CMD_APRW: matched = (local_adp == 0); do_read = 1; do_write = 1; break;

        case CMD_FPRD: matched = esc_addr_match(esc, adp); do_read = 1; break;
        case CMD_FPWR: matched = esc_addr_match(esc, adp); do_write = 1; break;
        case CMD_FPRW: matched = esc_addr_match(esc, adp); do_read = 1; do_write = 1; break;

        case CMD_BRD:  matched = 1; do_read = 1; break;
        case CMD_BWR:  matched = 1; do_write = 1; break;
        case CMD_BRW:  matched = 1; do_read = 1; do_write = 1; break;

        /* Read-multiple-write (Section I Table 5): the addressed node READS
         * (+1), every other node WRITES the frame data (+1). Used by SOEM's
         * DC datagram: FRMW 0x0910 on the reference clock. Note that nodes
         * UPSTREAM of the addressed one receive whatever the master put in
         * the data field (SOEM: the previous cycle's DCtime). */
        case CMD_ARMW:
            matched = 1;
            if (local_adp == 0) do_read = 1; else do_write = 1;
            break;
        case CMD_FRMW:
            matched = 1;
            if (esc_addr_match(esc, adp)) do_read = 1; else do_write = 1;
            break;

        case CMD_LRD: case CMD_LWR: case CMD_LRW: {
            if (esc->fault.skip_logical) continue;   /* wkc_short injection */
            //fprintf(stderr, "L*-CMD: cmd=0x%02x log_addr=0x%08x dlen=%u\n", cmd, log_addr, dlen);
            fmmu_apply(esc, log_addr, data, dlen, cmd, wkc);
            continue; /* WKC + all reg access already done inside fmmu_apply */
        }
        default:
            break; /* NOP */
        }

        if (matched) {
            /* Data Out (returned to master) = OLD read value; Data In
             * (written into the ESC) = the master's data — two independent
             * directions sharing the same buffer, so the write source must
             * be saved BEFORE the read overwrites it (Section I Table 7). */
            if (do_read && do_write) {
                uint8_t *write_src = malloc(dlen);
                if (write_src) {
                    memcpy(write_src, data, dlen);

                    if (cmd == CMD_BRW)
                        esc_phys_read_or(esc, phys_offset, data, dlen);
                    else
                        esc_phys_read(esc, phys_offset, data, dlen);
                    *wkc += 1;

                    esc_phys_write(esc, phys_offset, write_src, dlen);
                    *wkc += 2;

                    if (phys_offset == REG_AL_CONTROL) {
                        esc_al_control_write(esc);
                    }

                    free(write_src);
                }
            } else if (do_read) {
                if (cmd == CMD_BRD)
                    esc_phys_read_or(esc, phys_offset, data, dlen);
                else
                    esc_phys_read(esc, phys_offset, data, dlen);
                *wkc += 1;
            } else if (do_write) {
                esc_phys_write(esc, phys_offset, data, dlen);
                *wkc += 1;
                if (phys_offset == REG_AL_CONTROL) {
                    esc_al_control_write(esc);
                }
            }
        }

        /* Auto-increment: INCREMENTS for the next node, ALWAYS, whether or
         * not this node matched (Section I Table 7: "High Addr. Out = Pos.+1"). */
        if (cmd == CMD_APRD || cmd == CMD_APWR || cmd == CMD_APRW || cmd == CMD_ARMW) {
            local_adp = (uint16_t)(local_adp + 1);
        }
    }
}

/* ==========================================================================
 * process_frame — handles one full Ethernet frame (may contain several
 * chained datagrams). Mutates buf in place.
 * ========================================================================== */
void process_frame(esc_t *chain, int n, uint8_t *buf, size_t frame_len)
{
    if (frame_len < ETH_HDR_LEN + EC_HDR_LEN) return;

    uint16_t ethertype = ntohs(*(uint16_t *)(buf + 12));
    if (ethertype != ETH_P_ECAT) return;

    uint16_t ec_hdr = rd_le16(buf + ETH_HDR_LEN);
    uint16_t ec_len = ec_hdr & 0x07FF;

    size_t offset = ETH_HDR_LEN + EC_HDR_LEN;
    size_t end = offset + ec_len;
    if (end > frame_len) end = frame_len; /* safety if the frame got truncated */

    while (offset + DG_HDR_LEN + DG_WKC_LEN <= end) {
        uint8_t *dg = buf + offset;
        uint8_t  cmd = dg[0];
        uint16_t adp = rd_le16(dg + 2);
        uint16_t ado = rd_le16(dg + 4);
        uint16_t len_flags = rd_le16(dg + 6);
        uint16_t dlen = len_flags & 0x07FF;
        uint8_t  more = (len_flags & 0x8000) ? 1 : 0; (void)more;

        if (offset + DG_HDR_LEN + dlen + DG_WKC_LEN > end) break;

        uint8_t *data = dg + DG_HDR_LEN;
        uint8_t *wkc_bytes = data + dlen;
        uint16_t wkc = rd_le16(wkc_bytes);

        process_datagram(chain, n, cmd, adp, ado, data, dlen, &wkc);

        wr_le16(wkc_bytes, wkc);

        offset += DG_HDR_LEN + dlen + DG_WKC_LEN;
    }
}