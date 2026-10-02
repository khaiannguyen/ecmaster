/* ==========================================================================
 * esc_profile.h — Phase 9.9: a soft_bus node that stands in for another slave
 * (a vendor servo, our P1 draft), described by a profile file generated
 * from that slave's ESI by tools/esi/esi2profile.py.
 *
 * A profile node differs from the built-in SOFTBUS-PD4 node in:
 *   - SII: identity, config area, mailbox location, CoE details (General
 *     category), SyncManagers and EVERY PDO of the ESI (not assigned by
 *     default -> SM 0xFF), all from the profile
 *   - CoE object dictionary: the ESI's (Profile/Dictionary), generic --
 *     read, write (ro / rw / rw in PREOP only), Complete Access when the
 *     ESI allows it. 0x1C12/0x1C13 accept only PDOs of their direction and
 *     refuse a set that the ESI's Exclude lists forbid
 *   - PREOP -> SAFEOP: the SM2/SM3 sizes the master configured must equal
 *     the PDOs currently assigned, else the node refuses with AL status
 *     code 0x001D (outputs) / 0x001E (inputs), like a real drive (IS620N
 *     on LinuxCNC with a PDO mismatch: 0x001E)
 * What it does NOT model: the drive itself (CiA402 state machine, motion);
 * inputs stay what the fault injection puts there (Phase 10 adds a servo model).
 * ========================================================================== */
#ifndef ESC_PROFILE_H
#define ESC_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "esc_types.h"

#define ESC_PROF_MAX_PDO      64
#define ESC_PROF_MAX_ENTRIES  32
#define ESC_PROF_MAX_EXCL     16
#define ESC_PROF_VAL_MAX      64     /* bytes of one dictionary entry */

/* access, same numbers as esc_coe.c ACC_* */
enum { ESC_PROF_RO = 0, ESC_PROF_RW = 1, ESC_PROF_RW_PREOP = 2 };

typedef struct {
    uint16_t index;
    uint8_t  sub;
    uint16_t bits;
    uint8_t  access;
    uint8_t  len;                      /* bytes = (bits + 7) / 8          */
    uint8_t  dflt[ESC_PROF_VAL_MAX];
} esc_prof_sub_t;

typedef struct {
    uint16_t index;
    uint8_t  code;                     /* 7 VAR, 8 ARRAY, 9 RECORD        */
    uint8_t  max_sub;                  /* highest subindex defined        */
    int      first, n;                 /* range in esc_profile_t.sub      */
} esc_prof_obj_t;

typedef struct {
    uint16_t index;
    uint8_t  dir;                      /* 0 rx (outputs), 1 tx (inputs)   */
    uint8_t  sm;                       /* default SM, 255 = not assigned  */
    uint8_t  fixed;
    uint8_t  n;
    uint32_t entry[ESC_PROF_MAX_ENTRIES];   /* index << 16 | sub << 8 | bits */
    uint8_t  nex;
    uint16_t ex[ESC_PROF_MAX_EXCL];
} esc_prof_pdo_t;

typedef struct esc_profile {
    char     name[64];
    char     path[256];
    uint32_t vendor, product, rev, serial;
    int      has_config;
    uint16_t config[7];
    uint16_t mbx_out, mbx_out_len, mbx_in, mbx_in_len;
    uint8_t  coe, sdoinfo, pdoassign, pdoconfig, ca;
    struct { uint16_t start, len; uint8_t ctrl, en; } sm[4];
    uint8_t  dc;
    uint16_t dc_assign;
    int      npdo;
    esc_prof_pdo_t pdo[ESC_PROF_MAX_PDO];
    int      nobj, nsub;
    esc_prof_obj_t *obj;
    esc_prof_sub_t *sub;
} esc_profile_t;

struct esc_prof_state {
    uint8_t *val;                      /* nsub x ESC_PROF_VAL_MAX          */
    int      ca_bypass;                /* inside a checked CA download     */
};

/* Parse a profile file. Returns NULL with a message in err. */
esc_profile_t *esc_prof_load(const char *path, char *err, size_t errlen);
void           esc_prof_free(esc_profile_t *p);

/* Make esc a node of this profile: mailbox location, OD values back to the
 * ESI defaults, SII image rebuilt. Call after esc_init() (and again after a
 * power cycle). 0 ok, -1 out of memory / SII does not fit. */
int esc_prof_attach(esc_t *esc, const esc_profile_t *p);

/* ---- dictionary, for esc_coe.c ---- */
const esc_prof_obj_t *esc_prof_obj(const esc_t *esc, uint16_t index);
/* global sub number (index into p->sub) of index:sub, or -1 */
int      esc_prof_sub(const esc_t *esc, uint16_t index, uint8_t sub);
uint8_t *esc_prof_value(const esc_t *esc, int gsub);
/* Value check beyond size/access (0x1C12/0x1C13 contents). 0 = ok, else
 * an SDO abort code. */
uint32_t esc_prof_check_value(const esc_t *esc, uint16_t index, uint8_t sub,
                              const uint8_t *data, uint32_t len);

/* A Complete Access download of 0x1C12/0x1C13 changes SI0 and SIk at once:
 * check the resulting set as a whole (the per-entry SI0 = 0 rule does not
 * apply). 0 ok, else an SDO abort code. */
uint32_t esc_prof_check_ca(const esc_t *esc, uint16_t index, uint8_t start,
                           const uint8_t *data, uint32_t len);

/* PREOP -> SAFEOP: does the SM2/SM3 configuration the master wrote match
 * the PDOs assigned now? 0 ok, else the AL status code to refuse with. */
uint16_t esc_prof_check_pd(const esc_t *esc);

/* Bits of the PDOs assigned now in one direction (0 out, 1 in). */
uint32_t esc_prof_assigned_bits(const esc_t *esc, int dir);

#endif /* ESC_PROFILE_H */
