#ifndef ESC_SII_H
#define ESC_SII_H

#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
 * esc_sii.h
 * Anh SII EEPROM — phan CO DINH (Vendor/Product/Mailbox) + hang so category.
 * Don vi: WORD (16-bit).
 *
 * (English translation, per project convention going forward — see note
 *  at end of conversation: all new/updated code is now written in English.)
 * ========================================================================== */

#define SII_VENDOR_ID        0x00000499u  /* internal test placeholder, NOT a real Beckhoff ID */
#define SII_PRODUCT_CODE     0x00000001u
#define SII_REVISION_NUMBER  0x00000001u
#define SII_SERIAL_NUMBER    0x00000000u

#define SII_SM0_OFFSET   0x1000u  /* mailbox out (master->slave) */
#define SII_SM0_SIZE     0x0080u
#define SII_SM1_OFFSET   0x1080u  /* mailbox in (slave->master) */
#define SII_SM1_SIZE     0x0080u
#define SII_MBX_PROTOCOL_COE  (1u << 2)

/* Process data SyncManagers, published in the SII SyncManager category (41).
 * [Phase 7] Before this category existed, SOEM had no physical address for
 * SM2/SM3: ecx_map_sm() then set FMMU PhysStart = SM[x].StartAddr = 0x0000,
 * so every output write landed on ESC registers 0x0000.. (type, revision,
 * build and, for pdo_size >= 18, the station address at 0x0010), and SM2/SM3
 * were never programmed at all. Real ESIs always carry this category. SM3
 * starts right after SM2, 8-byte aligned; both must fit below 0x2000. */
#define SII_SM2_OFFSET   0x1100u  /* outputs (RxPDO, master -> slave) */
#define SII_PD_SM_ALIGN  8u
#define SII_PD_MAX_BYTES ((0x2000u - SII_SM2_OFFSET) / 2u)   /* 1920 */

/* SM control bytes, the usual Beckhoff values (Section II §2.14.3):
 *  SM0 0x26 mailbox, ECAT write, PDI IRQ    SM1 0x22 mailbox, ECAT read, PDI IRQ
 *  SM2 0x64 3-buffer, ECAT write, PDI IRQ, watchdog trigger enable
 *  SM3 0x20 3-buffer, ECAT read, PDI IRQ */
#define SII_SM0_CONTROL  0x26u
#define SII_SM1_CONTROL  0x22u
#define SII_SM2_CONTROL  0x64u
#define SII_SM3_CONTROL  0x20u

#define SII_WORD0_PDI_CONTROL  0x0080u  /* SPI, matches LAN9252 */

/* Category types (ETG.2000 / confirmed against SOEM ec_type.h) */
#define SII_CAT_STRINGS  10
#define SII_CAT_GENERAL  30
#define SII_CAT_FMMU     40
#define SII_CAT_SYNCM    41
#define SII_CAT_TXPDO    50
#define SII_CAT_RXPDO    51
#define SII_CAT_END      0xFFFFu

/* Categories MUST start at word 64 (0x0040) — confirmed against SOEM
 * ec_main.c ecx_siifind(): "a = ECT_SII_START << 1" where ECT_SII_START =
 * 0x0040 (ec_type.h). Words 32-63 are reserved/padding in this minimal image. */
#define SII_CATEGORY_START_WORD  0x0040

/* Max bits per single PDO entry: field is 1 byte (255 max). Use 248 (31 byte)
 * for a safety margin, larger PDOs are split across multiple entries. */
#define SII_PDO_ENTRY_MAX_BYTES  31

static const uint16_t g_sii_image_default[] = {
    /* word 0 */  SII_WORD0_PDI_CONTROL,
    /* word 1 */  0x0000,
    /* word 2 */  0x0000,
    /* word 3 */  0x0000,
    /* word 4 */  0x0000,
    /* word 5 */  0x0000,
    /* word 6 */  0x0000,
    /* word 7 */  0x0000,
    /* word 8-9   Vendor ID (32-bit, low word first) */
    (uint16_t)(SII_VENDOR_ID & 0xFFFF),
    (uint16_t)(SII_VENDOR_ID >> 16),
    /* word 10-11 Product Code */
    (uint16_t)(SII_PRODUCT_CODE & 0xFFFF),
    (uint16_t)(SII_PRODUCT_CODE >> 16),
    /* word 12-13 Revision */
    (uint16_t)(SII_REVISION_NUMBER & 0xFFFF),
    (uint16_t)(SII_REVISION_NUMBER >> 16),
    /* word 14-15 Serial */
    (uint16_t)(SII_SERIAL_NUMBER & 0xFFFF),
    (uint16_t)(SII_SERIAL_NUMBER >> 16),
    /* word 16-23: reserved/bootstrap mailbox */
    0, 0, 0, 0, 0, 0, 0, 0,
    /* word 24-25: SM0 offset/size (standard mailbox out) */
    (uint16_t)SII_SM0_OFFSET, (uint16_t)SII_SM0_SIZE,
    /* word 26-27: SM1 offset/size (standard mailbox in) */
    (uint16_t)SII_SM1_OFFSET, (uint16_t)SII_SM1_SIZE,
    /* word 28: mailbox protocol bitmask — CoE bit is SET. [Giai doan 5:
     * previously cleared here on purpose, back when no CoE SDO server
     * existed; esc_coe.c now implements one on SM0/SM1, so this bit
     * reflects that truthfully.] */
    SII_MBX_PROTOCOL_COE,
    /* word 29-31: reserved */
    0, 0, 0,
};
#define SII_IMAGE_DEFAULT_WORDS \
    (sizeof(g_sii_image_default) / sizeof(g_sii_image_default[0]))

#endif /* ESC_SII_H */