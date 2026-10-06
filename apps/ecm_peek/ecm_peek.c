/* ==========================================================================
 * ecm_peek — READ-ONLY look at a bus before the master ever runs it
 * (Phase 10.0 bring-up of the IS620N drives, R-02 .. R-04).
 *
 *   ecm_peek --iface IF [--sii-dir DIR] [--sdo IDX:SUB,...] [--no-default-sdo]
 *
 * What it does, in this order:
 *   1. counts the slaves (BRD), dumps each slave's whole SII EEPROM to
 *      DIR/sii_<n>.bin (same access as SOEM's eepromtool -r: EEPROM taken
 *      from the PDI for the read only, given back by ecx_config_init);
 *   2. ecx_config_init(): station addresses, slaves requested to PRE-OP --
 *      NOT further. No PDO mapping, no SAFE-OP, no process data, so a drive
 *      cannot be commanded by this tool;
 *   3. reads a fixed list of CoE objects by SDO upload (identity, PDO
 *      assignment and mapping, sync manager parameters, CiA402 objects of
 *      axis 0) plus --sdo extras, and prints every value or abort code.
 *
 * It never writes an SDO, never writes the EEPROM, never requests a state
 * above PRE-OP. The IS620N ESI says SdoInfo = 0: the object dictionary
 * cannot be listed (slaveinfo -sdo prints nothing), hence the fixed list.
 *
 * Output lines (stdout, one fact per line, parsed by tools/gd10/bringup_report.py):
 *   SLAVES n
 *   SLAVE s name "..." man 0x.. id 0x.. rev 0x.. serial 0x.. state 0x.. al 0x....
 *         hasdc d pdelay ns ports a.b.c.d parent p cfgaddr 0x.... mbx_proto 0x..
 *   SII s file PATH bytes N
 *   SDO s 0xIIII:SS size N = 0x...            (value, little-endian, <= 8 byte)
 *   SDO s 0xIIII:SS size N = "text"           (printable string)
 *   SDO s 0xIIII:SS size N = hex aa bb ..     (longer)
 *   SDO s 0xIIII:SS ABORT 0xAAAAAAAA          (SDO abort code)
 *   SDO s 0xIIII:SS NOREPLY                   (timeout, no abort)
 *
 * Exit: 0 ok, 1 no slave / PRE-OP not reached, 2 usage, 4 socket.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "soem/soem.h"

static ecx_contextt ctx;

/* Objects read on every slave. sub < 0: "array" -- read sub 0 (count)
 * then 1..count (capped at 64). Unknown objects just abort: that is data. */
typedef struct { uint16_t idx; int sub; } obj_t;
static const obj_t DEFAULT_OBJS[] = {
    { 0x1000, 0 }, { 0x1008, 0 }, { 0x1009, 0 }, { 0x100A, 0 },
    { 0x1018, -1 }, { 0x10F1, -1 },
    { 0x1C00, -1 }, { 0x1C12, -1 }, { 0x1C13, -1 },
    { 0x1600, -1 }, { 0x1701, -1 }, { 0x1702, -1 }, { 0x1703, -1 }, { 0x1704, -1 }, { 0x1705, -1 },
    { 0x1A00, -1 }, { 0x1B01, -1 }, { 0x1B02, -1 }, { 0x1B03, -1 }, { 0x1B04, -1 },
    { 0x1C32, -1 }, { 0x1C33, -1 },
    /* CiA402, axis 0: state, mode, limits the 10.9 checklist asks for */
    { 0x603F, 0 }, { 0x6041, 0 }, { 0x6060, 0 }, { 0x6061, 0 }, { 0x6064, 0 },
    { 0x6502, 0 }, { 0x6098, 0 }, { 0x607C, 0 }, { 0x6099, -1 }, { 0x609A, 0 },
    { 0x6065, 0 }, { 0x6066, 0 }, { 0x6072, 0 }, { 0x607D, -1 }, { 0x607F, 0 }, { 0x6080, 0 },
    { 0x6081, 0 }, { 0x6083, 0 }, { 0x6084, 0 }, { 0x6085, 0 }, { 0x6091, -1 }, { 0x608F, -1 },
    { 0x605A, 0 }, { 0x605B, 0 }, { 0x605C, 0 }, { 0x605D, 0 }, { 0x605E, 0 }, { 0x6007, 0 },
};

#define MAX_EXTRA 64
static obj_t g_extra[MAX_EXTRA];
static int g_nextra;

static void usage(void)
{
    fprintf(stderr, "usage: ecm_peek --iface IF [--sii-dir DIR] [--sdo 0xIIII:SUB[,...]] [--no-default-sdo]\n"
                    "       read-only: SII dump, PRE-OP, SDO uploads. Never writes, never goes above PRE-OP.\n");
}

static int parse_sdo_list(const char *spec)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        unsigned idx; int sub;
        if (g_nextra >= MAX_EXTRA) return -1;
        if (sscanf(t, "%x:%d", &idx, &sub) == 2) g_extra[g_nextra++] = (obj_t){ (uint16_t)idx, sub };
        else if (sscanf(t, "%x", &idx) == 1) g_extra[g_nextra++] = (obj_t){ (uint16_t)idx, -1 };
        else return -1;
    }
    return 0;
}

/* --------------------------------------------------------------- SII dump */
static int sii_dump(int slave, const char *dir)
{
    uint16 aiadr = (uint16)(1 - slave);
    uint8 eepctl = 2;
    ecx_APWR(&ctx.port, aiadr, ECT_REG_EEPCFG, sizeof(eepctl), &eepctl, EC_TIMEOUTRET);   /* force from PDI */
    eepctl = 0;
    ecx_APWR(&ctx.port, aiadr, ECT_REG_EEPCFG, sizeof(eepctl), &eepctl, EC_TIMEOUTRET);   /* to master */
    uint16 estat = 0;
    ecx_APRD(&ctx.port, aiadr, ECT_REG_EEPSTAT, sizeof(estat), &estat, EC_TIMEOUTRET);
    estat = etohs(estat);
    const int ainc = (estat & EC_ESTAT_R64) ? 8 : 4;

    static uint8 ebuf[65536];
    int have = 0;                                       /* bytes read so far */
#define EEP_READ_TO(end_) do { \
        int e_ = (end_) > (int)sizeof(ebuf) ? (int)sizeof(ebuf) : (end_); \
        for (; have < e_; have += ainc) { \
            uint64 v_ = ecx_readeepromAP(&ctx, aiadr, (uint16)(have >> 1), EC_TIMEOUTEEP); \
            for (int b_ = 0; b_ < ainc; b_++) ebuf[have + b_] = (uint8)(v_ >> (8 * b_)); \
        } } while (0)
    EEP_READ_TO(128);
    /* Size: the larger of word 0x3E (EEPROM size in KiBit - 1) and the end
     * of the category list (from word 0x40, ends with type 0xFFFF). A
     * slave may declare less than it carries (soft_bus: 0x3E = 0). */
    int size = ((ebuf[0x7C] | ebuf[0x7D] << 8) + 1) * 128;
    int pos = 128;
    while (pos + 4 <= (int)sizeof(ebuf)) {
        EEP_READ_TO(pos + 4);
        int type = ebuf[pos] | ebuf[pos + 1] << 8, words = ebuf[pos + 2] | ebuf[pos + 3] << 8;
        if (type == 0xFFFF) { pos += 2; break; }
        pos += 4 + 2 * words;
    }
    if (pos > size) size = pos;
    if (size > (int)sizeof(ebuf)) size = sizeof(ebuf);
    EEP_READ_TO(size);
#undef EEP_READ_TO
    char path[512];
    snprintf(path, sizeof(path), "%s/sii_%d.bin", dir, slave);
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); return -1; }
    fwrite(ebuf, 1, (size_t)size, fp);
    fclose(fp);
    printf("SII %d file %s bytes %d\n", slave, path, size);
    return 0;
}

/* ------------------------------------------------------------ SDO uploads */
static int sdo_one(int slave, uint16_t idx, uint8_t sub, uint8_t *out, int *outsz)
{
    uint8 buf[256];
    int sz = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    int wkc = ecx_SDOread(&ctx, (uint16)slave, idx, sub, FALSE, &sz, buf, EC_TIMEOUTRXM);
    uint32_t abort_code = 0;
    ec_errort er;
    while (ecx_poperror(&ctx, &er))
        if (er.Etype == EC_ERR_TYPE_SDO_ERROR) abort_code = (uint32_t)er.AbortCode;
    if (wkc > 0) {
        printf("SDO %d 0x%04X:%02X size %d = ", slave, idx, sub, sz);
        int printable = sz > 1;
        for (int i = 0; i < sz; i++)
            if (!(isprint(buf[i]) || (buf[i] == 0 && i == sz - 1))) { printable = 0; break; }
        if (printable && sz > 4) {
            printf("\"");
            for (int i = 0; i < sz && buf[i]; i++) putchar(buf[i]);
            printf("\"\n");
        } else if (sz <= 8) {
            uint64_t v = 0;
            for (int i = sz - 1; i >= 0; i--) v = v << 8 | buf[i];
            printf("0x%0*llX\n", 2 * sz, (unsigned long long)v);
        } else {
            printf("hex");
            for (int i = 0; i < sz; i++) printf(" %02x", buf[i]);
            printf("\n");
        }
        if (out) { memcpy(out, buf, (size_t)(sz < 8 ? sz : 8)); *outsz = sz; }
        return 1;
    }
    if (abort_code) printf("SDO %d 0x%04X:%02X ABORT 0x%08X\n", slave, idx, sub, abort_code);
    else            printf("SDO %d 0x%04X:%02X NOREPLY\n", slave, idx, sub);
    return 0;
}

static void sdo_obj(int slave, obj_t o)
{
    if (o.sub >= 0) { sdo_one(slave, o.idx, (uint8_t)o.sub, NULL, NULL); return; }
    uint8_t v[8] = { 0 };
    int sz = 0;
    if (!sdo_one(slave, o.idx, 0, v, &sz)) return;
    int n = v[0];
    if (n > 64) n = 64;
    for (int s = 1; s <= n; s++) sdo_one(slave, o.idx, (uint8_t)s, NULL, NULL);
}

int main(int argc, char **argv)
{
    const char *iface = NULL, *sii_dir = NULL;
    int defaults = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--iface") && i + 1 < argc) iface = argv[++i];
        else if (!strcmp(argv[i], "--sii-dir") && i + 1 < argc) sii_dir = argv[++i];
        else if (!strcmp(argv[i], "--sdo") && i + 1 < argc) {
            if (parse_sdo_list(argv[++i])) { fprintf(stderr, "ecm_peek: bad --sdo list\n"); return 2; }
        } else if (!strcmp(argv[i], "--no-default-sdo")) defaults = 0;
        else { usage(); return 2; }
    }
    if (!iface) { usage(); return 2; }
    if (!ecx_init(&ctx, iface)) {
        fprintf(stderr, "ecm_peek: ecx_init(%s) failed (root/capabilities? interface name?)\n", iface);
        return 4;
    }

    uint16 w = 0;
    int n = ecx_BRD(&ctx.port, 0x0000, ECT_REG_TYPE, sizeof(w), &w, EC_TIMEOUTSAFE);
    if (n <= 0) { printf("SLAVES 0\n"); fprintf(stderr, "ecm_peek: no slave answered\n"); ecx_close(&ctx); return 1; }
    if (sii_dir) {
        mkdir(sii_dir, 0755);
        ctx.slavecount = n;
        for (int s = 1; s <= n; s++) sii_dump(s, sii_dir);
    }

    int wc = ecx_config_init(&ctx);           /* station addresses + PRE-OP request; no mapping */
    printf("SLAVES %d\n", wc);
    if (wc <= 0) { ecx_close(&ctx); return 1; }
    /* ESI PreopTimeout of the IS620N is 3 s; give it 2x */
    int st = ecx_statecheck(&ctx, 0, EC_STATE_PRE_OP, 6 * 1000 * 1000);
    ecx_readstate(&ctx);
    for (int s = 1; s <= ctx.slavecount; s++) {
        const ec_slavet *sl = &ctx.slavelist[s];
        printf("SLAVE %d name \"%s\" man 0x%08X id 0x%08X rev 0x%08X serial 0x%08X state 0x%02X al 0x%04X "
               "hasdc %d pdelay %d ports %d.%d.%d.%d parent %d cfgaddr 0x%04X mbx_proto 0x%02X\n",
               s, sl->name, (unsigned)sl->eep_man, (unsigned)sl->eep_id, (unsigned)sl->eep_rev,
               (unsigned)sl->eep_ser, sl->state, sl->ALstatuscode, sl->hasdc, (int)sl->pdelay,
               (sl->activeports & 1) > 0, (sl->activeports & 2) > 0, (sl->activeports & 4) > 0,
               (sl->activeports & 8) > 0, sl->parent, sl->configadr, sl->mbx_proto);
    }
    if (st != EC_STATE_PRE_OP) {
        fprintf(stderr, "ecm_peek: not all slaves in PRE-OP (read 0x%02X) -- no SDO reads\n", st);
        ecx_close(&ctx); return 1;
    }
    for (int s = 1; s <= ctx.slavecount; s++) {
        if (!(ctx.slavelist[s].mbx_proto & ECT_MBXPROT_COE)) { printf("SDO %d none (no CoE)\n", s); continue; }
        if (defaults)
            for (size_t k = 0; k < sizeof(DEFAULT_OBJS) / sizeof(DEFAULT_OBJS[0]); k++) sdo_obj(s, DEFAULT_OBJS[k]);
        for (int k = 0; k < g_nextra; k++) sdo_obj(s, g_extra[k]);
    }
    fflush(stdout);
    ecx_close(&ctx);
    return 0;
}
