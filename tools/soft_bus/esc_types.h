#ifndef ESC_TYPES_H
#define ESC_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "esc_dc_state.h"

/* ==========================================================================
 * esc_types.h  — register map + esc_t struct, checked against the real
 * Beckhoff datasheets (Section I v2.5, Section II v3.3).
 *
 * Design: the register map is a RAW BYTE ARRAY, offsets identical to the
 * datasheet. The datasheet confirms little-endian ("LSB at the low address"),
 * matching rd_le16/wr_le16 in esc_core.c.
 * ========================================================================== */

/* Register area 0x0000-0x0FFF (4KB) + DPRAM from 0x1000 (LAN9252: 4KB) */
#define ESC_REG_SPACE_SIZE   0x2000

/* Max words for the per-node generated SII image (prefix + PDO categories +
 * End marker). 256 words leaves generous headroom beyond current test sizes
 * (largest tested: --pdo-size 64 needs about 65 words). */
#define ESC_SII_IMAGE_MAX_WORDS  256

/* Size of the fixed CoE test blob (object 0x8001:00) used to force
 * genuine multi-frame SDO segmentation -- see coe_od_t below. Must
 * exceed one mailbox buffer's usable payload (SII_SM0_SIZE/SM1_SIZE =
 * 128 byte, ~112 usable after the 16-byte SDO header). */
#define COE_SEGTEST_BLOB_SIZE  200

/* ---- ESC information (Section II §2.1) ---- */
#define REG_TYPE              0x0000  /* 1 byte */
#define REG_REVISION          0x0001  /* 1 byte */
#define REG_BUILD             0x0002  /* 2 byte */
#define REG_FMMU_SUPPORTED    0x0004  /* =3 for LAN9252 */
#define REG_SM_SUPPORTED      0x0005  /* =4 for LAN9252 */
#define REG_RAM_SIZE          0x0006  /* KB, =4 */
#define REG_PORT_DESCRIPTOR   0x0007  /* §2.1.7 — NOT 0x0008 (that is "ESC features supported") */
#define REG_ESC_FEATURES      0x0008  /* 2 byte, §2.1.8 */

#define ESC_TYPE_CUSTOM_IPCORE     0x04   /* §2.1.1 "Customer FPGA IP core" */
#define ESC_PORTDESC_2ETH          0x0F   /* port0=11b, port1=11b, port2/3=00b */
#define ESC_FEATURE_FMMU_BYTEWISE  0x0001 /* §2.1.8 bit0=1: byte-oriented FMMU */

/* ---- Station address (§2.2) ---- */
#define REG_STATION_ADDR      0x0010
#define REG_STATION_ALIAS     0x0012

/* ---- Data link layer (§2.4) ---- */
#define REG_DL_CONTROL        0x0100  /* 4 byte; bit24 = alias enable */
#define REG_DL_STATUS         0x0110  /* 2 byte */

/* DL Status bits (§2.4.3) — used by esc_chain_wire() */
#define DLSTAT_PDI_OPERATIONAL  (1u << 0)
#define DLSTAT_PDI_WD_OK        (1u << 1)
#define DLSTAT_LINK_PORT0       (1u << 4)
#define DLSTAT_LINK_PORT1       (1u << 5)
#define DLSTAT_LINK_PORT2       (1u << 6)
#define DLSTAT_LINK_PORT3       (1u << 7)
#define DLSTAT_LOOP_PORT0       (1u << 8)   /* 1 = closed */
#define DLSTAT_COMM_PORT0       (1u << 9)
#define DLSTAT_LOOP_PORT1       (1u << 10)
#define DLSTAT_COMM_PORT1       (1u << 11)
#define DLSTAT_LOOP_PORT2       (1u << 12)
#define DLSTAT_COMM_PORT2       (1u << 13)
#define DLSTAT_LOOP_PORT3       (1u << 14)
#define DLSTAT_COMM_PORT3       (1u << 15)

/* DL Control bit24 = alias enable. 0x0100 is a DWORD -> bit24 is in byte
 * 0x0103, bit0. */
#define REG_DL_CONTROL_ALIAS_BYTE  0x0103
#define DLCTRL_ALIAS_ENABLE_BIT    (1u << 0)

/* ---- Application layer (§2.5) ---- */
#define REG_AL_CONTROL        0x0120  /* power-on reset value = 1 (INIT) */
#define REG_AL_STATUS         0x0130  /* power-on reset value = 1 (INIT) */
#define REG_AL_STATUS_CODE    0x0134

/* ---- Error counters (§2.9) ---- */
#define REG_ERR_COUNTERS_BASE 0x0300
#define REG_ERR_COUNTERS_END  0x0317

/* ---- Watchdog (§2.10) ---- */
#define REG_WD_DIVIDER        0x0400
#define REG_WD_TIME_PDI0      0x0410
#define REG_WD_TIME_PROCDATA  0x0420

/* ---- SII EEPROM interface (§2.11) ---- */
#define REG_SII_BASE             0x0500  /* EEPROM ECAT access state (1 byte) */
#define REG_SII_PDI_ACCESS       0x0501
#define REG_SII_CONTROL_STATUS   0x0502  /* 2 byte, bit15 = Busy */
#define REG_SII_ADDRESS          0x0504  /* 4 byte (only low word used here) */
#define REG_SII_DATA             0x0508  /* 4-8 byte */
#define REG_SII_END              0x050F
#define SII_BUSY_BIT_MASK        0x80    /* bit15 -> high byte of 0x0502 */

/* ---- FMMU (Table 1: 16 entries x 16 byte, 0x0600:0x06FF) ---- */
#define REG_FMMU_BASE          0x0600
#define REG_FMMU_ENTRY_SIZE    16
#define REG_FMMU_COUNT         3       /* entries actually iterated in logic */
#define REG_FMMU_AREA_END      0x06FF  /* end of address space (16 entries) */
#define FMMU_OFF_LOG_START      0x0   /* 4 byte */
#define FMMU_OFF_LENGTH         0x4   /* 2 byte */
#define FMMU_OFF_LOG_START_BIT  0x6
#define FMMU_OFF_LOG_STOP_BIT   0x7
#define FMMU_OFF_PHYS_START     0x8   /* 2 byte */
#define FMMU_OFF_PHYS_START_BIT 0xA
#define FMMU_OFF_TYPE           0xB   /* bit0=read, bit1=write */
#define FMMU_OFF_ACTIVATE       0xC   /* bit0=enable */

/* ---- SyncManager (16 entries x 8 byte, 0x0800:0x087F) ---- */
#define REG_SM_BASE            0x0800
#define REG_SM_ENTRY_SIZE      8
#define REG_SM_COUNT           4       /* entries actually iterated in logic */
#define REG_SM_AREA_END        0x087F
#define SM_OFF_PHYS_START      0x0   /* 2 byte */
#define SM_OFF_LENGTH          0x2   /* 2 byte */
#define SM_OFF_CONTROL         0x4
#define SM_OFF_STATUS          0x5
#define SM_OFF_ACTIVATE        0x6
#define SM_OFF_PDI_CONTROL     0x7

/* SM index convention (standard EtherCAT, confirmed against esc_build_sii()'s
 * own sm_index arguments: RxPDO(outputs)=SM2, TxPDO(inputs)=SM3):
 *   SM0 = mailbox out (master -> slave), SM1 = mailbox in (slave -> master).
 * SM1's Status byte (bit3 = "mailbox full") is the exact physical register
 * SOEM's ecx_config_create_mbxstatus_mappings() maps an FMMU onto
 * (ECT_REG_SM1STAT in SOEM's own ec_type.h) -- computed here, not
 * independently guessed, from the same REG_SM_BASE/ENTRY_SIZE/OFF_STATUS
 * this file already defines. */
#define REG_SM1_STATUS  (REG_SM_BASE + 1 * REG_SM_ENTRY_SIZE + SM_OFF_STATUS) /* 0x080D */
#define SM_STATUS_MAILBOX_FULL  0x08

/* ---- Distributed Clock — not used at this stage, reserved for later ---- */
#define REG_DC_RECV_TIME_PORT0 0x0900
#define REG_DC_SYSTEM_TIME     0x0910
#define REG_DC_SYSTEM_OFFSET   0x0920
#define REG_DC_SYSTEM_DELAY    0x0928
#define REG_DC_SYNC0_CYCLE     0x09A0

/* ---- DPRAM ---- */
#define REG_DPRAM_BASE          0x1000

/* ---- ESM state (Section II §2.5.1/2.5.2) ---- */
#define ESM_INIT    0x01
#define ESM_PREOP   0x02
#define ESM_BOOT    0x03
#define ESM_SAFEOP  0x04
#define ESM_OP      0x08

/* ---- AL Status Code (ETG.1000-6) ---- */
#define ALSTATUSCODE_NOERROR              0x0000
#define ALSTATUSCODE_INVALIDALCONTROL     0x0011
#define ALSTATUSCODE_UNKNOWNALCONTROL     0x0012
#define ALSTATUSCODE_NOMAILBOXTIMEOUT     0x0016
#define ALSTATUSCODE_INVALIDMBXCONFIG     0x0017
#define ALSTATUSCODE_NOVALIDOUTPUTS       0x0019
#define ALSTATUSCODE_SYNCMANWATCHDOG      0x001B

/* ==========================================================================
 * esc_t — one ESC node in the chain.
 *
 * Deliberately has NO dedicated fields for DL/AL Status, FMMU, SM — those
 * live in regs[] at the exact datasheet offset.
 *
 * The SII image is now an OWNED buffer, generated per node in esc_init()
 * (see esc_build_sii() in esc_core.c) instead of a shared pointer to a
 * static array — this is what lets each node reflect its own pdo_size_bytes
 * in what the master reads back over SII.
 * ========================================================================== */
typedef struct {
    uint32_t kp, ki, kd;   /* 0x8000:01/02/03 -- read/write test PID params,
                            * exercises L4-02 (SDOwrite while OP, PDO not
                            * interrupted). Not tied to any real control
                            * loop -- pure protocol-level test storage. */
    uint8_t  segtest_blob[COE_SEGTEST_BLOB_SIZE];
                           /* 0x8001:00 -- fixed, read-only, incrementing byte
                            * pattern (regenerated in coe_od_init()). Larger
                            * than one mailbox buffer (SII_SM0_SIZE/SM1_SIZE
                            * = 128 byte, ~112 usable) so a plain SDOread on
                            * it can ONLY complete via genuine multi-frame
                            * segmentation -- exercises L4-05. */
} coe_od_t;

/* One in-flight segmented SDO transfer per node -- CoE continuation
 * frames (upload segment request / download segment data) carry NO
 * Index/SubIndex of their own (see esc_coe.c for why), so which
 * object/subindex/direction/offset they belong to must be remembered
 * here between mailbox exchanges. Only one transfer at a time per
 * node, matching a single master driving a single mailbox session per
 * slave -- sufficient for this test rig, not a general multi-session
 * CoE stack. */
typedef struct {
    uint8_t  active;
    uint8_t  is_upload;     /* 1 = upload (slave->master) in progress */
    uint16_t index;
    uint8_t  subindex;
    uint32_t total_size;
    uint32_t done;          /* bytes sent (upload) or received (download) so far */
    uint8_t  expected_toggle; /* next continuation frame's expected toggle bit (0x00/0x10) */
} coe_session_t;

typedef struct {
    uint8_t   regs[ESC_REG_SPACE_SIZE];

    uint16_t  station_address;   /* cache of regs[0x0010] */
    uint16_t  station_alias;     /* cache of regs[0x0012] */
    uint8_t   alias_enabled;     /* set by master via DL control bit24 */
    uint8_t   position_in_chain; /* 0-indexed */
    uint16_t  pdo_size_bytes;    /* PARAMETERIZED — not hardcoded */
    uint8_t   got_valid_outputs; /* Reset each time SAFEOP is re-entered */
    uint8_t   force_reject_al;     /* Test/CLI hook: force the NEXT AL Control
                                 * request to be rejected regardless of the
                                 * normal transition rules (L2-05 scenario) */

    uint16_t  sii_image_buf[ESC_SII_IMAGE_MAX_WORDS]; /* generated per node */
    size_t    sii_image_words;                        /* words actually used */

    coe_od_t      coe_od;      /* CoE object dictionary storage, this node's own */
    coe_session_t coe_session; /* in-flight segmented SDO transfer, if any */
    esc_dc_state_t dc;           /* Phase 6: Distributed Clock (esc_dc.c) */
} esc_t;


/* Initializes everything INDEPENDENT of topology, including building this
 * node's own SII image from pdo_size_bytes. Does not touch the network —
 * testable offline (test_offline.c) with no veth/socket involved. */
void esc_init(esc_t *esc, uint8_t position_in_chain, uint16_t pdo_size_bytes);

/* Sets DL Status based on chain position. Call AFTER esc_init on the whole
 * array — port link state is a relationship BETWEEN nodes, not a property
 * of a single node in isolation. */
void esc_chain_wire(esc_t *chain, int n);

/* ESM: called whenever a write physically lands on AL Control (0x0120).
 * Applies the valid state-transition graph, updates AL Status / AL Status
 * Code in regs[] accordingly. Phase 3 scope. */
void esc_al_control_write(esc_t *esc);

#endif /* ESC_TYPES_H */