/* ==========================================================================
 * esc_coe.c — see esc_coe.h for scope and design rationale. Byte layouts
 * below (mailbox header, CoE header, SDO command specifiers) are taken
 * directly from:
 *   - SOEM include/soem/ec_type.h: ECT_MBXT_* (mailbox type nibble),
 *     ECT_COES_* (CoE service, CANopen field bits 12-15),
 *     ECT_SDO_* (SDO command specifiers).
 *   - SOEM include/soem/ec_main.h: ec_mbxheadert (6 byte: length, address,
 *     priority, mbxtype).
 *   - SOEM src/ec_coe.c ecx_SDOread()/ecx_SDOwrite(): exact bit tests the
 *     master applies to a response, walked by hand against this file
 *     during development (see project chat log for the specific
 *     back-and-forth) rather than assumed from the CANopen spec alone.
 * ========================================================================== */

#include <string.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_coe.h"

/* ---- Little-endian helpers -- local copies, matching esc_core.c's own
 * house style of not sharing these across translation units. ---- */
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
static inline void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ---- Protocol constants, confirmed against SOEM include/soem/ec_type.h
 * (grep output, 22/9) -- local copies, soft_bus deliberately never
 * includes any SOEM header. ---- */
#define MBXTYPE_COE       0x03u  /* ECT_MBXT_COE (enum: ERR=0,AOE,EOE,COE=3,FOE,SOE,VOE=0x0f) */
#define COES_SDOREQ       0x02u  /* ECT_COES_SDOREQ */
#define COES_SDORES       0x03u  /* ECT_COES_SDORES */

#define SDO_DOWN_INIT     0x21u
#define SDO_DOWN_EXP      0x23u
#define SDO_DOWN_INIT_CA  0x31u
#define SDO_UP_REQ        0x40u
#define SDO_UP_REQ_CA     0x50u
#define SDO_SEG_UP_REQ    0x60u
#define SDO_ABORT         0x80u

/* Frame byte offsets, relative to the mailbox buffer's own base
 * (SII_SM0_OFFSET for requests, SII_SM1_OFFSET for responses) -- see
 * ec_mbxheadert + ec_SDOt in SOEM (ec_main.h / ec_coe.c). Init frames
 * (fresh upload/download request) carry Index/SubIndex; continuation
 * frames (segments) reuse that same space for raw data instead (exact
 * byte-for-byte behavior read out of ecx_SDOread/ecx_SDOwrite). */
#define OFF_MBX_LENGTH    0
#define OFF_MBX_TYPE      5
#define OFF_CANOPEN       6
#define OFF_COMMAND       8
#define OFF_INDEX         9
#define OFF_SUBINDEX      11
#define OFF_DATA_INIT     12   /* ldata[0] onward, for init frames */
#define OFF_DATA_SEG       9   /* continuation frames: data replaces Index/SubIndex */

/* Max data bytes carried in one init-frame response / one continuation
 * response, chosen so the frame's total footprint never exceeds one
 * mailbox buffer (SII_SM1_SIZE = 128 byte). Mirrors the exact same
 * "-0x10" convention ecx_SDOwrite itself uses for its own chunking
 * (maxdata = mbx_l - 0x10), read directly from ec_coe.c. */
#define COE_MAX_INIT_CHUNK  ((uint32_t)(SII_SM1_SIZE - 0x10))  /* 112 */
#define COE_MAX_SEG_CHUNK   ((uint32_t)(SII_SM1_SIZE - 9))     /* 119 */

/* CANopen SDO Abort codes used below (CiA301 / ETG.1000-6, standard
 * values, not project-specific). */
#define ABORT_UNSUPPORTED_ACCESS     0x06010000u
#define ABORT_OBJECT_DOES_NOT_EXIST  0x06020000u
#define ABORT_LENGTH_TOO_HIGH        0x06070012u
#define ABORT_CMD_SPECIFIER_INVALID  0x05040001u

/* ==========================================================================
 * Object dictionary — see esc_coe.h / esc_types.h's coe_od_t for scope.
 * ========================================================================== */
void coe_od_init(coe_od_t *od)
{
    od->kp = 0;
    od->ki = 0;
    od->kd = 0;
    for (size_t i = 0; i < COE_SEGTEST_BLOB_SIZE; i++) {
        od->segtest_blob[i] = (uint8_t)(i & 0xFFu); /* fixed, checkable pattern */
    }
}

/* Fills `out` (caller-supplied buffer, at least COE_SEGTEST_BLOB_SIZE
 * bytes) with the object's raw value and reports its true byte length.
 * Returns 0 if index/subindex is not a known readable object. */
static int coe_lookup_readable(const esc_t *esc, uint16_t index, uint8_t subindex,
                                uint8_t *out, uint32_t *out_len)
{
    uint32_t v;
    switch (index) {
    case 0x1018: /* Identity */
        if (subindex == 0) { out[0] = 4; *out_len = 1; return 1; } /* number of subindexes */
        switch (subindex) {
        case 1: v = SII_VENDOR_ID;       break;
        case 2: v = SII_PRODUCT_CODE;    break;
        case 3: v = SII_REVISION_NUMBER; break;
        case 4: v = SII_SERIAL_NUMBER;   break;
        default: return 0;
        }
        wr_le32(out, v); *out_len = 4;
        return 1;

    case 0x8000: /* test PID params -- read/write, see coe_write_object() */
        if (subindex == 0) { out[0] = 3; *out_len = 1; return 1; }
        switch (subindex) {
        case 1: v = esc->coe_od.kp; break;
        case 2: v = esc->coe_od.ki; break;
        case 3: v = esc->coe_od.kd; break;
        default: return 0;
        }
        wr_le32(out, v); *out_len = 4;
        return 1;

    case 0x8001: /* fixed >128 byte blob -- forces genuine segmentation */
        if (subindex != 0) return 0;
        memcpy(out, esc->coe_od.segtest_blob, COE_SEGTEST_BLOB_SIZE);
        *out_len = COE_SEGTEST_BLOB_SIZE;
        return 1;

    default:
        return 0;
    }
}

/* Returns 0 if index/subindex is not a known WRITABLE object (caller
 * sends Abort either way -- doesn't distinguish "read-only" from
 * "does not exist" today, both are equally "object does not exist to
 * a download" from the master's point of view for this test OD). */
static int coe_write_object(esc_t *esc, uint16_t index, uint8_t subindex, uint32_t value)
{
    switch (index) {
    case 0x8000:
        switch (subindex) {
        case 1: esc->coe_od.kp = value; return 1;
        case 2: esc->coe_od.ki = value; return 1;
        case 3: esc->coe_od.kd = value; return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* ==========================================================================
 * Response builders. Every one sets the full 6-byte mailbox header +
 * CoE header itself -- no shared helper, so each is readable on its own
 * next to the exact SOEM parsing logic it was written against.
 * ========================================================================== */

static void coe_send_abort(uint8_t *resp, uint32_t abort_code)
{
    /* Index/SubIndex deliberately left at 0, NOT echoing the request.
     * Confirmed by reading ecx_SDOread()/ecx_SDOwrite() directly: both
     * only reach their "if (Command == ECT_SDO_ABORT) ecx_SDOerror(...)"
     * path from the ELSE branch of a check that requires Index (and,
     * for SDOwrite, SubIndex) to NOT match the request -- if we echoed
     * them back (as ETG.1000 literally specifies), this exact SOEM
     * version would instead misparse our abort as a malformed "normal"
     * response and report a different, less specific error. Matching
     * what this master actually does, not what the spec says it should
     * do -- see project chat log for the full derivation. */
    wr_le16(resp + OFF_MBX_LENGTH, 0x000a);
    wr_le16(resp + 2 /* address */, 0x0000);
    resp[4] = 0x00; /* priority */
    resp[OFF_MBX_TYPE] = MBXTYPE_COE; /* counter left 0 -- unchecked by SOEM on this path */
    wr_le16(resp + OFF_CANOPEN, (uint16_t)(COES_SDORES << 12));
    resp[OFF_COMMAND] = SDO_ABORT;
    wr_le16(resp + OFF_INDEX, 0x0000);
    resp[OFF_SUBINDEX] = 0x00;
    wr_le32(resp + OFF_DATA_INIT, abort_code);
}

static void coe_send_upload_expedited(uint8_t *resp, uint16_t index, uint8_t subindex,
                                       const uint8_t *data, uint8_t len)
{
    uint8_t n = (uint8_t)(4 - len); /* unused byte count, bits2-3 of Command */

    wr_le16(resp + OFF_MBX_LENGTH, 0x000a);
    wr_le16(resp + 2, 0x0000);
    resp[4] = 0x00;
    resp[OFF_MBX_TYPE] = MBXTYPE_COE;
    wr_le16(resp + OFF_CANOPEN, (uint16_t)(COES_SDORES << 12));
    resp[OFF_COMMAND] = (uint8_t)(0x23u | (uint8_t)(n << 2)); /* scs=1,e=1,s=1 + n */
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    memset(resp + OFF_DATA_INIT, 0, 4);
    memcpy(resp + OFF_DATA_INIT, data, len);
}

/* Starts (and, if it fits in one frame, finishes) a "normal" upload.
 * `data`/`total_len` must remain valid only for this call -- whatever
 * doesn't fit is re-fetched from the object dictionary by index on each
 * later continuation, not buffered here. */
static void coe_start_upload_normal(esc_t *esc, uint8_t *resp, uint16_t index, uint8_t subindex,
                                     const uint8_t *data, uint32_t total_len)
{
    uint32_t first_chunk = total_len;
    uint8_t  more = 0;
    if (first_chunk > COE_MAX_INIT_CHUNK) {
        first_chunk = COE_MAX_INIT_CHUNK;
        more = 1;
    }

    wr_le16(resp + OFF_MBX_LENGTH, (uint16_t)(0x000a + first_chunk));
    wr_le16(resp + 2, 0x0000);
    resp[4] = 0x00;
    resp[OFF_MBX_TYPE] = MBXTYPE_COE;
    wr_le16(resp + OFF_CANOPEN, (uint16_t)(COES_SDORES << 12));
    resp[OFF_COMMAND] = 0x21; /* scs=1 (0x20), e=0, s=1 (size indicated) */
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    wr_le32(resp + OFF_DATA_INIT, total_len);            /* ldata[0] = total size */
    memcpy(resp + OFF_DATA_INIT + 4, data, first_chunk);  /* ldata[1..] */

    if (more) {
        esc->coe_session.active          = 1;
        esc->coe_session.is_upload       = 1;
        esc->coe_session.index           = index;
        esc->coe_session.subindex        = subindex;
        esc->coe_session.total_size      = total_len;
        esc->coe_session.done            = first_chunk;
        esc->coe_session.expected_toggle = 0x00; /* first continuation must carry toggle=0 */
    } else {
        esc->coe_session.active = 0;
    }
}

static void coe_handle_upload_segment(esc_t *esc, uint8_t *resp, uint8_t command)
{
    coe_session_t *s = &esc->coe_session;
    uint8_t toggle = (uint8_t)(command & 0x10u);

    if (!s->active || !s->is_upload || toggle != s->expected_toggle) {
        coe_send_abort(resp, ABORT_CMD_SPECIFIER_INVALID);
        s->active = 0;
        return;
    }

    uint8_t  scratch[COE_SEGTEST_BLOB_SIZE];
    uint32_t full_len;
    if (!coe_lookup_readable(esc, s->index, s->subindex, scratch, &full_len) ||
        full_len != s->total_size || s->done > full_len) {
        /* object dictionary changed out from under an in-flight
         * transfer -- shouldn't happen (nothing else in soft_bus
         * mutates the OD asynchronously), but fail closed rather than
         * read out of bounds if it ever does. */
        coe_send_abort(resp, ABORT_OBJECT_DOES_NOT_EXIST);
        s->active = 0;
        return;
    }

    uint32_t remaining = full_len - s->done;
    uint32_t chunk = remaining;
    uint8_t  is_last = 1;
    if (chunk > COE_MAX_SEG_CHUNK) {
        chunk = COE_MAX_SEG_CHUNK;
        is_last = 0;
    }

    uint8_t  cmd_byte;
    uint16_t frame_len;
    uint32_t declared_len = chunk; /* how many bytes of the 7-min area are "real" */
    if (is_last) {
        if (chunk < 7) {
            /* Same quirk ecx_SDOwrite's own segment builder uses: pad the
             * frame to the 7-byte minimum, encode how many of those 7
             * bytes are real in Command bits1-3. */
            frame_len = 0x000a;
            cmd_byte  = (uint8_t)(0x01u + (uint8_t)((7 - chunk) << 1));
            declared_len = 7;
        } else {
            frame_len = (uint16_t)(chunk + 3);
            cmd_byte  = 0x01;
        }
    } else {
        frame_len = (uint16_t)(chunk + 3);
        cmd_byte  = 0x00;
    }
    cmd_byte = (uint8_t)(cmd_byte + toggle);

    wr_le16(resp + OFF_MBX_LENGTH, frame_len);
    wr_le16(resp + 2, 0x0000);
    resp[4] = 0x00;
    resp[OFF_MBX_TYPE] = MBXTYPE_COE;
    wr_le16(resp + OFF_CANOPEN, (uint16_t)(COES_SDORES << 12));
    resp[OFF_COMMAND] = cmd_byte;
    memset(resp + OFF_DATA_SEG, 0, declared_len);
    memcpy(resp + OFF_DATA_SEG, scratch + s->done, chunk);

    s->done += chunk;
    if (is_last) {
        s->active = 0;
    } else {
        s->expected_toggle ^= 0x10u;
    }
}

static void coe_handle_download_expedited(esc_t *esc, uint8_t *resp, uint16_t index,
                                           uint8_t subindex, uint8_t command, const uint8_t *req)
{
    uint8_t  n   = (uint8_t)((command >> 2) & 0x03u);
    uint8_t  len = (uint8_t)(4 - n);
    uint32_t value = rd_le32(req + OFF_DATA_INIT);
    if (len < 4) {
        value &= (uint32_t)((1u << (8u * len)) - 1u); /* ignore padding beyond the real bytes */
    }

    if (!coe_write_object(esc, index, subindex, value)) {
        coe_send_abort(resp, ABORT_OBJECT_DOES_NOT_EXIST);
        return;
    }

    wr_le16(resp + OFF_MBX_LENGTH, 0x000a);
    wr_le16(resp + 2, 0x0000);
    resp[4] = 0x00;
    resp[OFF_MBX_TYPE] = MBXTYPE_COE;
    wr_le16(resp + OFF_CANOPEN, (uint16_t)(COES_SDORES << 12));
    resp[OFF_COMMAND] = 0x60; /* scs=3 (0x60): initiate download response */
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    memset(resp + OFF_DATA_INIT, 0, 4);
}

/* ==========================================================================
 * Entry point — see esc_coe.h.
 * ========================================================================== */
void coe_on_mailbox_out_write(esc_t *esc)
{
    uint8_t *req  = esc->regs + SII_SM0_OFFSET;
    uint8_t *resp = esc->regs + SII_SM1_OFFSET;

    uint8_t mbxtype = (uint8_t)(req[OFF_MBX_TYPE] & 0x0Fu);
    if (mbxtype != MBXTYPE_COE) {
        return; /* EoE/FoE/SoE/VoE -- not implemented, no reply placed */
    }

    uint16_t canopen = rd_le16(req + OFF_CANOPEN);
    uint8_t  service = (uint8_t)(canopen >> 12);
    if (service != COES_SDOREQ) {
        return; /* SDO Info / PDO remote request -- not implemented */
    }

    uint8_t command = req[OFF_COMMAND];

    if (command == SDO_UP_REQ_CA || command == SDO_DOWN_INIT_CA) {
        /* Complete Access not implemented -- see esc_coe.h scope note. */
        coe_send_abort(resp, ABORT_UNSUPPORTED_ACCESS);
    } else if (command == SDO_UP_REQ) {
        uint16_t index    = rd_le16(req + OFF_INDEX);
        uint8_t  subindex = req[OFF_SUBINDEX];
        uint8_t  scratch[COE_SEGTEST_BLOB_SIZE];
        uint32_t len;
        if (!coe_lookup_readable(esc, index, subindex, scratch, &len)) {
            coe_send_abort(resp, ABORT_OBJECT_DOES_NOT_EXIST);
        } else if (len <= 4) {
            coe_send_upload_expedited(resp, index, subindex, scratch, (uint8_t)len);
        } else {
            coe_start_upload_normal(esc, resp, index, subindex, scratch, len);
        }
    } else if ((uint8_t)(command & 0xEFu) == SDO_SEG_UP_REQ) {
        coe_handle_upload_segment(esc, resp, command);
    } else if ((uint8_t)(command & 0xF3u) == SDO_DOWN_EXP) {
        uint16_t index    = rd_le16(req + OFF_INDEX);
        uint8_t  subindex = req[OFF_SUBINDEX];
        coe_handle_download_expedited(esc, resp, index, subindex, command, req);
    } else if (command == SDO_DOWN_INIT) {
        /* No writable object in this OD exceeds 4 bytes (see
         * coe_write_object()), so a normal/segmented download always
         * means "wrong length for this object" here. */
        coe_send_abort(resp, ABORT_LENGTH_TOO_HIGH);
    } else {
        /* Covers, among others, a stray download-segment-continuation
         * command ((command & 0xE0) == 0) arriving with no matching
         * session -- which never legitimately happens given the note
         * above -- as well as anything else unrecognized. */
        coe_send_abort(resp, ABORT_CMD_SPECIFIER_INVALID);
    }

    esc->regs[REG_SM1_STATUS] |= SM_STATUS_MAILBOX_FULL;
}