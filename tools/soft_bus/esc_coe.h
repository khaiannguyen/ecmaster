#ifndef ESC_COE_H
#define ESC_COE_H

#include <stdint.h>
#include "esc_types.h"

/* ==========================================================================
 * esc_coe.h — minimal CoE/SDO server, one mailbox session per node.
 *
 * Scope (Giai doan 5, "Mailbox SM0/SM1 + CoE" per roadmap_master_ethercat.md;
 * extended in GD9.3):
 *  - SDO Upload (read), expedited, normal and segmented.
 *  - SDO Download (write), expedited, normal and segmented (GD9.3: the
 *    last two used to answer Abort 0x06070012). Every write is checked
 *    completely before anything is stored.
 *  - SDO Complete Access upload/download (GD9.3), only when the node was
 *    set up with esc_set_coe_features(ca=1) (soft_bus --coe-ca); otherwise
 *    a CA request gets Abort 0x06010000 exactly as before.
 *  - SDO Abort with CiA 301 codes (object / subindex missing, read-only,
 *    length, value range, wrong state, toggle, CA unsupported).
 *  - Object dictionary: see the table in esc_coe.c. 0x1000, 0x1018,
 *    0x8000 (PID test params, RW), 0x8001 (200 byte RO blob, segmented
 *    upload), 0x8002 (1..400 byte RW, segmented download); with
 *    --coe-pdo-od (implied by --coe-ca) also 0x1C00, 0x1C12/0x1C13 (PDO
 *    assign, writable in PREOP only) and 0x1600/0x1A00 (PDO mapping,
 *    mirroring the SII PDO categories).
 *
 * Explicitly OUT of scope here:
 *  - SDO Info (ECT_COES_SDOINFO) -- ecx_readODlist/readOE would get no
 *    reply. Not used by ecm_run.c today.
 *  - Emergency messages (ECT_COES_EMERGENCY) -- soft_bus never spontaneously
 *    reports one; nothing generates them yet.
 *  - Changing the process data layout: 0x1C12/0x1C13 only accept the one
 *    fixed PDO (0x1600/0x1A00), mapping objects are read-only.
 *
 * All request/response bytes are read/written directly at
 * esc->regs[SII_SM0_OFFSET]/[SII_SM1_OFFSET] using plain little-endian
 * byte offsets (same house style as esc_core.c's rd_le16/wr_le16) rather
 * than an overlaid struct -- avoids depending on SOEM's own packed
 * struct layout/headers, which this project deliberately keeps out of
 * soft_bus entirely.
 * ========================================================================== */

/* Fills the object dictionary with its power-on defaults (PID params
 * zeroed, segtest blob filled with a fixed, checkable byte pattern).
 * Called once from esc_init(). */
void coe_od_init(coe_od_t *od);

/* Called from esc_phys_write() the instant a write lands anywhere in
 * this node's SM0 (mailbox out) DPRAM range (SII_SM0_OFFSET..+SIZE).
 * Parses whatever CoE/SDO request the master just placed there and,
 * still synchronously within this same call, builds the response into
 * this node's SM1 (mailbox in) DPRAM range and sets SM1's Status byte
 * "mailbox full" bit (REG_SM1_STATUS, SM_STATUS_MAILBOX_FULL) so the
 * master's ecx_mbxinhandler picks it up on its next cyclic poll.
 *
 * Non-CoE mailbox types (EoE/FoE/SoE/VoE) are silently ignored (no
 * response placed, no full-bit set) -- this soft_bus implements CoE
 * only, matching the scope above. */
void coe_on_mailbox_out_write(esc_t *esc);

#endif /* ESC_COE_H */