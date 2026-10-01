#ifndef ECM_PDO_SOEM_H
#define ECM_PDO_SOEM_H
/*
 * ecm_pdo_soem.h -- GD9.10: fill an ecm_pdo_table_t / ecm_pdo_loc_t from a
 * SOEM context. Not RT: call at PREOP after ecx_config_map_group(), before
 * any thread uses the bus.
 */
#include "ecm_pdo.h"
#include "soem/soem.h"

/* Scan one slave's mapping into t, in process image order:
 *   CoE (slave has CoE): 0x1C12 / 0x1C13 -> PDO numbers -> their entries
 *   otherwise, or if 0x1C12:00 cannot be read: SII TxPDO/RxPDO categories
 *   (only PDOs with a valid SM, exactly what SOEM sizes the IOmap from).
 * Returns 0 ok, -1 on a read error. *src is set to "coe" or "sii". */
int ecm_pdo_scan_slave(ecx_contextt *ctx, int slave, ecm_pdo_table_t *t, const char **src);

/* Every slave, 1..slavecount. Returns the number of slaves that failed. */
int ecm_pdo_scan(ecx_contextt *ctx, ecm_pdo_table_t *t);

/* Where each slave's outputs/inputs start in its group's IOmap. iomap[g]
 * is the buffer given to ecx_config_map_group() for group g. */
void ecm_pdo_locate(ecx_contextt *ctx, uint8 *const *iomap, int ngroups, ecm_pdo_loc_t *loc);

/* SOEM's Obits/Ibits against the table. Returns mismatches (printed). */
int ecm_pdo_soem_check(ecx_contextt *ctx, const ecm_pdo_table_t *t);

#endif
