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
 *  - SM watchdog stores a value but has no behavior yet — Phase 4 scope.
 *  - No CoE SDO mailbox server: the SII mailbox-protocol CoE bit is
 *    deliberately cleared in esc_build_sii() so SOEM skips straight to
 *    reading PDO mapping from SII categories instead of attempting (and
 *    timing out on) a CoE PDO-mapping read.
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

static void esc_build_sii(esc_t *esc, uint16_t pdo_size_bytes)
{
    size_t words = 0;

    for (size_t i = 0; i < SII_IMAGE_DEFAULT_WORDS && i < ESC_SII_IMAGE_MAX_WORDS; i++)
        esc->sii_image_buf[i] = g_sii_image_default[i];
    words = SII_IMAGE_DEFAULT_WORDS;

    /* Clear the CoE bit — see file header comment. */
    esc->sii_image_buf[28] = (uint16_t)(g_sii_image_default[28] & (uint16_t)~SII_MBX_PROTOCOL_COE);

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

    esc_build_sii(esc, pdo_size_bytes);

    /* DL Status is filled in by esc_chain_wire() — depends on position. */
}

/* ==========================================================================
 * esc_chain_wire — link state is a relationship BETWEEN nodes, so it must
 * be set after the whole chain is known. Model: a straight chain, each node
 * uses port 0 (upstream) and port 1 (downstream).
 * ========================================================================== */
void esc_chain_wire(esc_t *chain, int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t dl = DLSTAT_PDI_OPERATIONAL | DLSTAT_PDI_WD_OK;

        dl |= DLSTAT_LINK_PORT0 | DLSTAT_COMM_PORT0; /* port0: always linked */

        if (i < n - 1) {
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

    if (phys_offset < REG_SII_DATA + 4 && phys_offset + len > REG_SII_DATA) {
        esc_sii_refresh_data(esc);
    }
    memcpy(data, esc->regs + phys_offset, len);
}

/* OR-accumulating read — REQUIRED for BRD/BRW per spec: multiple slaves OR
 * their data into the frame as it passes through each of them in turn. */
static void esc_phys_read_or(esc_t *esc, uint16_t phys_offset, uint8_t *data, uint16_t len)
{
    if (phys_offset + (uint32_t)len > ESC_REG_SPACE_SIZE) return;
    if (phys_offset < REG_SII_DATA + 4 && phys_offset + len > REG_SII_DATA) {
        esc_sii_refresh_data(esc);
    }
    for (uint16_t k = 0; k < len; k++) {
        data[k] |= esc->regs[phys_offset + k];
    }
}

static void esc_phys_write(esc_t *esc, uint16_t phys_offset, const uint8_t *data, uint16_t len)
{
    if (phys_offset + (uint32_t)len > ESC_REG_SPACE_SIZE) return;
    memcpy(esc->regs + phys_offset, data, len);

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

        case CMD_LRD: case CMD_LWR: case CMD_LRW: {
            //fprintf(stderr, "L*-CMD: cmd=0x%02x log_addr=0x%08x dlen=%u\n", cmd, log_addr, dlen);
            fmmu_apply(esc, log_addr, data, dlen, cmd, wkc);
            continue; /* WKC + all reg access already done inside fmmu_apply */
        }
        default:
            break; /* NOP or unsupported command (ARMW/FRMW) — ignored on purpose */
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
        if (cmd == CMD_APRD || cmd == CMD_APWR || cmd == CMD_APRW) {
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
