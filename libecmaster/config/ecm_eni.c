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
    "IP", "PI", "PS", "SP", "SO", "OS", "SI", "OI", "IB", "BI", "II", "PP", "SS", "OO",
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
            if (n != 2 || strcmp(tok[1], "1") != 0) {
                seterr(err, errlen, "line %d: unsupported enicfg version", lineno);
                return -1;
            }
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
        } else {
            seterr(err, errlen, "line %d: unknown record '%s'", lineno, tok[0]);
            return -1;
        }
    }

    if (!have_version) {
        seterr(err, errlen, "empty file or missing 'enicfg 1' header");
        return -1;
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
