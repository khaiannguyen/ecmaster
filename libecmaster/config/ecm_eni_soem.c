/*
 * ecm_eni_soem.c -- SOEM glue for ecm_eni (GD8 8.4). Not RT: every call here
 * happens during startup or during a recovery that already owns the bus.
 */
#include "ecm_eni_soem.h"

#include <stdio.h>
#include <string.h>

static const ecm_eni_t *g_hook_eni;
static int g_hook_failures;

int ecm_eni_soem_supported(const ecm_eni_t *eni)
{
    int bad = 0;
    for (int i = 0; i < eni->ncoe; i++) {
        const ecm_eni_coe_t *c = &eni->coe[i];
        uint16_t other = (uint16_t)(c->trans & ~ECM_ENI_SOEM_TRANS);
        if (other) {
            fprintf(stderr, "ecm_eni: slave %u CoE 0x%04X:%02X: transition(s)", c->pos, c->index, c->sub);
            for (uint16_t b = 1; b; b <<= 1)
                if (other & b)
                    fprintf(stderr, " %s", ecm_eni_trans_name(b));
            fprintf(stderr, " not supported by ecm_run (only IP, PS)\n");
            bad++;
        }
        /* GD9.3: Complete Access InitCmds are run with ecx_SDOwrite/
         * ecx_SDOread(CA = TRUE), data verbatim from the ENI. SOEM itself
         * clamps a CA subindex > 1 to 1; the ENI never has one (ETG.2100). */
    }
    return bad;
}

static int check_common(const char *what, int bad, const char *msg)
{
    if (bad) {
        fprintf(stderr, "ecm_eni: %s check FAILED (%d):\n%s", what, bad, msg);
    } else {
        fprintf(stderr, "ecm_eni: %s check OK\n", what);
    }
    return bad;
}

int ecm_eni_soem_check_identity(ecx_contextt *ctx, const ecm_eni_t *eni)
{
    static ecm_eni_found_t found[ECM_ENI_MAX_SLAVES];
    static char msg[4096];
    int n = ctx->slavecount;
    for (int s = 1; s <= n && s <= ECM_ENI_MAX_SLAVES; s++) {
        found[s - 1].vendor = ctx->slavelist[s].eep_man;
        found[s - 1].product = ctx->slavelist[s].eep_id;
        found[s - 1].rev = ctx->slavelist[s].eep_rev;
    }
    if (n > ECM_ENI_MAX_SLAVES)
        n = ECM_ENI_MAX_SLAVES + 1; /* guaranteed count mismatch */
    int bad = ecm_eni_check_identity(eni, n, found, msg, sizeof(msg));
    return check_common("identity", bad, msg);
}

int ecm_eni_soem_check_layout(ecx_contextt *ctx, const ecm_eni_t *eni)
{
    static ecm_eni_found_t found[ECM_ENI_MAX_SLAVES];
    static char msg[4096];
    int n = ctx->slavecount > ECM_ENI_MAX_SLAVES ? ECM_ENI_MAX_SLAVES : ctx->slavecount;
    for (int s = 1; s <= n; s++) {
        found[s - 1].obits = ctx->slavelist[s].Obits;
        found[s - 1].ibits = ctx->slavelist[s].Ibits;
    }
    int bad = ecm_eni_check_layout(eni, n, found, msg, sizeof(msg));
    return check_common("process data layout", bad, msg);
}

/* GD9.3: the error list is shared with SOEM's own configuration. When the
 * PS InitCmds run from the PO2SO hook, the list may still hold what SOEM
 * queued for the PREVIOUS slave after its hook (for example the abort of
 * its 0x1C00 read before it falls back to the SII PDO categories), and
 * ecx_elist2string() prints one entry per call: the old code therefore
 * reported another slave's abort for a failed InitCmd. Now the list is
 * emptied before each InitCmd and every entry it leaves is printed. */
static int print_errors(ecx_contextt *ctx)
{
    ec_errort e;
    int n = 0;
    while (ecx_poperror(ctx, &e)) {
        if (e.Etype == EC_ERR_TYPE_SDO_ERROR)
            fprintf(stderr, "ecm_eni:   slave %u 0x%04X:%02X abort 0x%08X %s\n", e.Slave, e.Index,
                    e.SubIdx, (unsigned)e.AbortCode, ec_sdoerror2string((uint32)e.AbortCode));
        else
            fprintf(stderr, "ecm_eni:   %s", ecx_err2string(e));
        n++;
    }
    return n;
}

static int run_one(ecx_contextt *ctx, const ecm_eni_coe_t *c)
{
    int timeout = c->timeout_ms ? (int)c->timeout_ms * 1000 : EC_TIMEOUTRXM;
    ec_errort stale;
    while (ecx_poperror(ctx, &stale)) { }
    int wkc;
    uint8_t buf[ECM_ENI_MAX_DATA];
    int size = (int)sizeof(buf);

    if (c->ccs == ECM_ENI_CCS_DOWNLOAD) {
        wkc = ecx_SDOwrite(ctx, c->pos, c->index, c->sub, c->ca ? TRUE : FALSE, c->len, c->data, timeout);
    } else {
        wkc = ecx_SDOread(ctx, c->pos, c->index, c->sub, c->ca ? TRUE : FALSE, &size, buf, timeout);
    }
    const char *ca = c->ca ? " (complete access)" : "";
    int err = ecx_iserror(ctx);
    if (wkc <= 0 || err) {
        fprintf(stderr, "ecm_eni: slave %u CoE %s 0x%04X:%02X%s FAILED (wkc=%d)\n",
                c->pos, c->ccs == ECM_ENI_CCS_DOWNLOAD ? "download" : "upload",
                c->index, c->sub, ca, wkc);
        if (err)
            print_errors(ctx);
        return 1;
    }
    fprintf(stderr, "ecm_eni: slave %u CoE %s 0x%04X:%02X%s ok (%d byte)\n", c->pos,
            c->ccs == ECM_ENI_CCS_DOWNLOAD ? "download" : "upload", c->index, c->sub, ca,
            c->ccs == ECM_ENI_CCS_DOWNLOAD ? c->len : size);
    return 0;
}

int ecm_eni_soem_run_transition(ecx_contextt *ctx, const ecm_eni_t *eni,
                                uint16_t trans, uint16_t slave)
{
    int fails = 0;
    for (int i = 0; i < eni->ncoe; i++) {
        const ecm_eni_coe_t *c = &eni->coe[i];
        if (!(c->trans & trans) || (slave && c->pos != slave))
            continue;
        if (c->pos > ctx->slavecount) {
            fprintf(stderr, "ecm_eni: CoE InitCmd for slave %u, bus has %d\n", c->pos, ctx->slavecount);
            fails++;
            continue;
        }
        fails += run_one(ctx, c);
    }
    return fails;
}

int ecm_eni_soem_po2so(ecx_contextt *ctx, uint16 slave)
{
    if (!g_hook_eni)
        return 1;
    int f = ecm_eni_soem_run_transition(ctx, g_hook_eni, ECM_ENI_T_PS, slave);
    g_hook_failures += f;
    return f == 0;
}

void ecm_eni_soem_arm_po2so(ecx_contextt *ctx, const ecm_eni_t *eni)
{
    g_hook_eni = eni;
    g_hook_failures = 0;
    for (int s = 1; s <= ctx->slavecount; s++)
        ctx->slavelist[s].PO2SOconfig = ecm_eni_soem_po2so;
}

int ecm_eni_soem_po2so_failures(void)
{
    return g_hook_failures;
}
