/* ==========================================================================
 * ecm_cia402_diag.h — Phase 10.8: axis diagnosis for the CiA402 layer.
 * Pure logic (no SOEM, not RT): unit tested offline (test_cia402_diag.c).
 *
 *   EMCY / 0x603F code -> text: a vendor table (config/emcy/NAME.emcy, loaded
 *     at start, codes from the drive manual / R-07 on the real drive) first,
 *     then the CiA 402 / ETG codes below, else the CiA 301 class only.
 *   Attribution of a slave's EMCY to an axis: EMCY carries no axis number.
 *     Code of an axis in fault, in this order:
 *       1. its own 0x603F (process data), when mapped and not 0
 *       2. the slave's last EMCY, when the slave has one axis, or exactly
 *          one of its axes is in fault
 *       3. none ("not attributable"), and the reason is printed
 *   One line per axis: state, mode, axis error, code + text + source, the
 *   slave's last EMCY when it was not attributed.
 * ========================================================================== */
#ifndef ECM_CIA402_DIAG_H
#define ECM_CIA402_DIAG_H

#include <stddef.h>
#include <stdint.h>

#include "ecm_cia402_axis.h"

#define ECM_EMCY_VENDOR_MAX 256

/* Text of an error code; NULL when neither the vendor table (for this
 * vendor id) nor the CiA 402 / ETG table knows it. */
const char *ecm_cia402_emcy_text(uint16_t code, uint32_t vendor);

/* Vendor table file:
 *     vendor 0x00100000          # the slave's vendor id (SII), required first
 *     0xFF01 overcurrent ...     # code, then the text up to the end of line
 * '#' starts a comment. Several files may be loaded (several vendors).
 * 0 ok, -1 "file:line: reason" in err. */
int  ecm_cia402_emcy_load(const char *path, char *err, size_t errlen);
void ecm_cia402_emcy_clear(void);
int  ecm_cia402_emcy_count(void);

typedef struct {
    int      have;            /* the slave sent at least one EMCY      */
    uint16_t code;
    uint8_t  reg;
    uint64_t tick;
} ecm_cia402_emcy_in_t;

typedef enum { ECM_CODE_NONE = 0, ECM_CODE_603F, ECM_CODE_EMCY } ecm_code_src_t;

/* The code of an axis in fault (rules above). axes_on_slave / faulted_on_slave:
 * axes of the same slave configured / currently in Fault or Fault reaction. */
ecm_code_src_t ecm_cia402_axis_code(const ecm_cia402_state_t *s, int axes_on_slave, int faulted_on_slave,
                                    const ecm_cia402_emcy_in_t *last, uint16_t *code);

/* "axis 1:0 (slave 1): Fault, mode CSP, error drive fault; code 0x2310
 *  continuous over current [current] (0x603F, = EMCY)" */
size_t ecm_cia402_axis_diag(const char *name, uint16_t slave, const ecm_cia402_state_t *s,
                            int axes_on_slave, int faulted_on_slave, const ecm_cia402_emcy_in_t *last,
                            uint32_t vendor, char *buf, size_t cap);

#endif /* ECM_CIA402_DIAG_H */
