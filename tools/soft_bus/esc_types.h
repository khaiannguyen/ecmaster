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
#define ESC_SII_IMAGE_MAX_WORDS  1024

/* Size of the fixed CoE test blob (object 0x8001:00) used to force
 * genuine multi-frame SDO segmentation -- see coe_od_t below. Must
 * exceed one mailbox buffer's usable payload (SII_SM0_SIZE/SM1_SIZE =
 * 128 byte, ~112 usable after the 16-byte SDO header). */
#define COE_SEGTEST_BLOB_SIZE  200

/* Phase 9.3: 0x8002, read/write OCTET_STRING (1..COE_OCTET_RW_MAX bytes) --
 * the target of normal and segmented SDO download tests (L4-07, X-04).
 * 400 byte with 128 byte mailboxes = init frame (112) + 3 segments. */
#define COE_OCTET_RW_MAX       400
/* Largest download (normal or segmented) one session buffers before it is
 * checked and applied atomically; also the scratch size of one serialized
 * object (Complete Access of a 0x1600 with 64 entries = 2 + 64*4 bytes). */
#define COE_XFER_BUF_MAX       512

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
#define REG_ERR_INVALID_P(p)  (0x0300 + 2 * (p))  /* invalid frame counter, port p   */
#define REG_ERR_RX_P(p)       (0x0301 + 2 * (p))  /* RX (physical) error, port p     */
#define REG_ERR_FWD_P(p)      (0x0308 + (p))      /* forwarded RX error, port p      */
#define REG_ERR_ECAT_PU       0x030C              /* ECAT processing unit error      */
#define REG_ERR_PDI           0x030D              /* PDI0 error counter (+code 0x030E) */
#define REG_LOST_LINK_P(p)    (0x0310 + (p))      /* lost link counter, port p       */

/* ---- Watchdog (§2.10) ---- */
#define REG_WD_DIVIDER        0x0400
#define REG_WD_TIME_PDI0      0x0410
#define REG_WD_TIME_PROCDATA  0x0420
#define REG_WD_STATUS_PD      0x0440  /* bit0: 0 = expired, 1 = active or disabled */
#define REG_WD_COUNTER_PD     0x0442  /* expirations, saturates at 0xFF */
#define WD_DIVIDER_RESET      0x09C2  /* 2498 -> 100 us base tick (§2.10.1) */
#define WD_TIME_RESET         0x03E8  /* 1000 ticks -> 100 ms (§2.10.4) */

/* ---- SII EEPROM interface (§2.11) ---- */
#define REG_SII_BASE             0x0500  /* EEPROM ECAT access state (1 byte) */
#define REG_SII_PDI_ACCESS       0x0501
#define REG_SII_CONTROL_STATUS   0x0502  /* 2 byte, bit15 = Busy */
#define REG_SII_ADDRESS          0x0504  /* 4 byte (only low word used here) */
#define REG_SII_DATA             0x0508  /* 4-8 byte */
#define REG_SII_END              0x050F
#define SII_BUSY_BIT_MASK        0x80    /* bit15 -> high byte of 0x0502 */

/* 0x0502 low byte */
#define SII_WRITE_ENABLE   0x01   /* bit 0, self-clearing */
/* 0x0503 (high byte of 0x0502) */
#define SII_CMD_MASK       0x07   /* bits [10:8] */
#define SII_CMD_NOP        0x00
#define SII_CMD_READ       0x01
#define SII_CMD_WRITE      0x02
#define SII_CMD_RELOAD     0x04
#define SII_ERR_ACK_CMD    0x20   /* bit 13: missing ack / invalid command */
#define SII_ERR_WRITE_EN   0x40   /* bit 14: write without write enable */
#define SII_ERR_MASK       (SII_ERR_ACK_CMD | SII_ERR_WRITE_EN)

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
#define SM_CTRL_DIR_MASK       0x0C  /* bits 3:2: 00 = ECAT read, 01 = ECAT write */
#define SM_CTRL_DIR_WRITE      0x04
#define SM_CTRL_WD_TRIGGER     0x40  /* bit6: watchdog trigger enable (§2.14.3) */
#define SM_ACT_ENABLE          0x01  /* activate register bit0 */
#define SM_ACT_REPEAT_REQ      0x02  /* activate register bit1 (master toggles) */
#define SM_PDI_REPEAT_ACK      0x02  /* PDI control register bit1 (slave toggles) */

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
    uint8_t  octet_rw[COE_OCTET_RW_MAX];  /* Phase 9.3: 0x8002:00 value          */
    uint16_t octet_rw_len;                /* its current length (1..MAX)     */
    uint8_t  pdo_assign_n[2];     /* Phase 9.3: 0x1C12:00 / 0x1C13:00 (0 or 1)   */
    uint16_t pdo_assign_idx[2];   /* 0x1C12:01 / 0x1C13:01 (0x1600/0x1A00)   */
    uint8_t  resp_cnt;     /* [Phase 7.4] mailbox counter of the last response
                            * (ETG.1000.4: 1..7, 0 reserved). A NEW response
                            * gets the next value; a repeated or duplicated
                            * one keeps it -- that is how a master tells them
                            * apart (L5-12). */
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
    uint8_t  is_ca;         /* Phase 9.3: Complete Access transfer               */
    uint8_t  xfer[COE_XFER_BUF_MAX]; /* Phase 9.3: download data received so far  */
} coe_session_t;

/* Phase 7: per-node fault-injection state. Frame-scoped flags (*_frame)
 * are set by esc_fault_frame_begin() and only live for one frame. */
typedef struct {
    uint8_t  powered_off;       /* drop_node: node is gone from the bus        */
    uint32_t wkc_short_left;    /* PD frames left in which L* is ignored       */
    uint32_t stale_left;        /* PD frames left with frozen TxPDO            */
    uint8_t  skip_logical;      /* this frame: ignore L* datagrams             */
    uint8_t  stale_frame;       /* this frame: do not refresh TxPDO            */
    uint8_t  sm1_consumed;      /* this frame: master read the SM1 mailbox     */
    uint8_t  mbx_lose_armed;    /* mbx_repeat: lose the reply of the next read */
    uint8_t  mbx_dup_armed;     /* mbx_dup: re-post the next response once     */
    uint8_t  mbx_dup_pending;   /* re-post at the start of the next frame      */
    uint16_t app_seq;           /* --app-seq: slave application counter        */
    uint64_t mbx_repeats_served;/* master repeat requests answered (0x080E)    */
    uint32_t coe_delay_ms;      /* Phase 9.6: SDO download responses held this long */
    uint32_t emcy_left;         /* Phase 9.7: EMCY still to post (ctl "emcy")      */
    uint16_t emcy_code;
    uint8_t  emcy_reg;
    uint16_t emcy_seq;          /* data[0..1] = sequence number, LE (order check) */
    uint64_t emcy_posted;
    uint8_t  mbx_held;          /* a response is in SM1 but not yet "full"     */
    uint64_t mbx_release_ns;    /* when it becomes visible (frame arrival time)*/
    uint16_t reject_al_code;    /* Phase 9.7: AL status code of the next rejection (0 = 0x0012) */
    uint8_t  reject_al_state;   /* Phase 9.7: only reject a request TO this state (0 = any)      */
    uint8_t  reject_al_sticky;  /* Phase 9.7: reject every such request until "clear"            */
} esc_node_fault_t;

/* Phase 7: process data watchdog (Section I §13.1, Section II §2.10). */
typedef struct {
    uint64_t now_ns;            /* arrival time of the frame being processed   */
    uint64_t last_trigger_ns;   /* last complete write to a trigger-enabled SM */
    uint8_t  running;           /* triggered and not yet expired               */
    uint8_t  react;             /* slave application drops OP on expiry        */
    uint64_t expire_events;
} esc_wd_state_t;

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
    esc_node_fault_t fault;      /* Phase 7: per-node fault injection (esc_fault.c) */
    esc_wd_state_t   wd;         /* Phase 7: process data watchdog model */
    uint32_t  sii_cmd_reads;
    uint8_t   coe_pdo_od;  /* Phase 9.3 --coe-pdo-od: 0x1C00/0x1C12/0x1C13/0x1600/0x1A00 */
    uint8_t   coe_ca;      /* Phase 9.3 --coe-ca: SDO Complete Access + SII General cat. */
    /* Phase 9.9: mailbox location of THIS node (SII words 24..27). The default
     * soft_bus node keeps SII_SM0/SM1_OFFSET; a node with a profile takes
     * its ESI's MBoxOut/MBoxIn (IS620N: 0x1000 / 0x1400). */
    uint16_t  mbx_out, mbx_out_len, mbx_in, mbx_in_len;
    const struct esc_profile *prof;      /* Phase 9.9: NULL = built-in SOFTBUS-PD4 */
    struct esc_prof_state    *prof_st;   /* Phase 9.9: this node's OD values      */
} esc_t;


/* Initializes everything INDEPENDENT of topology, including building this
 * node's own SII image from pdo_size_bytes. Does not touch the network —
 * testable offline (test_offline.c) with no veth/socket involved. */
void esc_init(esc_t *esc, uint8_t position_in_chain, uint16_t pdo_size_bytes);

/* Phase 9.3: optional CoE features, both off by default (the wire traffic of a
 * default soft_bus does not change). pdo_od adds the PDO mapping/assign
 * objects to the object dictionary; ca adds SDO Complete Access and an SII
 * General category advertising it (CoE details SDO|PDOASSIGN|SDOCA), and
 * implies pdo_od. Rebuilds the SII image, so call it after esc_init() and
 * before any --sii-poke. */
void esc_set_coe_features(esc_t *esc, int pdo_od, int ca);

/* Sets DL Status based on chain position. Call AFTER esc_init on the whole
 * array — port link state is a relationship BETWEEN nodes, not a property
 * of a single node in isolation. */
void esc_chain_wire(esc_t *chain, int n);

/* ESM: called whenever a write physically lands on AL Control (0x0120).
 * Applies the valid state-transition graph, updates AL Status / AL Status
 * Code in regs[] accordingly. Phase 3 scope. */
void esc_al_control_write(esc_t *esc);

#endif /* ESC_TYPES_H */