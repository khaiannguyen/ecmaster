/*
 * ecm_eni.h -- GD8 8.4: ENI-derived network configuration for ecm_run.
 *
 * The ENI (TwinCAT export) is converted offline by tools/eni/eni2cfg.py into
 * a line-based .enicfg file; this module loads it at startup (non-RT) and
 * provides the checks ecm_run runs against the scanned bus. It has no SOEM
 * dependency (the SOEM glue lives in ecm_eni_soem.c), so it is unit-tested
 * offline (test_eni_offline.c).
 *
 * What the ENI decides in ecm_run (docs/eni.md section 5):
 *   - the expected slave list: count, and vendor/product(/revision) per
 *     position                                   -> ecm_eni_check_identity (E-03)
 *   - the expected process data size per slave   -> ecm_eni_check_layout
 *   - CoE InitCmds with their transitions        -> run by ecm_eni_soem.c,
 *                                                   any failure aborts (E-04)
 *   - DC: which slaves, reference clock, SYNC0/1 cycle, shift, AssignActivate
 * What it does NOT decide: SM/FMMU/logical layout, station addresses, DC
 * delay/offset/start time -- SOEM and libecmaster compute those at runtime.
 */
#ifndef ECM_ENI_H
#define ECM_ENI_H

#include <stddef.h>
#include <stdint.h>

#define ECM_ENI_MAX_SLAVES   64
#define ECM_ENI_MAX_COE      256
#define ECM_ENI_MAX_DATA     64     /* bytes of one CoE InitCmd payload */
#define ECM_ENI_NAME_LEN     64

/* ESM transitions, one bit each (a CoE InitCmd may list several) */
enum {
    ECM_ENI_T_IP = 1u << 0,  ECM_ENI_T_PI = 1u << 1,
    ECM_ENI_T_PS = 1u << 2,  ECM_ENI_T_SP = 1u << 3,
    ECM_ENI_T_SO = 1u << 4,  ECM_ENI_T_OS = 1u << 5,
    ECM_ENI_T_SI = 1u << 6,  ECM_ENI_T_OI = 1u << 7,
    ECM_ENI_T_IB = 1u << 8,  ECM_ENI_T_BI = 1u << 9,
    ECM_ENI_T_II = 1u << 10, ECM_ENI_T_PP = 1u << 11,
    ECM_ENI_T_SS = 1u << 12, ECM_ENI_T_OO = 1u << 13,
};

#define ECM_ENI_CCS_DOWNLOAD 1
#define ECM_ENI_CCS_UPLOAD   2

typedef struct {
    uint16_t pos;                 /* 1-based bus position                    */
    char     name[ECM_ENI_NAME_LEN];
    uint32_t vendor, product, rev;
    uint8_t  check_rev;           /* ENI asked for a revision check          */
    uint16_t addr;                /* configured station address in the ENI   */
    uint32_t osize_bits, isize_bits;
    uint8_t  dc, refclock;
    uint32_t sync0_ns, sync1_ns;
    int32_t  shift_ns;
    uint16_t assign;              /* DC AssignActivate (0x0980/0x0981)       */
} ecm_eni_slave_t;

typedef struct {
    uint16_t pos;
    uint16_t trans;               /* ECM_ENI_T_* mask                        */
    uint8_t  ccs;                 /* ECM_ENI_CCS_*                           */
    uint16_t index;
    uint8_t  sub;
    uint8_t  ca;                  /* complete access                         */
    uint32_t timeout_ms;          /* 0 = master default                      */
    uint16_t len;
    uint8_t  data[ECM_ENI_MAX_DATA];
} ecm_eni_coe_t;

typedef struct {
    char     source[128];         /* ENI file name the .enicfg came from     */
    uint32_t cycle_us;
    int      nslaves;
    ecm_eni_slave_t slave[ECM_ENI_MAX_SLAVES];   /* slave[i].pos == i + 1    */
    int      ncoe;
    ecm_eni_coe_t coe[ECM_ENI_MAX_COE];          /* file order = run order   */
} ecm_eni_t;

/* What the master actually found for one position (filled from SOEM's
 * slavelist by ecm_eni_soem.c, or by hand in the offline test). */
typedef struct {
    uint32_t vendor, product, rev;
    uint32_t obits, ibits;        /* after ecx_config_map_group              */
} ecm_eni_found_t;

/* Load / parse. Return 0 on success, -1 with a message in err. */
int ecm_eni_load(const char *path, ecm_eni_t *eni, char *err, size_t errlen);
int ecm_eni_parse_text(const char *text, ecm_eni_t *eni, char *err, size_t errlen);

/* E-03: slave count and identity per position. found[0] is position 1.
 * Vendor and product always, revision only where the ENI set check_rev.
 * Returns the number of mismatches (0 = OK); every mismatch is described in
 * err, one per line (truncated to errlen). */
int ecm_eni_check_identity(const ecm_eni_t *eni, int nfound,
                           const ecm_eni_found_t *found, char *err, size_t errlen);

/* Layout: process data size per position after mapping. Same return rule. */
int ecm_eni_check_layout(const ecm_eni_t *eni, int nfound,
                         const ecm_eni_found_t *found, char *err, size_t errlen);

/* Parse "IP,PS" into a mask; 0 if any token is unknown. */
uint16_t ecm_eni_trans_parse(const char *list);
/* "PS" etc. for a single bit, "?" otherwise. */
const char *ecm_eni_trans_name(uint16_t bit);

/* Reference clock position, 0 if the ENI has no DC slave. */
int ecm_eni_refclock(const ecm_eni_t *eni);

#endif
