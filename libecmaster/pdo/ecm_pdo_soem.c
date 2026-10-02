/*
 * ecm_pdo_soem.c -- Phase 9.10 SOEM glue for ecm_pdo (scan, locate, check).
 */
#include "ecm_pdo_soem.h"

#include <stdio.h>
#include <string.h>

#define SII_CAT_TXPDO 50   /* ECT_SII_PDO: inputs  */
#define SII_CAT_RXPDO 51   /* ECT_SII_PDO + 1: outputs */

static int sdo_u8(ecx_contextt *ctx, int s, uint16 idx, uint8 sub, uint8 *v)
{
    int sz = 1;
    *v = 0;
    return ecx_SDOread(ctx, (uint16)s, idx, sub, FALSE, &sz, v, EC_TIMEOUTRXM) > 0 ? 0 : -1;
}

static int sdo_u16(ecx_contextt *ctx, int s, uint16 idx, uint8 sub, uint16 *v)
{
    int sz = 2;
    uint16 x = 0;
    if (ecx_SDOread(ctx, (uint16)s, idx, sub, FALSE, &sz, &x, EC_TIMEOUTRXM) <= 0) return -1;
    *v = etohs(x);
    return 0;
}

static int sdo_u32(ecx_contextt *ctx, int s, uint16 idx, uint8 sub, uint32 *v)
{
    int sz = 4;
    uint32 x = 0;
    if (ecx_SDOread(ctx, (uint16)s, idx, sub, FALSE, &sz, &x, EC_TIMEOUTRXM) <= 0) return -1;
    *v = etohl(x);
    return 0;
}

static int scan_coe(ecx_contextt *ctx, int s, ecm_pdo_table_t *t)
{
    static const struct { uint16 obj; ecm_pdo_dir_t dir; } sm[2] = {
        { 0x1C12, ECM_PDO_OUT }, { 0x1C13, ECM_PDO_IN } };
    for (int k = 0; k < 2; k++) {
        uint8 npdo;
        if (sdo_u8(ctx, s, sm[k].obj, 0, &npdo)) return -1;
        for (int p = 1; p <= npdo; p++) {
            uint16 pdo;
            uint8 ne;
            if (sdo_u16(ctx, s, sm[k].obj, (uint8)p, &pdo) || sdo_u8(ctx, s, pdo, 0, &ne)) return -1;
            for (int e = 1; e <= ne; e++) {
                uint32 v;
                if (sdo_u32(ctx, s, pdo, (uint8)e, &v)) return -1;
                if (ecm_pdo_add(t, s, sm[k].dir, pdo, (uint16)(v >> 16), (uint8)(v >> 8), (uint16)(v & 0xFF)))
                    return -1;
            }
        }
    }
    return 0;
}

/* Same walk as SOEM's ecx_siiPDO(), keeping the entries. */
static int scan_sii(ecx_contextt *ctx, int s, ecm_pdo_table_t *t)
{
    static const struct { uint16 cat; ecm_pdo_dir_t dir; } cats[2] = {
        { SII_CAT_RXPDO, ECM_PDO_OUT }, { SII_CAT_TXPDO, ECM_PDO_IN } };
    uint8 eectl = ctx->slavelist[s].eep_pdi;
    int rc = 0;
    for (int k = 0; k < 2 && !rc; k++) {
        uint16 a = ecx_siifind(ctx, (uint16)s, cats[k].cat);
        if (!a) continue;
        uint16 len = (uint16)(ecx_siigetbyte(ctx, (uint16)s, a) | ecx_siigetbyte(ctx, (uint16)s, a + 1) << 8);
        a += 2;
        uint16 c = 1;
        int npdo = 0;
        do {
            uint16 pdo = (uint16)(ecx_siigetbyte(ctx, (uint16)s, a) | ecx_siigetbyte(ctx, (uint16)s, a + 1) << 8);
            uint8 ne = ecx_siigetbyte(ctx, (uint16)s, a + 2);
            uint8 smi = ecx_siigetbyte(ctx, (uint16)s, a + 3);
            a += 8;
            c += 4;
            for (int e = 0; e < ne; e++) {
                if (smi < EC_MAXSM) {
                    uint16 ix = (uint16)(ecx_siigetbyte(ctx, (uint16)s, a) | ecx_siigetbyte(ctx, (uint16)s, a + 1) << 8);
                    uint8 sub = ecx_siigetbyte(ctx, (uint16)s, a + 2);
                    uint8 bl = ecx_siigetbyte(ctx, (uint16)s, a + 5);
                    if (bl && ecm_pdo_add(t, s, cats[k].dir, pdo, ix, sub, bl)) { rc = -1; break; }
                }
                a += 8;
                c += 4;
            }
            if (++npdo >= EC_MAXEEPDO - 1) break;
        } while (c < len && !rc);
    }
    if (eectl) ecx_eeprom2pdi(ctx, (uint16)s);
    return rc;
}

int ecm_pdo_scan_slave(ecx_contextt *ctx, int slave, ecm_pdo_table_t *t, const char **src)
{
    if (ctx->slavelist[slave].mbx_proto & ECT_MBXPROT_COE) {
        int n0 = t->n;
        uint32_t o0 = t->bits[ECM_PDO_OUT][slave], i0 = t->bits[ECM_PDO_IN][slave];
        if (scan_coe(ctx, slave, t) == 0) { *src = "coe"; return 0; }
        t->n = n0;                         /* undo a partial CoE scan */
        t->bits[ECM_PDO_OUT][slave] = o0;
        t->bits[ECM_PDO_IN][slave] = i0;
    }
    *src = "sii";
    return scan_sii(ctx, slave, t);
}

int ecm_pdo_scan(ecx_contextt *ctx, ecm_pdo_table_t *t)
{
    int bad = 0;
    for (int s = 1; s <= ctx->slavecount && s <= ECM_PDO_MAX_SLAVES; s++) {
        const char *src = "?";
        int n0 = t->n;
        if (ecm_pdo_scan_slave(ctx, s, t, &src)) {
            fprintf(stderr, "ecm_pdo: slave %d: scan failed\n", s);
            bad++;
        } else {
            fprintf(stderr, "ecm_pdo: slave %d: %d entries from %s\n", s, t->n - n0, src);
        }
    }
    return bad;
}

void ecm_pdo_locate(ecx_contextt *ctx, uint8 *const *iomap, int ngroups, ecm_pdo_loc_t *loc)
{
    for (int s = 1; s <= ctx->slavecount && s <= ECM_PDO_MAX_SLAVES; s++) {
        const ec_slavet *sl = &ctx->slavelist[s];
        int g = sl->group < ngroups ? sl->group : 0;
        loc[s].group = (uint8_t)g;
        loc[s].out_bit = sl->outputs ? (uint32_t)(sl->outputs - iomap[g]) * 8u + sl->Ostartbit : 0;
        loc[s].in_bit = sl->inputs ? (uint32_t)(sl->inputs - iomap[g]) * 8u + sl->Istartbit : 0;
    }
}

int ecm_pdo_soem_check(ecx_contextt *ctx, const ecm_pdo_table_t *t)
{
    static uint32_t ob[ECM_PDO_MAX_SLAVES + 1], ib[ECM_PDO_MAX_SLAVES + 1];
    static char err[4096];
    int n = ctx->slavecount < ECM_PDO_MAX_SLAVES ? ctx->slavecount : ECM_PDO_MAX_SLAVES;
    for (int s = 1; s <= n; s++) { ob[s] = ctx->slavelist[s].Obits; ib[s] = ctx->slavelist[s].Ibits; }
    int bad = ecm_pdo_check_sizes(t, n, ob, ib, err, sizeof(err));
    if (bad) fprintf(stderr, "ecm_pdo: table does not match the mapping (%d):\n%s", bad, err);
    return bad;
}
