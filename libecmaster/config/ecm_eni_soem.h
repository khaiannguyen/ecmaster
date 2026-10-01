/*
 * ecm_eni_soem.h -- apply an ecm_eni_t to a SOEM context (GD8 8.4).
 *
 * Order in ecm_run (docs/eni.md section 5):
 *   ecx_config_init()
 *   ecm_eni_soem_check_identity()      E-03: refuse on any mismatch
 *   ecm_eni_soem_run_transition(IP)    CoE InitCmds of INIT->PREOP
 *   ecm_eni_soem_arm_po2so()           PS InitCmds run from SOEM's
 *                                      PO2SOconfig hook, inside
 *                                      ecx_config_map_group(), BEFORE SOEM
 *                                      reads the PDO assignment -- the same
 *                                      point where SOEM's own ENI path runs
 *                                      them (ec_config.c, ecx_map_coe_soe)
 *   ecx_config_map_group() ...
 *   ecm_eni_soem_po2so_failures()      any PS failure -> refuse (E-04)
 *   ecm_eni_soem_check_layout()        Obits/Ibits vs ENI -> refuse
 *
 * Unlike SOEM's ENI path, every SDO failure is reported with its abort code
 * and makes ecm_run stop before SAFE-OP.
 */
#ifndef ECM_ENI_SOEM_H
#define ECM_ENI_SOEM_H

#include "ecm_eni.h"
#include "soem/soem.h"

/* Transitions ecm_run can execute. A CoE InitCmd whose transition list
 * contains nothing from this set is rejected at load time by
 * ecm_eni_soem_supported(), never silently skipped. */
#define ECM_ENI_SOEM_TRANS (ECM_ENI_T_IP | ECM_ENI_T_PS)

/* 0 if ecm_run can do everything the ENI asks for, else the number of
 * refusals, each printed: CoE InitCmds of other transitions than IP/PS,
 * and (GD9.6) ecm_eni_check_supported(): LRD/LWR process data (E-06) and
 * register InitCmds outside the known table (E-08; with allow_unknown_reg
 * they are printed as warnings and not counted). Call right after loading,
 * before the bus is touched. */
int ecm_eni_soem_supported(const ecm_eni_t *eni, int allow_unknown_reg);

/* GD9.6: SDO timeout of a CoE InitCmd whose ENI Timeout is 0 (default
 * EC_TIMEOUTRXM). ecm_run --sdo-timeout-ms. */
void ecm_eni_soem_set_sdo_timeout_us(int us);

/* E-03. Returns number of mismatches, prints each. */
int ecm_eni_soem_check_identity(ecx_contextt *ctx, const ecm_eni_t *eni);

/* Run all CoE InitCmds carrying `trans` (one ECM_ENI_T_* bit) for `slave`
 * (1-based), or for every slave when slave == 0, in file order.
 * Returns the number of failed commands (each printed with the SOEM error). */
int ecm_eni_soem_run_transition(ecx_contextt *ctx, const ecm_eni_t *eni,
                                uint16_t trans, uint16_t slave);

/* Register the PS hook on every slave. The ENI must outlive the context. */
void ecm_eni_soem_arm_po2so(ecx_contextt *ctx, const ecm_eni_t *eni);
/* The hook itself, for callers that chain it (ecm_run's recovery hook). */
int ecm_eni_soem_po2so(ecx_contextt *ctx, uint16 slave);
/* Failures accumulated by the hook since arming. */
int ecm_eni_soem_po2so_failures(void);

/* Layout after ecx_config_map_group(). Returns mismatches, prints each. */
int ecm_eni_soem_check_layout(ecx_contextt *ctx, const ecm_eni_t *eni);

#endif
