#ifndef ESC_COE_H
#define ESC_COE_H

#include <stdint.h>
#include "esc_types.h"

/* ==========================================================================
 * esc_coe.h — minimal CoE/SDO server, one mailbox session per node.
 *
 * Scope (Giai doan 5, "Mailbox SM0/SM1 + CoE" per roadmap_master_ethercat.md):
 *  - SDO Upload (read), expedited and segmented, single subindex only
 *    (no Complete Access server-side -- ecx_SDOread(CA=true) will get an
 *    Abort, see esc_coe.c's default case).
 *  - SDO Download (write), expedited and segmented, single subindex only.
 *  - SDO Abort for unknown Index/SubIndex.
 *  - Fixed object dictionary (esc_types.h's coe_od_t): 0x1018 Identity
 *    (read-only, mirrors this node's own SII Vendor/Product/Revision/
 *    Serial), 0x8000 PID test params (read/write, expedited-sized),
 *    0x8001 a >128 byte read-only blob (forces real segmentation).
 *
 * Explicitly OUT of scope here (not needed by L4-01..L4-05):
 *  - SDO Info (ECT_COES_SDOINFO) -- ecx_readODlist/readOE would get no
 *    reply. Not used by ecm_run.c today.
 *  - Emergency messages (ECT_COES_EMERGENCY) -- soft_bus never spontaneously
 *    reports one; nothing generates them yet.
 *  - Complete Access (CA) SDO upload/download.
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