/*
 * ecm_eni.c -- .enicfg loader and ENI checks (no SOEM dependency).
 * Format: see tools/eni/eni2cfg.py. Loading is strict: an unknown record,
 * a missing field, a position out of order or a value out of range is an
 * error -- a half-understood ENI must never drive a bus.
 */
#include "ecm_eni.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const TRANS_NAMES[] = {
    "IP", "PI", "PS", "SP", "SO", "OS", "SI", "OI", "IB", "BI", "II", "PP", "SS", "OO", "OP",
};
#define NTRANS (sizeof(TRANS_NAMES) / sizeof(TRANS_NAMES[0]))

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* append one line to err, keeping it NUL-terminated */
static void adderr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    size_t used = strlen(err);
    if (used + 1 >= errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + used, errlen - used, fmt, ap);
    va_end(ap);
}

uint16_t ecm_eni_trans_parse(const char *list)
{
    uint16_t mask = 0;
    char buf[64];
    if (!list || strlen(list) >= sizeof(buf))
        return 0;
    strcpy(buf, list);
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        size_t i;
        for (i = 0; i < NTRANS; i++)
            if (strcmp(tok, TRANS_NAMES[i]) == 0)
                break;
        if (i == NTRANS)
            return 0;
        mask |= (uint16_t)(1u << i);
    }
    return mask;
}

const char *ecm_eni_trans_name(uint16_t bit)
{
    for (size_t i = 0; i < NTRANS; i++)
        if (bit == (1u << i))
            return TRANS_NAMES[i];
    return "?";
}

/* ---- tokenizer: whitespace separated, one "quoted string" allowed ---- */
#define MAXTOK 48

static int tokenize(char *line, char **tok)
{
    int n = 0;
    char *p = line;
    while (*p && n < MAXTOK) {
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            tok[n++] = ++p;
            while (*p && *p != '"')
                p++;
            if (*p)
                *p++ = '\0';
            continue;
        }
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = '\0';
    }
    return n;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || *end || end == s || v > 0xFFFFFFFFull)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_i32(const char *s, int32_t *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 0);
    if (errno || *end || end == s || v < INT32_MIN || v > INT32_MAX)
        return -1;
    *out = (int32_t)v;
    return 0;
}

/* value of "key" in a key/value token list starting at tok[start] */
static const char *kv(char **tok, int n, int start, const char *key)
{
    for (int i = start; i + 1 < n; i += 2)
        if (strcmp(tok[i], key) == 0)
            return tok[i + 1];
    return NULL;
}

#define NEED_U32(key, dst, maxv)                                                        \
    do {                                                                                \
        const char *v_ = kv(tok, n, 2, key);                                            \
        uint32_t t_;                                                                    \
        if (!v_ || parse_u32(v_, &t_) || t_ > (uint32_t)(maxv)) {                       \
            seterr(err, errlen, "line %d: %s: bad or missing '%s'", lineno, tok[0], key); \
            return -1;                                                                  \
        }                                                                               \
        (dst) = t_;                                                                     \
    } while (0)

static int parse_slave(char **tok, int n, int lineno, ecm_eni_t *eni, char *err, size_t errlen)
{
    uint32_t pos;
    if (n < 2 || parse_u32(tok[1], &pos)) {
        seterr(err, errlen, "line %d: slave: bad position", lineno);
        return -1;
    }
    if ((int)pos != eni->nslaves + 1 || pos > ECM_ENI_MAX_SLAVES) {
        seterr(err, errlen, "line %d: slave %u out of order (expected %d)", lineno, pos, eni->nslaves + 1);
        return -1;
    }
    ecm_eni_slave_t *s = &eni->slave[pos - 1];
    memset(s, 0, sizeof(*s));
    s->pos = (uint16_t)pos;
    const char *name = kv(tok, n, 2, "name");
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
    uint32_t v;
    NEED_U32("vendor", s->vendor, 0xFFFFFFFFu);
    NEED_U32("product", s->product, 0xFFFFFFFFu);
    NEED_U32("rev", s->rev, 0xFFFFFFFFu);
    NEED_U32("check_rev", v, 1); s->check_rev = (uint8_t)v;
    NEED_U32("addr", v, 0xFFFF); s->addr = (uint16_t)v;
    NEED_U32("osize_bits", s->osize_bits, 0xFFFFFFu);
    NEED_U32("isize_bits", s->isize_bits, 0xFFFFFFu);
    NEED_U32("dc", v, 1); s->dc = (uint8_t)v;
    NEED_U32("refclock", v, 1); s->refclock = (uint8_t)v;
    NEED_U32("sync0_ns", s->sync0_ns, 0xFFFFFFFFu);
    NEED_U32("sync1_ns", s->sync1_ns, 0xFFFFFFFFu);
    NEED_U32("assign", v, 0xFFFF); s->assign = (uint16_t)v;
    const char *sh = kv(tok, n, 2, "shift_ns");
    if (!sh || parse_i32(sh, &s->shift_ns)) {
        seterr(err, errlen, "line %d: slave: bad or missing 'shift_ns'", lineno);
        return -1;
    }
    /* Phase 9.6: optional (enicfg 1 has none) */
    static const char *const st_key[ECM_ENI_ST_COUNT] = { "preop_ms", "safeop_ms", "op_ms" };
    for (int k = 0; k < ECM_ENI_ST_COUNT; k++) {
        const char *sv = kv(tok, n, 2, st_key[k]);
        if (sv && (parse_u32(sv, &s->state_ms[k]) || s->state_ms[k] > 600000)) {
            seterr(err, errlen, "line %d: slave: bad '%s'", lineno, st_key[k]);
            return -1;
        }
    }
    if (s->dc && (s->sync0_ns == 0 || s->assign == 0)) {
        seterr(err, errlen, "line %d: slave %u: dc 1 needs sync0_ns and assign", lineno, pos);
        return -1;
    }
    if (!s->dc && s->refclock) {
        seterr(err, errlen, "line %d: slave %u: refclock without dc", lineno, pos);
        return -1;
    }
    eni->nslaves++;
    return 0;
}

static int hexbytes(const char *s, uint8_t *out, size_t max, uint16_t *len)
{
    if (strcmp(s, "-") == 0) {
        *len = 0;
        return 0;
    }
    size_t n = strlen(s);
    if (n % 2 || n / 2 > max)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        char b[3] = { s[2 * i], s[2 * i + 1], 0 };
        if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1]))
            return -1;
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    *len = (uint16_t)(n / 2);
    return 0;
}

static int parse_reg(char **tok, int n, int lineno, ecm_eni_t *eni, char *err, size_t errlen)
{
    uint32_t pos, v;
    if (n < 2 || parse_u32(tok[1], &pos) || (int)pos > eni->nslaves) {
        seterr(err, errlen, "line %d: reg: position not 0 (master) or a declared slave", lineno);
        return -1;
    }
    if (eni->nreg >= ECM_ENI_MAX_REG) {
        seterr(err, errlen, "line %d: more than %d register InitCmds", lineno, ECM_ENI_MAX_REG);
        return -1;
    }
    ecm_eni_reg_t *r = &eni->reg[eni->nreg];
    memset(r, 0, sizeof(*r));
    r->pos = (uint16_t)pos;
    r->trans = ecm_eni_trans_parse(kv(tok, n, 2, "trans"));
    if (!r->trans) {
        seterr(err, errlen, "line %d: reg: bad 'trans'", lineno);
        return -1;
    }
    NEED_U32("cmd", v, 14); r->cmd = (uint8_t)v;
    NEED_U32("ado", v, 0xFFFF); r->ado = (uint16_t)v;
    NEED_U32("len", v, 0x7FF); r->len = (uint16_t)v;
    const char *d = kv(tok, n, 2, "data");
    uint16_t kept;
    if (!d || hexbytes(d, r->data, sizeof(r->data), &kept)) {
        seterr(err, errlen, "line %d: reg: bad 'data' (hex, max %d byte)", lineno, ECM_ENI_REG_DATA);
        return -1;
    }
    r->nkept = (uint8_t)kept;
    eni->nreg++;
    return 0;
}

static int parse_pdo(char **tok, int n, int lineno, ecm_eni_t *eni, char *err, size_t errlen)
{
    uint32_t pos, v;
    if (n < 2 || parse_u32(tok[1], &pos) || pos < 1 || (int)pos > eni->nslaves) {
        seterr(err, errlen, "line %d: pdo: position not a declared slave", lineno);
        return -1;
    }
    if (eni->npdo >= ECM_ENI_MAX_PDO) {
        seterr(err, errlen, "line %d: more than %d PDO entries", lineno, ECM_ENI_MAX_PDO);
        return -1;
    }
    ecm_eni_pdo_t *p = &eni->pdo[eni->npdo];
    memset(p, 0, sizeof(*p));
    p->pos = (uint16_t)pos;
    const char *d = kv(tok, n, 2, "dir");
    if (!d || (strcmp(d, "out") && strcmp(d, "in"))) {
        seterr(err, errlen, "line %d: pdo: bad 'dir'", lineno);
        return -1;
    }
    p->dir = (uint8_t)(strcmp(d, "in") == 0);
    NEED_U32("pdo", v, 0xFFFF); p->pdo = (uint16_t)v;
    NEED_U32("index", v, 0xFFFF); p->index = (uint16_t)v;
    NEED_U32("sub", v, 0xFF); p->sub = (uint8_t)v;
    NEED_U32("bits", v, 0xFFFF); p->bits = (uint16_t)v;
    if (!p->bits) {
        seterr(err, errlen, "line %d: pdo: bits 0", lineno);
        return -1;
    }
    eni->npdo++;
    return 0;
}

static int parse_coe(char **tok, int n, int lineno, ecm_eni_t *eni, char *err, size_t errlen)
{
    uint32_t pos, v;
    if (n < 2 || parse_u32(tok[1], &pos) || pos < 1 || (int)pos > eni->nslaves) {
        seterr(err, errlen, "line %d: coe: position not a declared slave", lineno);
        return -1;
    }
    if (eni->ncoe >= ECM_ENI_MAX_COE) {
        seterr(err, errlen, "line %d: more than %d CoE InitCmds", lineno, ECM_ENI_MAX_COE);
        return -1;
    }
    ecm_eni_coe_t *c = &eni->coe[eni->ncoe];
    memset(c, 0, sizeof(*c));
    c->pos = (uint16_t)pos;
    c->trans = ecm_eni_trans_parse(kv(tok, n, 2, "trans"));
    if (!c->trans) {
        seterr(err, errlen, "line %d: coe: bad 'trans'", lineno);
        return -1;
    }
    NEED_U32("ccs", v, 2); c->ccs = (uint8_t)v;
    if (c->ccs != ECM_ENI_CCS_DOWNLOAD && c->ccs != ECM_ENI_CCS_UPLOAD) {
        seterr(err, errlen, "line %d: coe: ccs %u unsupported", lineno, v);
        return -1;
    }
    NEED_U32("index", v, 0xFFFF); c->index = (uint16_t)v;
    NEED_U32("sub", v, 0xFF); c->sub = (uint8_t)v;
    NEED_U32("ca", v, 1); c->ca = (uint8_t)v;
    NEED_U32("timeout_ms", c->timeout_ms, 600000);
    const char *d = kv(tok, n, 2, "data");
    if (!d || hexbytes(d, c->data, sizeof(c->data), &c->len)) {
        seterr(err, errlen, "line %d: coe: bad 'data' (hex, max %d byte)", lineno, ECM_ENI_MAX_DATA);
        return -1;
    }
    if (c->ccs == ECM_ENI_CCS_DOWNLOAD && c->len == 0) {
        seterr(err, errlen, "line %d: coe: download without data", lineno);
        return -1;
    }
    eni->ncoe++;
    return 0;
}

int ecm_eni_parse_text(const char *text, ecm_eni_t *eni, char *err, size_t errlen)
{
    memset(eni, 0, sizeof(*eni));
    if (err && errlen)
        err[0] = '\0';
    int declared = -1, have_version = 0, lineno = 0;
    const char *p = text;
    char line[1024];

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        lineno++;
        if (len >= sizeof(line)) {
            seterr(err, errlen, "line %d: too long", lineno);
            return -1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p += len + (eol ? 1 : 0);

        char *tok[MAXTOK];
        int n = tokenize(line, tok);
        if (n == 0 || tok[0][0] == '#')
            continue;

        if (strcmp(tok[0], "enicfg") == 0) {
            if (n != 2 || (strcmp(tok[1], "1") != 0 && strcmp(tok[1], "2") != 0)) {
                seterr(err, errlen, "line %d: unsupported enicfg version", lineno);
                return -1;
            }
            eni->version = tok[1][0] - '0';
            have_version = 1;
        } else if (!have_version) {
            seterr(err, errlen, "line %d: missing 'enicfg 1' header", lineno);
            return -1;
        } else if (strcmp(tok[0], "source") == 0 && n >= 2) {
            snprintf(eni->source, sizeof(eni->source), "%s", tok[1]);
        } else if (strcmp(tok[0], "cycle_us") == 0 && n == 2) {
            if (parse_u32(tok[1], &eni->cycle_us)) {
                seterr(err, errlen, "line %d: bad cycle_us", lineno);
                return -1;
            }
        } else if (strcmp(tok[0], "slaves") == 0 && n == 2) {
            uint32_t v;
            if (parse_u32(tok[1], &v) || v == 0 || v > ECM_ENI_MAX_SLAVES) {
                seterr(err, errlen, "line %d: bad slave count", lineno);
                return -1;
            }
            declared = (int)v;
        } else if (strcmp(tok[0], "slave") == 0) {
            if (parse_slave(tok, n, lineno, eni, err, errlen))
                return -1;
        } else if (strcmp(tok[0], "coe") == 0) {
            if (parse_coe(tok, n, lineno, eni, err, errlen))
                return -1;
        } else if (strcmp(tok[0], "pdo") == 0 && eni->version >= 2) {
            if (parse_pdo(tok, n, lineno, eni, err, errlen))
                return -1;
        } else if (strcmp(tok[0], "reg") == 0 && eni->version >= 2) {
            if (parse_reg(tok, n, lineno, eni, err, errlen))
                return -1;
        } else if (strcmp(tok[0], "pd_cmd") == 0 && eni->version >= 2 && n == 2) {
            if (strcmp(tok[1], "lrw") == 0)          eni->pd_cmd = ECM_ENI_PD_LRW;
            else if (strcmp(tok[1], "lrd_lwr") == 0) eni->pd_cmd = ECM_ENI_PD_LRD_LWR;
            else if (strcmp(tok[1], "none") == 0)    eni->pd_cmd = ECM_ENI_PD_NONE;
            else {
                seterr(err, errlen, "line %d: bad pd_cmd '%s'", lineno, tok[1]);
                return -1;
            }
        } else {
            seterr(err, errlen, "line %d: unknown record '%s'", lineno, tok[0]);
            return -1;
        }
    }

    if (!have_version) {
        seterr(err, errlen, "empty file or missing 'enicfg 1' header");
        return -1;
    }
    if (eni->version >= 2 && eni->pd_cmd == ECM_ENI_PD_UNKNOWN) {
        seterr(err, errlen, "enicfg 2 without a 'pd_cmd' record");
        return -1;
    }
    /* Phase 9.10: PDO entries, when present, must add up to the slave sizes */
    if (eni->npdo) {
        for (int i = 0; i < eni->nslaves; i++) {
            uint32_t o = 0, in = 0;
            for (int k = 0; k < eni->npdo; k++)
                if (eni->pdo[k].pos == i + 1) {
                    if (eni->pdo[k].dir) in += eni->pdo[k].bits; else o += eni->pdo[k].bits;
                }
            if (o != eni->slave[i].osize_bits || in != eni->slave[i].isize_bits) {
                seterr(err, errlen, "slave %d: pdo entries %u out / %u in bits, slave record %u / %u",
                       i + 1, o, in, eni->slave[i].osize_bits, eni->slave[i].isize_bits);
                return -1;
            }
        }
    }
    if (declared != eni->nslaves) {
        seterr(err, errlen, "'slaves %d' but %d slave records", declared, eni->nslaves);
        return -1;
    }
    int refs = 0, dcs = 0;
    for (int i = 0; i < eni->nslaves; i++) {
        refs += eni->slave[i].refclock;
        dcs += eni->slave[i].dc;
    }
    if (refs > 1 || (dcs && refs != 1)) {
        seterr(err, errlen, "%d DC slaves but %d reference clocks", dcs, refs);
        return -1;
    }
    return 0;
}

int ecm_eni_load(const char *path, ecm_eni_t *eni, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        seterr(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    size_t cap = 1 << 20;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        seterr(err, errlen, "out of memory");
        return -1;
    }
    size_t len = fread(buf, 1, cap - 1, f);
    int too_big = !feof(f);
    fclose(f);
    if (too_big) {
        free(buf);
        seterr(err, errlen, "%s: larger than 1 MiB", path);
        return -1;
    }
    buf[len] = '\0';
    int rc = ecm_eni_parse_text(buf, eni, err, errlen);
    free(buf);
    return rc;
}

int ecm_eni_check_identity(const ecm_eni_t *eni, int nfound,
                           const ecm_eni_found_t *found, char *err, size_t errlen)
{
    int bad = 0;
    if (err && errlen)
        err[0] = '\0';
    if (nfound != eni->nslaves) {
        adderr(err, errlen, "slave count: ENI %d, bus %d\n", eni->nslaves, nfound);
        bad++;
    }
    int n = nfound < eni->nslaves ? nfound : eni->nslaves;
    for (int i = 0; i < n; i++) {
        const ecm_eni_slave_t *e = &eni->slave[i];
        const ecm_eni_found_t *f = &found[i];
        if (f->vendor != e->vendor || f->product != e->product ||
            (e->check_rev && f->rev != e->rev)) {
            adderr(err, errlen,
                   "position %d (%s): expected vendor 0x%X product 0x%X rev 0x%X%s, found 0x%X 0x%X 0x%X\n",
                   i + 1, e->name, e->vendor, e->product, e->rev, e->check_rev ? "" : " (rev not checked)",
                   f->vendor, f->product, f->rev);
            bad++;
        }
    }
    return bad;
}

int ecm_eni_check_layout(const ecm_eni_t *eni, int nfound,
                         const ecm_eni_found_t *found, char *err, size_t errlen)
{
    int bad = 0;
    if (err && errlen)
        err[0] = '\0';
    int n = nfound < eni->nslaves ? nfound : eni->nslaves;
    if (nfound != eni->nslaves) {
        adderr(err, errlen, "slave count: ENI %d, bus %d\n", eni->nslaves, nfound);
        bad++;
    }
    for (int i = 0; i < n; i++) {
        const ecm_eni_slave_t *e = &eni->slave[i];
        if (found[i].obits != e->osize_bits || found[i].ibits != e->isize_bits) {
            adderr(err, errlen, "position %d (%s): expected %u out / %u in bits, mapped %u / %u\n",
                   i + 1, e->name, e->osize_bits, e->isize_bits, found[i].obits, found[i].ibits);
            bad++;
        }
    }
    return bad;
}

int ecm_eni_refclock(const ecm_eni_t *eni)
{
    for (int i = 0; i < eni->nslaves; i++)
        if (eni->slave[i].refclock)
            return i + 1;
    return 0;
}

/* ---------------------------------------------------------------- Phase 9.6 */

const char *ecm_eni_cmd_name(uint8_t cmd)
{
    static const char *const names[] = { "NOP", "APRD", "APWR", "APRW", "FPRD", "FPWR", "FPRW",
                                         "BRD", "BWR", "BRW", "LRD", "LWR", "LRW", "ARMW", "FRMW" };
    return cmd < sizeof(names) / sizeof(names[0]) ? names[cmd] : "?";
}

#define K_RD 1u   /* APRD FPRD BRD */
#define K_WR 2u   /* APWR FPWR BWR */

static unsigned cmd_kind(uint8_t cmd)
{
    switch (cmd) {
    case 1: case 4: case 7: return K_RD;
    case 2: case 5: case 8: return K_WR;
    default: return 0;   /* RW, logical, ARMW/FRMW: never part of a slave configuration */
    }
}

/* Register InitCmds that SOEM (ecx_config_init / ecx_config_map_group /
 * ecx_configdc) and libecmaster (ecm_run state machine, DC SYNC0 setup)
 * already do -- the table of kehoach §8 item 1. Seeded from the audit of
 * the Phase 8 ENIs (giai_doan_8_nhat_ky_8_3.md §3) and of every TwinCAT ENI of
 * step 9.4 (config/eni/known_regs.txt, docs/eni_audit_9_4*.md): all of
 * them use only these. Deliberately NOT here (an ENI with them is refused
 * until someone decides): 0x0400-0x0420 watchdogs (ecm_run sets 0x0420
 * itself, an ENI value would be ignored), 0x0982 pulse length (an ESI
 * value, SOEM never writes it), EEPROM writes, anything vendor specific.
 * The DC writes (0x0980, 0x09A0, 0x09A4) are known because ecm_run does
 * them from the slave record; ecm_eni_check_supported() also checks that
 * their data says the same as that record (Phase 9.5). */
static const struct {
    uint16_t ado;
    unsigned kinds;
    uint8_t  zero_only;   /* only an all-zero write is "known" */
    const char *why;
} KNOWN_REGS[] = {
    { 0x0010, K_RD | K_WR, 0, "station address: SOEM ecx_config_init" },
    { 0x0101, K_WR, 0, "DL control: SOEM ecx_config_init" },
    { 0x0103, K_WR, 0, "DL control (loop): SOEM ecx_config_init" },
    { 0x0120, K_WR, 0, "AL control: ecm_run state machine" },
    { 0x0130, K_RD, 0, "AL status: ecm_run state machine" },
    { 0x0200, K_WR, 0, "ECAT event mask: SOEM ecx_config_init" },
    { 0x0300, K_WR, 0, "RX error counters: SOEM ecx_config_init" },
    { 0x0500, K_WR, 0, "SII ownership: SOEM SII access" },
    { 0x0502, K_RD | K_WR, 0, "SII control: SOEM SII access (identity check E-03)" },
    { 0x0508, K_RD, 0, "SII data: SOEM SII access (identity check E-03)" },
    { 0x0600, K_WR, 0, "FMMU 0: SOEM ecx_config_map_group" },
    { 0x0610, K_WR, 0, "FMMU 1: SOEM ecx_config_map_group" },
    { 0x0620, K_WR, 0, "FMMU 2: SOEM ecx_config_map_group" },
    { 0x0630, K_WR, 0, "FMMU 3: SOEM ecx_config_map_group" },
    { 0x0800, K_RD | K_WR, 0, "SM 0: SOEM (SII / CoE)" },
    { 0x0808, K_WR, 0, "SM 1: SOEM (SII / CoE)" },
    { 0x0810, K_WR, 0, "SM 2: SOEM (SII / CoE); size checked by the layout check" },
    { 0x0818, K_WR, 0, "SM 3: SOEM (SII / CoE); size checked by the layout check" },
    { 0x0820, K_WR, 0, "SM 4: SOEM (SII / CoE)" },
    { 0x0828, K_WR, 0, "SM 5: SOEM (SII / CoE)" },
    { 0x0910, K_WR, 0, "DC system time: SOEM ecx_configdc" },
    { 0x0930, K_WR, 0, "DC speed counter start: SOEM ecx_configdc" },
    { 0x0934, K_WR, 0, "DC filter: SOEM ecx_configdc" },
    { 0x0980, K_WR, 0, "DC AssignActivate: ecm_run from the ENI (assign)" },
    { 0x0981, K_WR, 0, "DC activation: ecm_run DC setup" },
    { 0x0990, K_WR, 0, "DC start time: ecm_run DC setup" },
    { 0x09A0, K_WR, 0, "SYNC0 (+ SYNC1) cycle: ecm_run from the ENI (sync0_ns, sync1_ns)" },
    { 0x09A4, K_WR, 0, "SYNC1 cycle: ecm_run from the ENI (sync1_ns, Phase 9.5)" },
    { 0x09A8, K_WR, 1, "latch control left at reset (0): latch not used by ecm_run" },
};

int ecm_eni_reg_known(const ecm_eni_reg_t *r, const char **why)
{
    unsigned kind = cmd_kind(r->cmd);
    for (size_t i = 0; i < sizeof(KNOWN_REGS) / sizeof(KNOWN_REGS[0]); i++) {
        if (KNOWN_REGS[i].ado != r->ado)
            continue;
        if (!(kind & KNOWN_REGS[i].kinds)) {
            if (why) *why = "register known, but not with this command";
            return 0;
        }
        if (KNOWN_REGS[i].zero_only && kind == K_WR) {
            for (int b = 0; b < r->nkept; b++)
                if (r->data[b]) {
                    if (why) *why = "non-zero write; ecm_run only supports the reset value";
                    return 0;
                }
        }
        if (why) *why = KNOWN_REGS[i].why;
        return 1;
    }
    if (why) *why = "not done by SOEM/ecm_run";
    return 0;
}

static void trans_list(uint16_t mask, char *out, size_t outlen)
{
    out[0] = '\0';
    for (uint16_t b = 1; b; b <<= 1)
        if (mask & b) {
            size_t u = strlen(out);
            snprintf(out + u, outlen - u, "%s%s", u ? "," : "", ecm_eni_trans_name(b));
        }
}

int ecm_eni_check_supported(const ecm_eni_t *eni, int allow_unknown_reg, int *nwarn,
                            char *err, size_t errlen)
{
    int bad = 0, warn = 0;
    if (err && errlen)
        err[0] = '\0';
    if (eni->version < 2) {
        adderr(err, errlen, "warning: enicfg %d has no register InitCmds / process data command: "
               "not checked (regenerate with tools/eni/eni2cfg.py)\n", eni->version);
        warn++;
    }
    if (eni->pd_cmd == ECM_ENI_PD_LRD_LWR) {
        adderr(err, errlen, "process data by LRD/LWR not supported, use LRW (SOEM maps one LRW; "
               "TwinCAT: untick 'Use RD/WR instead of RW' in every Box)\n");
        bad++;
    }
    for (int i = 0; i < eni->nreg; i++) {
        const ecm_eni_reg_t *r = &eni->reg[i];
        const char *why;
        if (ecm_eni_reg_known(r, &why))
            continue;
        char tl[96], hex[2 * ECM_ENI_REG_DATA + 4];
        trans_list(r->trans, tl, sizeof(tl));
        hex[0] = '\0';
        for (int b = 0; b < r->nkept; b++)
            snprintf(hex + 2 * b, sizeof(hex) - 2 * (size_t)b, "%02x", r->data[b]);
        if (r->len > r->nkept)
            strcat(hex, "..");
        char who[24];
        if (r->pos)
            snprintf(who, sizeof(who), "slave %u", r->pos);
        else
            snprintf(who, sizeof(who), "master");
        adderr(err, errlen, "%s%s register InitCmd %s %s 0x%04X len %u data %s: %s\n",
               allow_unknown_reg ? "warning: " : "", who, tl, ecm_eni_cmd_name(r->cmd), r->ado,
               r->len, hex[0] ? hex : "-", why);
        if (allow_unknown_reg)
            warn++;
        else
            bad++;
    }
    /* Phase 9.5: the DC register InitCmds must agree with the slave record that
     * ecm_run applies (assign, sync0_ns, sync1_ns). They come from different
     * parts of the ENI (InitCmds vs the DC element); a hand-edited ENI can
     * make them disagree, and then the ENI says two things. */
    for (int i = 0; i < eni->nreg; i++) {
        const ecm_eni_reg_t *r = &eni->reg[i];
        if (!r->pos || !(r->trans & ECM_ENI_T_PS) || cmd_kind(r->cmd) != K_WR)
            continue;
        const ecm_eni_slave_t *s = &eni->slave[r->pos - 1];
        if (!s->dc)
            continue;
        uint32_t want[3], got[3];
        int nchk = 0;
        const char *what = NULL;
        if (r->ado == 0x0980 && r->nkept >= 2) {
            want[0] = s->assign; got[0] = (uint32_t)(r->data[0] | r->data[1] << 8); nchk = 1;
            what = "AssignActivate";
        } else if (r->ado == 0x09A0 && r->nkept >= 4) {
            want[0] = s->sync0_ns;
            got[0] = (uint32_t)r->data[0] | (uint32_t)r->data[1] << 8 | (uint32_t)r->data[2] << 16 |
                     (uint32_t)r->data[3] << 24;
            nchk = 1;
            if (r->nkept >= 8) {
                want[1] = s->sync1_ns;
                got[1] = (uint32_t)r->data[4] | (uint32_t)r->data[5] << 8 | (uint32_t)r->data[6] << 16 |
                         (uint32_t)r->data[7] << 24;
                nchk = 2;
            }
            what = "SYNC0/SYNC1 cycle";
        } else if (r->ado == 0x09A4 && r->nkept >= 4) {
            want[0] = s->sync1_ns;
            got[0] = (uint32_t)r->data[0] | (uint32_t)r->data[1] << 8 | (uint32_t)r->data[2] << 16 |
                     (uint32_t)r->data[3] << 24;
            nchk = 1;
            what = "SYNC1 cycle";
        }
        for (int k = 0; k < nchk; k++)
            if (want[k] != got[k]) {
                adderr(err, errlen, "slave %u: DC register InitCmd 0x%04X (%s) writes %u, the DC element says %u\n",
                       r->pos, r->ado + 4 * k, what, got[k], want[k]);
                bad++;
            }
    }
    if (nwarn)
        *nwarn = warn;
    return bad;
}

uint32_t ecm_eni_state_timeout_ms(const ecm_eni_t *eni, int st)
{
    uint32_t m = 0;
    if (st < 0 || st >= ECM_ENI_ST_COUNT)
        return 0;
    for (int i = 0; i < eni->nslaves; i++)
        if (eni->slave[i].state_ms[st] > m)
            m = eni->slave[i].state_ms[st];
    return m;
}
