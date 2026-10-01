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
 *   - GD9.3: SOES soes/esc_coe.c (commit 6ef7b94) for the slave side of
 *     Complete Access (SI0 padded to 16 bit, CA only from subindex 0/1,
 *     CA bit echoed in the response) and for the abort codes; SOEM
 *     ecx_readPDOmap()/ecx_readPDOmapCA() for the PDO objects' layout.
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

/* SDO command byte (CiA 301 / ETG.1000.6). Initiate download request:
 * 001 CA(4) n(3:2) e(1) s(0); upload request: 010 CA(4) 0000; segment
 * requests: 000 t(4) n(3:1) c(0) (download), 011 t(4) 0000 (upload). */
#define SDO_UP_REQ        0x40u
#define SDO_UP_REQ_CA     0x50u
#define SDO_SEG_UP_REQ    0x60u
#define SDO_ABORT         0x80u
#define SDO_CA_BIT        0x10u
#define SDO_TOGGLE_BIT    0x10u

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
/* Same limits for what a REQUEST can carry in SM0 (download direction). */
#define COE_MAX_REQ_INIT    ((uint32_t)(SII_SM0_SIZE - 0x10))
#define COE_MAX_REQ_SEG     ((uint32_t)(SII_SM0_SIZE - 9))

/* CANopen SDO Abort codes (CiA 301 Table 22 / ETG.1000.6 Table 41; the
 * same values SOES uses in soes/esc_coe.h). */
#define ABORT_TOGGLE                   0x05030000u  /* toggle bit not alternated          */
#define ABORT_CMD_SPECIFIER_INVALID    0x05040001u  /* client/server command invalid      */
#define ABORT_UNSUPPORTED_ACCESS       0x06010000u  /* unsupported access to an object    */
#define ABORT_READ_ONLY                0x06010002u  /* attempt to write a read-only object */
#define ABORT_CA_UNSUPPORTED           0x06010004u  /* SDO Complete Access not supported  */
#define ABORT_OBJECT_DOES_NOT_EXIST    0x06020000u
#define ABORT_TYPE_MISMATCH            0x06070010u  /* length of service parameter does not match */
#define ABORT_LENGTH_TOO_HIGH          0x06070012u
#define ABORT_LENGTH_TOO_LOW           0x06070013u
#define ABORT_SUBINDEX_DOES_NOT_EXIST  0x06090011u
#define ABORT_VALUE_RANGE              0x06090030u  /* value range of parameter exceeded  */
#define ABORT_WRONG_STATE              0x08000022u  /* not possible in present device state */

/* ==========================================================================
 * Object dictionary model.
 *
 * Objects (index: code, entries):
 *   0x1000 VAR    U32 RO  Device Type = 0
 *   0x1018 RECORD SI0=4, 1..4 U32 RO (Vendor/Product/Revision/Serial)
 *   0x1C00 ARRAY  SI0=4, 1..4 U8 RO = 1,2,3,4          [--coe-pdo-od]
 *   0x1C12 ARRAY  SI0 U8 (0..1), SI1 U16 = 0x1600,      [--coe-pdo-od]
 *                 both writable in PREOP only
 *   0x1C13 ARRAY  same, SI1 = 0x1A00                    [--coe-pdo-od]
 *   0x1600 RECORD SI0=n, 1..n U32 RO 0x7000:i, 8*chunk  [--coe-pdo-od]
 *   0x1A00 RECORD SI0=n, 1..n U32 RO 0x6000:i, 8*chunk  [--coe-pdo-od]
 *   0x8000 RECORD SI0=3, 1..3 U32 RW (test PID params)
 *   0x8001 VAR    OCTET_STRING[200] RO (segmented upload test)
 *   0x8002 VAR    OCTET_STRING[1..400] RW (normal/segmented download test)
 * The PDO mapping objects mirror the SII PDO categories chunk for chunk
 * (esc_core.c append_pdo_category()), so a master that maps from CoE gets
 * exactly the layout it would get from SII.
 * ========================================================================== */
#define OBJ_VAR     0x07u
#define OBJ_ARRAY   0x08u
#define OBJ_RECORD  0x09u

#define DT_U8            0x0005u
#define DT_U16           0x0006u
#define DT_U32           0x0007u
#define DT_OCTET_STRING  0x000Au

#define ACC_RO        0u
#define ACC_RW        1u
#define ACC_RW_PREOP  2u   /* writable in PREOP only (PDO assign, ETG.1020) */

typedef struct {
    uint16_t dtype;
    uint16_t bits;
    uint8_t  access;
} od_entry_t;

void coe_od_init(coe_od_t *od)
{
    od->kp = 0;
    od->ki = 0;
    od->kd = 0;
    for (size_t i = 0; i < COE_SEGTEST_BLOB_SIZE; i++) {
        od->segtest_blob[i] = (uint8_t)(i & 0xFFu); /* fixed, checkable pattern */
    }
    memset(od->octet_rw, 0, sizeof(od->octet_rw));
    od->octet_rw_len = 16;
    od->pdo_assign_n[0]   = 1;
    od->pdo_assign_n[1]   = 1;
    od->pdo_assign_idx[0] = 0x1600;
    od->pdo_assign_idx[1] = 0x1A00;
}

static uint8_t pdo_entry_count(const esc_t *esc)
{
    return (uint8_t)((esc->pdo_size_bytes + SII_PDO_ENTRY_MAX_BYTES - 1) / SII_PDO_ENTRY_MAX_BYTES);
}

static uint32_t pdo_entry_value(const esc_t *esc, uint16_t entry_index, uint8_t sub)
{
    uint32_t before = (uint32_t)(sub - 1) * SII_PDO_ENTRY_MAX_BYTES;
    uint32_t chunk  = esc->pdo_size_bytes - before;
    if (chunk > SII_PDO_ENTRY_MAX_BYTES) chunk = SII_PDO_ENTRY_MAX_BYTES;
    return ((uint32_t)entry_index << 16) | ((uint32_t)sub << 8) | (chunk * 8u);
}

static int is_pdo_od_index(uint16_t index)
{
    return index == 0x1C00 || index == 0x1C12 || index == 0x1C13 ||
           index == 0x1600 || index == 0x1A00;
}

/* Returns 0 if the object does not exist in this node's dictionary. */
static int od_object(const esc_t *esc, uint16_t index, uint8_t *objcode, uint8_t *max_sub)
{
    if (is_pdo_od_index(index) && !esc->coe_pdo_od) return 0;
    switch (index) {
    case 0x1000: *objcode = OBJ_VAR;    *max_sub = 0; return 1;
    case 0x1018: *objcode = OBJ_RECORD; *max_sub = 4; return 1;
    case 0x1C00: *objcode = OBJ_ARRAY;  *max_sub = 4; return 1;
    case 0x1C12:
    case 0x1C13: *objcode = OBJ_ARRAY;  *max_sub = 1; return 1;
    case 0x1600:
    case 0x1A00: *objcode = OBJ_RECORD; *max_sub = pdo_entry_count(esc); return 1;
    case 0x8000: *objcode = OBJ_RECORD; *max_sub = 3; return 1;
    case 0x8001:
    case 0x8002: *objcode = OBJ_VAR;    *max_sub = 0; return 1;
    default:     return 0;
    }
}

/* Returns 0 if the subindex does not exist. Assumes od_object() == 1. */
static int od_entry(const esc_t *esc, uint16_t index, uint8_t sub, od_entry_t *e)
{
    uint8_t objcode, max_sub;
    if (!od_object(esc, index, &objcode, &max_sub) || sub > max_sub) return 0;

    if (objcode != OBJ_VAR && sub == 0) {
        e->dtype = DT_U8; e->bits = 8;
        e->access = (index == 0x1C12 || index == 0x1C13) ? ACC_RW_PREOP : ACC_RO;
        return 1;
    }
    switch (index) {
    case 0x1000:
    case 0x1018:
    case 0x1600:
    case 0x1A00: e->dtype = DT_U32; e->bits = 32; e->access = ACC_RO; return 1;
    case 0x1C00: e->dtype = DT_U8;  e->bits = 8;  e->access = ACC_RO; return 1;
    case 0x1C12:
    case 0x1C13: e->dtype = DT_U16; e->bits = 16; e->access = ACC_RW_PREOP; return 1;
    case 0x8000: e->dtype = DT_U32; e->bits = 32; e->access = ACC_RW; return 1;
    case 0x8001:
        e->dtype = DT_OCTET_STRING; e->bits = COE_SEGTEST_BLOB_SIZE * 8u; e->access = ACC_RO;
        return 1;
    case 0x8002:
        e->dtype = DT_OCTET_STRING; e->bits = (uint16_t)(esc->coe_od.octet_rw_len * 8u);
        e->access = ACC_RW;
        return 1;
    default:
        return 0;
    }
}

/* Current value of one entry, little endian. Returns its byte length.
 * Assumes od_entry() == 1. */
static uint32_t od_read(const esc_t *esc, uint16_t index, uint8_t sub, uint8_t *out)
{
    const coe_od_t *od = &esc->coe_od;
    uint32_t v = 0;

    switch (index) {
    case 0x1000:
        wr_le32(out, 0); /* no standard device profile */
        return 4;
    case 0x1018:
        if (sub == 0) { out[0] = 4; return 1; }
        switch (sub) {
        case 1: v = SII_VENDOR_ID;       break;
        case 2: v = SII_PRODUCT_CODE;    break;
        case 3: v = SII_REVISION_NUMBER; break;
        default: v = SII_SERIAL_NUMBER;  break;
        }
        wr_le32(out, v);
        return 4;
    case 0x1C00:
        out[0] = (uint8_t)(sub == 0 ? 4 : sub); /* SM0..3 = mbx out, mbx in, outputs, inputs */
        return 1;
    case 0x1C12:
    case 0x1C13: {
        int k = (index == 0x1C13);
        if (sub == 0) { out[0] = od->pdo_assign_n[k]; return 1; }
        wr_le16(out, od->pdo_assign_idx[k]);
        return 2;
    }
    case 0x1600:
    case 0x1A00:
        if (sub == 0) { out[0] = pdo_entry_count(esc); return 1; }
        wr_le32(out, pdo_entry_value(esc, index == 0x1A00 ? 0x6000 : 0x7000, sub));
        return 4;
    case 0x8000:
        if (sub == 0) { out[0] = 3; return 1; }
        v = sub == 1 ? od->kp : sub == 2 ? od->ki : od->kd;
        wr_le32(out, v);
        return 4;
    case 0x8001:
        memcpy(out, od->segtest_blob, COE_SEGTEST_BLOB_SIZE);
        return COE_SEGTEST_BLOB_SIZE;
    case 0x8002:
        memcpy(out, od->octet_rw, od->octet_rw_len);
        return od->octet_rw_len;
    default:
        return 0;
    }
}

/* Checks a write of `len` bytes to one entry (existence, access, device
 * state, length, and -- when data != NULL -- the value). 0 = allowed. */
static uint32_t od_check_write(const esc_t *esc, uint16_t index, uint8_t sub,
                               const uint8_t *data, uint32_t len)
{
    uint8_t objcode, max_sub;
    od_entry_t e;

    if (!od_object(esc, index, &objcode, &max_sub)) return ABORT_OBJECT_DOES_NOT_EXIST;
    if (!od_entry(esc, index, sub, &e))            return ABORT_SUBINDEX_DOES_NOT_EXIST;
    if (e.access == ACC_RO)                        return ABORT_READ_ONLY;
    if (e.access == ACC_RW_PREOP &&
        (esc->regs[REG_AL_STATUS] & 0x0F) != ESM_PREOP)
        return ABORT_WRONG_STATE;

    if (e.dtype == DT_OCTET_STRING) {
        if (len == 0)                return ABORT_LENGTH_TOO_LOW;
        if (len > COE_OCTET_RW_MAX)  return ABORT_LENGTH_TOO_HIGH;
    } else if (len != (uint32_t)e.bits / 8u) {
        return ABORT_TYPE_MISMATCH;
    }
    if (!data) return 0;

    if (index == 0x1C12 || index == 0x1C13) {
        if (sub == 0 && data[0] > 1) return ABORT_VALUE_RANGE;
        if (sub == 1 && rd_le16(data) != (index == 0x1C12 ? 0x1600 : 0x1A00))
            return ABORT_VALUE_RANGE; /* only the one fixed PDO exists */
    }
    return 0;
}

/* Stores a write that od_check_write() accepted. */
static void od_commit(esc_t *esc, uint16_t index, uint8_t sub, const uint8_t *data, uint32_t len)
{
    coe_od_t *od = &esc->coe_od;
    switch (index) {
    case 0x1C12:
    case 0x1C13: {
        int k = (index == 0x1C13);
        if (sub == 0) od->pdo_assign_n[k]   = data[0];
        else          od->pdo_assign_idx[k] = rd_le16(data);
        break;
    }
    case 0x8000: {
        uint32_t v = rd_le32(data);
        if (sub == 1) od->kp = v; else if (sub == 2) od->ki = v; else od->kd = v;
        break;
    }
    case 0x8002:
        memcpy(od->octet_rw, data, len);
        od->octet_rw_len = (uint16_t)len;
        break;
    default:
        break;
    }
}

/* ---- Complete Access (ETG.1000.6 5.6.2.x; SOES esc_coe.c) ----
 * The CA image of a RECORD/ARRAY is SI0 as U8 plus one pad byte (16 bit),
 * then entries 1..SI0 packed at their own size; starting at subindex 1
 * leaves SI0 out. Only subindex 0 or 1 may start a CA. A VAR is just its
 * value; a VAR of variable length (OCTET_STRING) cannot be completely
 * accessed (SOES: ABORT_CA_NOT_SUPPORTED). */
static uint32_t od_ca_read(const esc_t *esc, uint16_t index, uint8_t start,
                           uint8_t *out, uint32_t *len)
{
    uint8_t objcode, max_sub;
    od_entry_t e;

    if (!od_object(esc, index, &objcode, &max_sub)) return ABORT_OBJECT_DOES_NOT_EXIST;
    if (start > 1)                                  return ABORT_UNSUPPORTED_ACCESS;
    if (objcode == OBJ_VAR) {
        if (start != 0) return ABORT_SUBINDEX_DOES_NOT_EXIST;
        od_entry(esc, index, 0, &e);
        if (e.dtype == DT_OCTET_STRING) return ABORT_CA_UNSUPPORTED;
        *len = od_read(esc, index, 0, out);
        return 0;
    }

    uint8_t  n;
    uint32_t pos = 0;
    od_read(esc, index, 0, &n);
    if (n > max_sub) n = max_sub;
    if (start == 0) { out[0] = n; out[1] = 0; pos = 2; }
    for (uint8_t s = 1; s <= n; s++) {
        if (pos + 4 > COE_XFER_BUF_MAX) return ABORT_LENGTH_TOO_HIGH;
        pos += od_read(esc, index, s, out + pos);
    }
    *len = pos;
    return 0;
}

/* Checks (commit == 0) or applies (commit == 1) a CA download. Every entry
 * the data covers is checked before anything is stored, so a rejected CA
 * leaves the object untouched. Read-only entries are skipped (SOES does
 * the same); a CA that covers no writable entry at all is refused. The
 * data may end early at an entry boundary (SOES: bytes <= full size). */
static uint32_t od_ca_write(esc_t *esc, uint16_t index, uint8_t start,
                            const uint8_t *data, uint32_t len, int commit)
{
    uint8_t objcode, max_sub;
    od_entry_t e;

    if (!od_object(esc, index, &objcode, &max_sub)) return ABORT_OBJECT_DOES_NOT_EXIST;
    if (start > 1)                                  return ABORT_UNSUPPORTED_ACCESS;
    if (objcode == OBJ_VAR) {
        if (start != 0) return ABORT_SUBINDEX_DOES_NOT_EXIST;
        od_entry(esc, index, 0, &e);
        if (e.dtype == DT_OCTET_STRING) return ABORT_CA_UNSUPPORTED;
        uint32_t a = od_check_write(esc, index, 0, data, len);
        if (a == 0 && commit) od_commit(esc, index, 0, data, len);
        return a;
    }

    uint32_t full = start == 0 ? 2u : 0u;
    for (uint8_t s = 1; s <= max_sub; s++) {
        od_entry(esc, index, s, &e);
        full += e.bits / 8u;
    }
    if (len > full) return ABORT_LENGTH_TOO_HIGH;

    uint32_t pos = 0;
    int writable = 0;
    uint8_t s = start;
    if (start == 0) {
        if (len < 2) return ABORT_TYPE_MISMATCH;
        od_entry(esc, index, 0, &e);
        if (e.access != ACC_RO) {
            uint32_t a = od_check_write(esc, index, 0, data, 1);
            if (a) return a;
            if (commit) od_commit(esc, index, 0, data, 1);
            writable++;
        }
        pos = 2;
        s = 1;
    }
    for (; s <= max_sub && pos < len; s++) {
        od_entry(esc, index, s, &e);
        uint32_t b = e.bits / 8u;
        if (pos + b > len) return ABORT_TYPE_MISMATCH; /* data ends inside an entry */
        if (e.access != ACC_RO) {
            uint32_t a = od_check_write(esc, index, s, data + pos, b);
            if (a) return a;
            if (commit) od_commit(esc, index, s, data + pos, b);
            writable++;
        }
        pos += b;
    }
    return writable ? 0 : ABORT_READ_ONLY;
}

/* One download, CA or not: check everything, then apply. */
static uint32_t od_download(esc_t *esc, uint16_t index, uint8_t sub, int ca,
                            const uint8_t *data, uint32_t len)
{
    if (ca) {
        uint32_t a = od_ca_write(esc, index, sub, data, len, 0);
        if (a == 0) od_ca_write(esc, index, sub, data, len, 1);
        return a;
    }
    uint32_t a = od_check_write(esc, index, sub, data, len);
    if (a == 0) od_commit(esc, index, sub, data, len);
    return a;
}

/* Value (single entry or CA image) an upload transfers. 0 = ok. */
static uint32_t od_upload_value(const esc_t *esc, uint16_t index, uint8_t sub, int ca,
                                uint8_t *out, uint32_t *len)
{
    if (ca) return od_ca_read(esc, index, sub, out, len);

    uint8_t objcode, max_sub;
    od_entry_t e;
    if (!od_object(esc, index, &objcode, &max_sub)) return ABORT_OBJECT_DOES_NOT_EXIST;
    if (!od_entry(esc, index, sub, &e))            return ABORT_SUBINDEX_DOES_NOT_EXIST;
    *len = od_read(esc, index, sub, out);
    return 0;
}

/* ==========================================================================
 * Response builders.
 * ========================================================================== */
static void resp_header(uint8_t *resp, uint16_t length, uint8_t service)
{
    wr_le16(resp + OFF_MBX_LENGTH, length);
    wr_le16(resp + 2 /* address */, 0x0000);
    resp[4] = 0x00; /* priority */
    resp[OFF_MBX_TYPE] = MBXTYPE_COE; /* Cnt set in coe_on_mailbox_out_write() */
    wr_le16(resp + OFF_CANOPEN, (uint16_t)(service << 12));
}

/* Abort SDO Transfer (ETG.1000.6 / CiA 301): CoE service = SDO REQUEST (2),
 * command 0x80, Index/SubIndex ECHO the request, 4-byte abort code.
 *
 * X-01a (GD8): the Phase 5 version sent service = SDO RESPONSE (3) with
 * Index = 0. IgH rejects that ("unknown response"): it only recognises an
 * abort by service == 2. The Index = 0 trick existed only because SOEM's
 * ecx_SDOread() takes the "data" branch when service == SDORES AND Index
 * matches (ec_coe.c:176-178); with the correct service SOEM always reaches
 * its ECT_SDO_ABORT branch, whatever the Index. So the old comment's
 * "SOEM quirk" was really a soft_bus bug masked by a second one. */
static void coe_send_abort(uint8_t *resp, uint16_t index, uint8_t subindex,
                           uint32_t abort_code)
{
    resp_header(resp, 0x000a, COES_SDOREQ);
    resp[OFF_COMMAND] = SDO_ABORT;
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    wr_le32(resp + OFF_DATA_INIT, abort_code);
}

/* scs=2 (0x40), e=1, s=1, n = unused bytes (CiA 301; was 0x23 = download
 * request, X-01a). CA bit echoed like SOES does. */
static void coe_send_upload_expedited(uint8_t *resp, uint16_t index, uint8_t subindex,
                                      int ca, const uint8_t *data, uint8_t len)
{
    uint8_t n = (uint8_t)(4 - len);

    resp_header(resp, 0x000a, COES_SDORES);
    resp[OFF_COMMAND] = (uint8_t)(0x43u | (uint8_t)(n << 2) | (ca ? SDO_CA_BIT : 0));
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    memset(resp + OFF_DATA_INIT, 0, 4);
    memcpy(resp + OFF_DATA_INIT, data, len);
}

/* Starts (and, if it fits in one frame, finishes) a "normal" upload.
 * Whatever doesn't fit is re-fetched from the object dictionary on each
 * later continuation (od_upload_value()), not buffered here. */
static void coe_start_upload_normal(esc_t *esc, uint8_t *resp, uint16_t index, uint8_t subindex,
                                    int ca, const uint8_t *data, uint32_t total_len)
{
    uint32_t first_chunk = total_len;
    uint8_t  more = 0;
    if (first_chunk > COE_MAX_INIT_CHUNK) {
        first_chunk = COE_MAX_INIT_CHUNK;
        more = 1;
    }

    resp_header(resp, (uint16_t)(0x000a + first_chunk), COES_SDORES);
    /* scs=2 (0x40), e=0, s=1 size indicated (CiA 301; was 0x21, X-01a) */
    resp[OFF_COMMAND] = (uint8_t)(0x41u | (ca ? SDO_CA_BIT : 0));
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    wr_le32(resp + OFF_DATA_INIT, total_len);            /* ldata[0] = total size */
    memcpy(resp + OFF_DATA_INIT + 4, data, first_chunk);  /* ldata[1..] */

    memset(&esc->coe_session, 0, sizeof(esc->coe_session));
    if (more) {
        esc->coe_session.active          = 1;
        esc->coe_session.is_upload       = 1;
        esc->coe_session.is_ca           = (uint8_t)(ca ? 1 : 0);
        esc->coe_session.index           = index;
        esc->coe_session.subindex        = subindex;
        esc->coe_session.total_size      = total_len;
        esc->coe_session.done            = first_chunk;
        esc->coe_session.expected_toggle = 0x00; /* first continuation must carry toggle=0 */
    }
}

static void coe_handle_upload_request(esc_t *esc, uint8_t *resp, uint16_t index,
                                      uint8_t subindex, int ca)
{
    uint8_t  scratch[COE_XFER_BUF_MAX];
    uint32_t len = 0;
    uint32_t a = od_upload_value(esc, index, subindex, ca, scratch, &len);

    if (a) {
        /* Echo the REQUEST's index/subindex (was coe_session's, i.e. the
         * last segmented transfer's -- missed by the X-01a fix). */
        coe_send_abort(resp, index, subindex, a);
        esc->coe_session.active = 0;
    } else if (len >= 1 && len <= 4) {
        coe_send_upload_expedited(resp, index, subindex, ca, scratch, (uint8_t)len);
        esc->coe_session.active = 0;
    } else {
        coe_start_upload_normal(esc, resp, index, subindex, ca, scratch, len);
    }
}

static void coe_handle_upload_segment(esc_t *esc, uint8_t *resp, uint8_t command)
{
    coe_session_t *s = &esc->coe_session;
    uint8_t toggle = (uint8_t)(command & SDO_TOGGLE_BIT);

    if (!s->active || !s->is_upload) {
        coe_send_abort(resp, s->index, s->subindex, ABORT_CMD_SPECIFIER_INVALID);
        s->active = 0;
        return;
    }
    if (toggle != s->expected_toggle) {
        coe_send_abort(resp, s->index, s->subindex, ABORT_TOGGLE);
        s->active = 0;
        return;
    }

    uint8_t  scratch[COE_XFER_BUF_MAX];
    uint32_t full_len = 0;
    if (od_upload_value(esc, s->index, s->subindex, s->is_ca, scratch, &full_len) ||
        full_len != s->total_size || s->done > full_len) {
        /* object dictionary changed out from under an in-flight
         * transfer -- shouldn't happen (nothing else in soft_bus
         * mutates the OD asynchronously), but fail closed rather than
         * read out of bounds if it ever does. */
        coe_send_abort(resp, s->index, s->subindex, ABORT_OBJECT_DOES_NOT_EXIST);
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
             * bytes are unused in Command bits1-3. */
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

    resp_header(resp, frame_len, COES_SDORES);
    resp[OFF_COMMAND] = cmd_byte;
    memset(resp + OFF_DATA_SEG, 0, declared_len);
    memcpy(resp + OFF_DATA_SEG, scratch + s->done, chunk);

    s->done += chunk;
    if (is_last) {
        s->active = 0;
    } else {
        s->expected_toggle ^= SDO_TOGGLE_BIT;
    }
}

/* Initiate download response: scs=3 (0x60), CA bit echoed (SOES), 4 zero
 * data bytes. SOEM checks Index and SubIndex, IgH only scs. */
static void coe_send_download_response(uint8_t *resp, uint16_t index, uint8_t subindex, int ca)
{
    resp_header(resp, 0x000a, COES_SDORES);
    resp[OFF_COMMAND] = (uint8_t)(0x60u | (ca ? SDO_CA_BIT : 0));
    wr_le16(resp + OFF_INDEX, index);
    resp[OFF_SUBINDEX] = subindex;
    memset(resp + OFF_DATA_INIT, 0, 4);
}

/* Initiate download request, command 001 CA n n e s:
 *  - e=1 expedited: data in ldata[0], size 4-n when s=1 (4 otherwise).
 *  - e=0 normal: ldata[0] = total size, data follows; whatever does not
 *    fit arrives in download segments (coe_handle_download_segment()).
 * Structural checks (object, subindex, access, state, length) happen here
 * already, so a master is refused before it sends any segment; the value
 * checks and the store happen once the whole data is in. */
static void coe_handle_download_init(esc_t *esc, uint8_t *resp, const uint8_t *req,
                                     uint16_t index, uint8_t subindex, uint8_t command)
{
    int ca = (command & SDO_CA_BIT) != 0;
    coe_session_t *s = &esc->coe_session;
    uint32_t a;

    s->active = 0;
    if (ca && !esc->coe_ca) {
        /* Complete Access off (default) -- same answer as before GD9.3. */
        coe_send_abort(resp, index, subindex, ABORT_UNSUPPORTED_ACCESS);
        return;
    }

    if (command & 0x02u) { /* expedited */
        uint32_t len = (command & 0x01u) ? (uint32_t)(4u - ((command >> 2) & 0x03u)) : 4u;
        a = od_download(esc, index, subindex, ca, req + OFF_DATA_INIT, len);
        if (a) coe_send_abort(resp, index, subindex, a);
        else   coe_send_download_response(resp, index, subindex, ca);
        return;
    }
    if (!(command & 0x01u)) { /* normal transfer without size: not supported */
        coe_send_abort(resp, index, subindex, ABORT_CMD_SPECIFIER_INVALID);
        return;
    }

    uint32_t total = rd_le32(req + OFF_DATA_INIT);
    uint16_t mbx_len = rd_le16(req + OFF_MBX_LENGTH);
    uint32_t in_frame = mbx_len > 10 ? (uint32_t)(mbx_len - 10) : 0;
    if (in_frame > COE_MAX_REQ_INIT) in_frame = COE_MAX_REQ_INIT;
    if (in_frame > total) in_frame = total;

    if (total > COE_XFER_BUF_MAX) {
        coe_send_abort(resp, index, subindex, ABORT_LENGTH_TOO_HIGH);
        return;
    }
    /* Structural pre-check with the declared size, no data yet. */
    a = ca ? 0 : od_check_write(esc, index, subindex, NULL, total);
    if (ca) {
        uint8_t objcode, max_sub;
        if (!od_object(esc, index, &objcode, &max_sub)) a = ABORT_OBJECT_DOES_NOT_EXIST;
        else if (subindex > 1)                          a = ABORT_UNSUPPORTED_ACCESS;
    }
    if (a) {
        coe_send_abort(resp, index, subindex, a);
        return;
    }

    if (in_frame == total) { /* normal, one frame */
        a = od_download(esc, index, subindex, ca, req + OFF_DATA_INIT + 4, total);
        if (a) coe_send_abort(resp, index, subindex, a);
        else   coe_send_download_response(resp, index, subindex, ca);
        return;
    }

    memset(s, 0, sizeof(*s));
    s->active          = 1;
    s->is_upload       = 0;
    s->is_ca           = (uint8_t)ca;
    s->index           = index;
    s->subindex        = subindex;
    s->total_size      = total;
    s->done            = in_frame;
    s->expected_toggle = 0x00;
    memcpy(s->xfer, req + OFF_DATA_INIT + 4, in_frame);
    coe_send_download_response(resp, index, subindex, ca);
}

/* Download segment request, command 000 t n n n c; data from byte 9, frame
 * length = data + 3, except a last segment shorter than 7 byte, which is
 * padded to 7 with n = unused bytes (ecx_SDOwrite). Response 001 t 0000,
 * Index/SubIndex bytes zero (SOEM checks (cmd & 0xE0) == 0x20). */
static void coe_handle_download_segment(esc_t *esc, uint8_t *resp, const uint8_t *req,
                                        uint8_t command)
{
    coe_session_t *s = &esc->coe_session;
    uint8_t toggle = (uint8_t)(command & SDO_TOGGLE_BIT);
    int last = (command & 0x01u) != 0;

    if (!s->active || s->is_upload) {
        coe_send_abort(resp, s->index, s->subindex, ABORT_CMD_SPECIFIER_INVALID);
        s->active = 0;
        return;
    }
    if (toggle != s->expected_toggle) {
        coe_send_abort(resp, s->index, s->subindex, ABORT_TOGGLE);
        s->active = 0;
        return;
    }

    uint16_t mbx_len = rd_le16(req + OFF_MBX_LENGTH);
    uint32_t seg = mbx_len > 3 ? (uint32_t)(mbx_len - 3) : 0;
    if (seg > COE_MAX_REQ_SEG) seg = COE_MAX_REQ_SEG;
    if (last && seg == 7) seg -= (uint32_t)((command >> 1) & 0x07u);

    if (s->done + seg > s->total_size) {
        coe_send_abort(resp, s->index, s->subindex, ABORT_LENGTH_TOO_HIGH);
        s->active = 0;
        return;
    }
    memcpy(s->xfer + s->done, req + OFF_DATA_SEG, seg);
    s->done += seg;

    if (last) {
        s->active = 0;
        uint32_t a = s->done != s->total_size ? ABORT_LENGTH_TOO_LOW
                   : od_download(esc, s->index, s->subindex, s->is_ca, s->xfer, s->total_size);
        if (a) {
            coe_send_abort(resp, s->index, s->subindex, a);
            return;
        }
    } else {
        s->expected_toggle ^= SDO_TOGGLE_BIT;
    }

    resp_header(resp, 0x000a, COES_SDORES);
    resp[OFF_COMMAND] = (uint8_t)(0x20u | toggle);
    memset(resp + OFF_INDEX, 0, 7);
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
    /* Echoed in any abort sent from this dispatcher (CiA 301 / ETG.1000.6). */
    uint16_t req_index    = rd_le16(req + OFF_INDEX);
    uint8_t  req_subindex = req[OFF_SUBINDEX];

    if (command == SDO_ABORT) {
        /* Master aborts the transfer in progress: no response (CiA 301). */
        esc->coe_session.active = 0;
        return;
    } else if (command == SDO_UP_REQ_CA) {
        if (!esc->coe_ca) {
            /* Complete Access off (default) -- see esc_coe.h. */
            coe_send_abort(resp, req_index, req_subindex, ABORT_UNSUPPORTED_ACCESS);
        } else {
            coe_handle_upload_request(esc, resp, req_index, req_subindex, 1);
        }
    } else if (command == SDO_UP_REQ) {
        coe_handle_upload_request(esc, resp, req_index, req_subindex, 0);
    } else if ((uint8_t)(command & 0xEFu) == SDO_SEG_UP_REQ) {
        coe_handle_upload_segment(esc, resp, command);
    } else if ((command & 0xE0u) == 0x20u) {
        coe_handle_download_init(esc, resp, req, req_index, req_subindex, command);
    } else if ((command & 0xE0u) == 0x00u) {
        coe_handle_download_segment(esc, resp, req, command);
    } else {
        coe_send_abort(resp, req_index, req_subindex, ABORT_CMD_SPECIFIER_INVALID);
    }

    /* [Phase 7.4] Cnt (bits 4..6 of the type byte), 1..7 cyclic, one step
     * per NEW response. Repeat requests and the mbx_dup injection re-post
     * the same bytes, so they carry the same Cnt as the original. */
    esc->coe_od.resp_cnt = (uint8_t)(esc->coe_od.resp_cnt % 7u + 1u);
    resp[OFF_MBX_TYPE] = (uint8_t)((resp[OFF_MBX_TYPE] & 0x0Fu) | (esc->coe_od.resp_cnt << 4));

    /* GD9.6 (E-09): a slow slave -- e.g. a drive that stores a parameter
     * before it answers -- posts its download response only after
     * coe_delay_ms. The bytes are in SM1 already; "mailbox full" is set by
     * esc_fault_frame_begin() at the first frame after the release time.
     * Uploads (SOEM's own configuration reads) are never delayed. */
    int is_download = (command & 0xE0u) == 0x20u || (command & 0xE0u) == 0x00u;
    if (esc->fault.coe_delay_ms && is_download) {
        esc->fault.mbx_held = 1;
        esc->fault.mbx_release_ns = esc->wd.now_ns + (uint64_t)esc->fault.coe_delay_ms * 1000000ull;
        return;
    }
    esc->regs[REG_SM1_STATUS] |= SM_STATUS_MAILBOX_FULL;
}

/* ==========================================================================
 * GD9.7: Emergency message (fault injection only).
 * ========================================================================== */
#define COES_EMERGENCY 0x01u   /* ECT_COES_EMERGENCY */

int coe_post_emcy(esc_t *esc, uint16_t code, uint8_t reg, const uint8_t data[5])
{
    if ((esc->regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL) || esc->fault.mbx_held)
        return 0;
    uint8_t *resp = esc->regs + SII_SM1_OFFSET;
    memset(resp, 0, 16);
    resp_header(resp, 0x000a, COES_EMERGENCY);
    wr_le16(resp + 8, code);           /* error code         */
    resp[10] = reg;                    /* error register     */
    memcpy(resp + 11, data, 5);        /* manufacturer data  */
    esc->coe_od.resp_cnt = (uint8_t)(esc->coe_od.resp_cnt % 7u + 1u);
    resp[OFF_MBX_TYPE] = (uint8_t)((resp[OFF_MBX_TYPE] & 0x0Fu) | (esc->coe_od.resp_cnt << 4));
    esc->regs[REG_SM1_STATUS] |= SM_STATUS_MAILBOX_FULL;
    return 1;
}
