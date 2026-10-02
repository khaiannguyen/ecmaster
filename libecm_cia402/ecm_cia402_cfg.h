/* ==========================================================================
 * ecm_cia402_cfg.h — Phase 10.3: CiA402 axis configuration, the first part
 * of libecm_cia402 (the CiA402 layer kept OUT of libecmaster: every object
 * index with a CiA402 meaning lives here, libecmaster only binds numbers).
 *
 * An axis is (slave, n): drive object index + 0x800 * n (ETG.6010 multi-
 * axis). Pure logic, no SOEM: unit tested offline (test_cia402_cfg.c).
 *
 *   ecm_axis_parse_list("1:0,2:0", ...)   axes from the command line
 *   ecm_axis_load_cfg("config/axes.cfg")  or from a file
 *   ecm_axis_bind(...)                    every object the requested modes
 *                                         need, through ecm_pdo_bind()
 *                                         (Phase 9.10); missing -> refused,
 *                                         naming axis, mode and object
 *   ecm_axis_check_modes(...)             0x6502 (supported drive modes)
 *
 * Which objects a mode needs (CiA 402, process data):
 *   always      0x6040 controlword (out), 0x6041 statusword (in)
 *   CSP, PP     0x607A target position (out), 0x6064 position actual (in)
 *   HM          0x6064 position actual (in)
 *   CSV, PV     0x60FF target velocity (out), 0x606C velocity actual (in)
 *   CST         0x6071 target torque (out), 0x6077 torque actual (in)
 *   > 1 mode    0x6060 / 0x6061 in the PDOs: the mode changes at run time.
 *               With one mode they may be absent: the mode is then set by
 *               SDO / ENI InitCmd before OP (IS620N 0x1701/0x1B01).
 *   optional    0x603F error code (in)
 * ========================================================================== */
#ifndef ECM_CIA402_CFG_H
#define ECM_CIA402_CFG_H

#include <stddef.h>
#include <stdint.h>

#include "../libecmaster/pdo/ecm_pdo.h"

#define ECM_AXIS_MAX       32
#define ECM_AXIS_OFFSET    0x800u

/* modes as bits of 0x6502 (CiA 402) */
#define ECM_MODE_PP   (1u << 0)
#define ECM_MODE_VL   (1u << 1)
#define ECM_MODE_PV   (1u << 2)
#define ECM_MODE_TQ   (1u << 3)
#define ECM_MODE_HM   (1u << 5)
#define ECM_MODE_IP   (1u << 6)
#define ECM_MODE_CSP  (1u << 7)
#define ECM_MODE_CSV  (1u << 8)
#define ECM_MODE_CST  (1u << 9)
#define ECM_MODE_KNOWN (ECM_MODE_PP | ECM_MODE_PV | ECM_MODE_HM | ECM_MODE_CSP | ECM_MODE_CSV | ECM_MODE_CST)

typedef struct {
    uint16_t slave;           /* 1-based SOEM slave number            */
    uint8_t  n;               /* axis on that slave (0x800 * n)        */
    uint32_t modes;           /* ECM_MODE_* requested                  */
    char     name[24];        /* "1:0" unless the cfg file names it    */
} ecm_axis_cfg_t;

/* Bound process data of one axis. A handle with bits == 0 is absent. */
typedef struct {
    ecm_pdo_handle_t cw, sw;          /* 0x6040 / 0x6041                  */
    ecm_pdo_handle_t tpos, apos;      /* 0x607A / 0x6064                  */
    ecm_pdo_handle_t tvel, avel;      /* 0x60FF / 0x606C                  */
    ecm_pdo_handle_t ttq, atq;        /* 0x6071 / 0x6077                  */
    ecm_pdo_handle_t mode, mode_disp; /* 0x6060 / 0x6061                  */
    ecm_pdo_handle_t err;             /* 0x603F (optional)                */
    uint8_t          mode_by_sdo;     /* 0x6060 not in the PDOs           */
} ecm_axis_bind_t;

/* "csp,csv,pp" -> ECM_MODE_* bits. 0 ok, -1 unknown name (in err). */
int ecm_axis_parse_modes(const char *s, uint32_t *modes, char *err, size_t errlen);

/* "1:0,2:0" or "1,2" (n = 0): appends to ax[*nax] with the given modes.
 * 0 ok, -1 syntax / too many / duplicate (in err). */
int ecm_axis_parse_list(const char *s, uint32_t modes, ecm_axis_cfg_t *ax, int *nax, int max,
                        char *err, size_t errlen);

/* Axes file, one per line ('#' comments):
 *     axis <slave>[:<n>] modes <list> [name <name>]
 * Returns 0 ok, -1 with "file:line: reason" in err. */
int ecm_axis_load_cfg(const char *path, ecm_axis_cfg_t *ax, int *nax, int max, char *err, size_t errlen);

/* Object index of base (0x6040 ...) for this axis. */
uint16_t ecm_axis_index(const ecm_axis_cfg_t *a, uint16_t base);

/* Bind every object the axis' modes need. 0 ok; -1: err names the axis,
 * the mode that needs the object, the object and its direction, and what
 * the slave does map (ecm_pdo_bind). */
int ecm_axis_bind(const ecm_axis_cfg_t *a, const ecm_pdo_table_t *t, const ecm_pdo_loc_t *loc,
                  ecm_axis_bind_t *b, char *err, size_t errlen);

/* Every requested mode in 0x6502? 0 ok, -1 err names the axis, the modes
 * missing and the 0x6502 value. */
int ecm_axis_check_modes(const ecm_axis_cfg_t *a, uint32_t supported, char *err, size_t errlen);

/* "CSP,CSV" for logs. */
size_t ecm_axis_modes_str(uint32_t modes, char *buf, size_t cap);

#endif /* ECM_CIA402_CFG_H */
