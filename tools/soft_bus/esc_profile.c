/* ==========================================================================
 * esc_profile.c — Phase 9.9 soft_bus node profiles (see esc_profile.h).
 * ========================================================================== */
#include "esc_profile.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esc_sii.h"

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int u32(const char *s, uint32_t *v)
{
    if (!s) return -1;
    char *end;
    errno = 0;
    unsigned long long x = strtoull(s, &end, 0);
    if (errno || end == s || *end || x > 0xFFFFFFFFull) return -1;
    *v = (uint32_t)x;
    return 0;
}

/* value of "key" in tok[start..] (key value pairs) */
static const char *kv(char **tok, int n, int start, const char *key)
{
    for (int i = start; i + 1 < n; i++)
        if (!strcmp(tok[i], key)) return tok[i + 1];
    return NULL;
}

#define MAXTOK 32
static int tokenize(char *line, char **tok)
{
    int n = 0;
    char *p = line;
    while (*p && n < MAXTOK) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"') {
            tok[n++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
            continue;
        }
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

static int hex_bytes(const char *s, uint8_t *out, size_t max, uint8_t *len)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        char b[3] = { s[2 * i], s[2 * i + 1], 0 };
        if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return -1;
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    *len = (uint8_t)(n / 2);
    return 0;
}

#define NEED(key, var)                                                            \
    do {                                                                          \
        uint32_t v_;                                                              \
        if (u32(kv(tok, n, 1, key), &v_)) {                                       \
            seterr(err, errlen, "%s:%d: %s: bad or missing '%s'", path, ln, tok[0], key); \
            goto fail;                                                            \
        }                                                                         \
        (var) = v_;                                                               \
    } while (0)

esc_profile_t *esc_prof_load(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) { seterr(err, errlen, "%s: %s", path, strerror(errno)); return NULL; }
    esc_profile_t *p = calloc(1, sizeof(*p));
    int cap_obj = 256, cap_sub = 2048;
    if (p) {
        p->obj = calloc((size_t)cap_obj, sizeof(*p->obj));
        p->sub = calloc((size_t)cap_sub, sizeof(*p->sub));
    }
    if (!p || !p->obj || !p->sub) { seterr(err, errlen, "out of memory"); fclose(f); esc_prof_free(p); return NULL; }
    snprintf(p->path, sizeof(p->path), "%s", path);

    char line[4096];
    int ln = 0, have_hdr = 0, have_id = 0, have_mbx = 0, nsm = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *tok[MAXTOK];
        int n = tokenize(line, tok);
        if (n == 0 || tok[0][0] == '#') continue;
        uint32_t v;
        if (!strcmp(tok[0], "profile")) {
            if (n != 2 || strcmp(tok[1], "1")) { seterr(err, errlen, "%s:%d: unsupported profile version", path, ln); goto fail; }
            have_hdr = 1;
        } else if (!have_hdr) {
            seterr(err, errlen, "%s:%d: missing 'profile 1' header", path, ln); goto fail;
        } else if (!strcmp(tok[0], "name") && n >= 2) {
            snprintf(p->name, sizeof(p->name), "%s", tok[1]);
        } else if (!strcmp(tok[0], "identity")) {
            NEED("vendor", p->vendor); NEED("product", p->product); NEED("rev", p->rev); NEED("serial", p->serial);
            have_id = 1;
        } else if (!strcmp(tok[0], "config") && n == 8) {
            for (int i = 0; i < 7; i++) {
                if (u32(tok[1 + i], &v) || v > 0xFFFF) { seterr(err, errlen, "%s:%d: bad config word", path, ln); goto fail; }
                p->config[i] = (uint16_t)v;
            }
            p->has_config = 1;
        } else if (!strcmp(tok[0], "mailbox") && n == 9) {
            uint32_t a, b, c, d;
            if (strcmp(tok[1], "out") || u32(tok[2], &a) || u32(tok[3], &b) || strcmp(tok[4], "in") ||
                u32(tok[5], &c) || u32(tok[6], &d) || strcmp(tok[7], "coe") || u32(tok[8], &v)) {
                seterr(err, errlen, "%s:%d: bad mailbox line", path, ln); goto fail;
            }
            p->mbx_out = (uint16_t)a; p->mbx_out_len = (uint16_t)b;
            p->mbx_in = (uint16_t)c; p->mbx_in_len = (uint16_t)d; p->coe = (uint8_t)v;
            have_mbx = 1;
        } else if (!strcmp(tok[0], "coe")) {
            NEED("sdoinfo", p->sdoinfo); NEED("pdoassign", p->pdoassign); NEED("pdoconfig", p->pdoconfig); NEED("ca", p->ca);
        } else if (!strcmp(tok[0], "sm") && n >= 2) {
            uint32_t i, st, len, ctrl, en;
            if (u32(tok[1], &i) || i > 3) { seterr(err, errlen, "%s:%d: bad sm", path, ln); goto fail; }
            NEED("start", st); NEED("len", len); NEED("ctrl", ctrl); NEED("en", en);
            p->sm[i].start = (uint16_t)st; p->sm[i].len = (uint16_t)len;
            p->sm[i].ctrl = (uint8_t)ctrl; p->sm[i].en = (uint8_t)en;
            nsm++;
        } else if (!strcmp(tok[0], "dc")) {
            if (n < 2 || u32(tok[1], &v)) { seterr(err, errlen, "%s:%d: bad dc", path, ln); goto fail; }
            p->dc = (uint8_t)v;
            NEED("assign", p->dc_assign);
        } else if (!strcmp(tok[0], "pdo") && n >= 3) {
            if (p->npdo >= ESC_PROF_MAX_PDO) { seterr(err, errlen, "%s:%d: too many PDOs", path, ln); goto fail; }
            esc_prof_pdo_t *q = &p->pdo[p->npdo];
            memset(q, 0, sizeof(*q));
            q->dir = (uint8_t)!strcmp(tok[1], "tx");
            if (strcmp(tok[1], "rx") && strcmp(tok[1], "tx")) { seterr(err, errlen, "%s:%d: pdo rx|tx", path, ln); goto fail; }
            if (u32(tok[2], &v) || v > 0xFFFF) { seterr(err, errlen, "%s:%d: bad pdo index", path, ln); goto fail; }
            q->index = (uint16_t)v;
            uint32_t sm, fx;
            NEED("sm", sm); NEED("fixed", fx);
            q->sm = (uint8_t)sm; q->fixed = (uint8_t)fx;
            const char *ents = kv(tok, n, 3, "entries"), *exs = kv(tok, n, 3, "excludes");
            if (!ents || !exs) { seterr(err, errlen, "%s:%d: pdo: entries/excludes missing", path, ln); goto fail; }
            if (strcmp(ents, "-")) {
                char buf[2048];
                snprintf(buf, sizeof(buf), "%s", ents);
                for (char *s = NULL, *t = strtok_r(buf, ",", &s); t; t = strtok_r(NULL, ",", &s)) {
                    unsigned ix, sb, bl;
                    if (sscanf(t, "%x:%x:%u", &ix, &sb, &bl) != 3 || q->n >= ESC_PROF_MAX_ENTRIES || bl > 255) {
                        seterr(err, errlen, "%s:%d: bad entry '%s'", path, ln, t); goto fail;
                    }
                    q->entry[q->n++] = (uint32_t)ix << 16 | (uint32_t)sb << 8 | bl;
                }
            }
            if (strcmp(exs, "-")) {
                char buf[512];
                snprintf(buf, sizeof(buf), "%s", exs);
                for (char *s = NULL, *t = strtok_r(buf, ",", &s); t; t = strtok_r(NULL, ",", &s)) {
                    if (u32(t, &v) || q->nex >= ESC_PROF_MAX_EXCL) { seterr(err, errlen, "%s:%d: bad exclude", path, ln); goto fail; }
                    q->ex[q->nex++] = (uint16_t)v;
                }
            }
            p->npdo++;
        } else if (!strcmp(tok[0], "obj") && n == 4) {
            if (p->nobj >= cap_obj) { seterr(err, errlen, "%s:%d: too many objects", path, ln); goto fail; }
            esc_prof_obj_t *o = &p->obj[p->nobj++];
            if (u32(tok[1], &v) || v > 0xFFFF) { seterr(err, errlen, "%s:%d: bad obj index", path, ln); goto fail; }
            o->index = (uint16_t)v;
            o->code = !strcmp(tok[2], "var") ? 7 : !strcmp(tok[2], "array") ? 8 : 9;
            o->first = p->nsub;
            o->n = 0;
        } else if (!strcmp(tok[0], "sub") && n == 7) {
            if (!p->nobj || p->nsub >= cap_sub) { seterr(err, errlen, "%s:%d: sub without obj / too many", path, ln); goto fail; }
            esc_prof_obj_t *o = &p->obj[p->nobj - 1];
            esc_prof_sub_t *s = &p->sub[p->nsub];
            uint32_t ix, sb, bits;
            if (u32(tok[1], &ix) || ix != o->index || u32(tok[2], &sb) || sb > 255 || strcmp(tok[3], "bits") ||
                u32(tok[4], &bits) || bits == 0 || bits > 8 * ESC_PROF_VAL_MAX) {
                seterr(err, errlen, "%s:%d: bad sub line", path, ln); goto fail;
            }
            s->index = (uint16_t)ix; s->sub = (uint8_t)sb; s->bits = (uint16_t)bits;
            s->access = !strcmp(tok[5], "rw") ? ESC_PROF_RW : !strcmp(tok[5], "rw_preop") ? ESC_PROF_RW_PREOP : ESC_PROF_RO;
            uint8_t l;
            if (hex_bytes(tok[6], s->dflt, sizeof(s->dflt), &l)) { seterr(err, errlen, "%s:%d: bad value", path, ln); goto fail; }
            s->len = (uint8_t)((bits + 7) / 8);
            if (sb > o->max_sub) o->max_sub = (uint8_t)sb;
            o->n++;
            p->nsub++;
        } else {
            seterr(err, errlen, "%s:%d: unknown record '%s'", path, ln, tok[0]); goto fail;
        }
    }
    fclose(f);
    if (!have_hdr || !have_id || !have_mbx || nsm != 4) {
        seterr(err, errlen, "%s: incomplete profile (header %d identity %d mailbox %d sm %d/4)",
               path, have_hdr, have_id, have_mbx, nsm);
        esc_prof_free(p);
        return NULL;
    }
    return p;
fail:
    fclose(f);
    esc_prof_free(p);
    return NULL;
}

void esc_prof_free(esc_profile_t *p)
{
    if (!p) return;
    free(p->obj);
    free(p->sub);
    free(p);
}

/* ---------------------------------------------------------------- SII --- */
static void put32(uint16_t *w, uint32_t v) { w[0] = (uint16_t)v; w[1] = (uint16_t)(v >> 16); }

static uint8_t crc8(const uint16_t *w)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < 14; i++) {
        uint8_t byte = (uint8_t)((i & 1) ? (w[i / 2] >> 8) : (w[i / 2] & 0xFF));
        crc ^= byte;
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static int build_sii(esc_t *esc, const esc_profile_t *p)
{
    uint16_t *w = esc->sii_image_buf;
    size_t cap = ESC_SII_IMAGE_MAX_WORDS, n = 0;
    memset(w, 0, cap * sizeof(*w));
    if (p->has_config) memcpy(w, p->config, sizeof(p->config));
    else w[0] = SII_WORD0_PDI_CONTROL;
    w[7] = crc8(w);
    put32(w + 8, p->vendor); put32(w + 10, p->product); put32(w + 12, p->rev); put32(w + 14, p->serial);
    w[24] = p->mbx_out; w[25] = p->mbx_out_len; w[26] = p->mbx_in; w[27] = p->mbx_in_len;
    w[28] = p->coe ? SII_MBX_PROTOCOL_COE : 0;
    n = SII_CATEGORY_START_WORD;

    /* Strings (10): one string, the device name (SOEM shows it as the
     * slave name; General byte 3 points at it) */
    size_t nl = strlen(p->name);
    if (nl > 63) nl = 63;
    size_t sbytes = 2 + nl, swords = (sbytes + 1) / 2;
    if (nl) {
        if (n + 2 + swords > cap) return -1;
        w[n++] = SII_CAT_STRINGS; w[n++] = (uint16_t)swords;
        uint8_t *b = (uint8_t *)(w + n);      /* SII words are little endian; so is the host */
        b[0] = 1;
        b[1] = (uint8_t)nl;
        memcpy(b + 2, p->name, nl);
        n += swords;
    }

    /* General (30): CoE details (ETG.2010: bit0 SDO, 1 SDO Info, 2 PDO assign,
     * 3 PDO configuration, 5 complete access), PhysicalPort 0x0011 */
    if (n + 18 > cap) return -1;
    w[n++] = SII_CAT_GENERAL; w[n++] = 16;
    uint8_t det = p->coe ? (uint8_t)(0x01 | (p->sdoinfo ? 0x02 : 0) | (p->pdoassign ? 0x04 : 0) |
                                     (p->pdoconfig ? 0x08 : 0) | (p->ca ? 0x20 : 0)) : 0;
    w[n + 1] = (uint16_t)(nl ? 0x0100 : 0);   /* byte 3: name = string 1 */
    w[n + 2] = (uint16_t)((uint16_t)det << 8);
    w[n + 8] = 0x0011;
    n += 16;

    /* SyncM (41) */
    if (n + 18 > cap) return -1;
    w[n++] = SII_CAT_SYNCM; w[n++] = 16;
    for (int i = 0; i < 4; i++) {
        w[n++] = p->sm[i].start;
        w[n++] = p->sm[i].len;
        w[n++] = p->sm[i].ctrl;
        w[n++] = (uint16_t)(p->sm[i].en && (i < 2 || p->sm[i].len) ? 1 : 0);
    }

    /* TxPDO (50) then RxPDO (51), every PDO, unassigned ones with SM 0xFF */
    for (int cat = 0; cat < 2; cat++) {
        int dir = cat == 0 ? 1 : 0;
        size_t words = 0;
        for (int k = 0; k < p->npdo; k++)
            if (p->pdo[k].dir == dir) words += 4 + 4 * (size_t)p->pdo[k].n;
        if (!words) continue;
        if (n + 2 + words > cap) return -1;
        w[n++] = (uint16_t)(cat == 0 ? SII_CAT_TXPDO : SII_CAT_RXPDO);
        w[n++] = (uint16_t)words;
        for (int k = 0; k < p->npdo; k++) {
            const esc_prof_pdo_t *q = &p->pdo[k];
            if (q->dir != dir) continue;
            w[n++] = q->index;
            w[n++] = (uint16_t)(q->n | (uint16_t)q->sm << 8);
            w[n++] = 0;                                  /* synchronization, name */
            w[n++] = q->fixed ? 0x0010 : 0;              /* flags (bit4 fixed content) */
            for (int e = 0; e < q->n; e++) {
                uint32_t en = q->entry[e];
                w[n++] = (uint16_t)(en >> 16);           /* index            */
                w[n++] = (uint16_t)((en >> 8) & 0xFF);   /* sub, name idx 0  */
                w[n++] = (uint16_t)((en & 0xFF) << 8);   /* datatype 0, bits */
                w[n++] = 0;                              /* flags            */
            }
        }
    }
    if (n + 1 > cap) return -1;
    w[n++] = SII_CAT_END;
    esc->sii_image_words = n;
    return 0;
}

/* --------------------------------------------------------- attach / OD --- */
int esc_prof_attach(esc_t *esc, const esc_profile_t *p)
{
    if (!esc->prof_st) {
        esc->prof_st = calloc(1, sizeof(*esc->prof_st));
        if (!esc->prof_st) return -1;
    }
    free(esc->prof_st->val);
    esc->prof_st->val = calloc((size_t)(p->nsub ? p->nsub : 1), ESC_PROF_VAL_MAX);
    if (!esc->prof_st->val) return -1;
    for (int i = 0; i < p->nsub; i++)
        memcpy(esc->prof_st->val + (size_t)i * ESC_PROF_VAL_MAX, p->sub[i].dflt, ESC_PROF_VAL_MAX);
    esc->prof = p;
    esc->mbx_out = p->mbx_out; esc->mbx_out_len = p->mbx_out_len;
    esc->mbx_in = p->mbx_in;   esc->mbx_in_len = p->mbx_in_len;
    esc->coe_pdo_od = 0;
    esc->coe_ca = p->ca;
    esc->pdo_size_bytes = p->sm[2].len > p->sm[3].len ? p->sm[2].len : p->sm[3].len;
    return build_sii(esc, p);
}

const esc_prof_obj_t *esc_prof_obj(const esc_t *esc, uint16_t index)
{
    const esc_profile_t *p = esc->prof;
    for (int i = 0; i < p->nobj; i++)
        if (p->obj[i].index == index) return &p->obj[i];
    return NULL;
}

int esc_prof_sub(const esc_t *esc, uint16_t index, uint8_t sub)
{
    const esc_prof_obj_t *o = esc_prof_obj(esc, index);
    if (!o) return -1;
    for (int k = o->first; k < o->first + o->n; k++)
        if (esc->prof->sub[k].sub == sub) return k;
    return -1;
}

uint8_t *esc_prof_value(const esc_t *esc, int gsub)
{
    return esc->prof_st->val + (size_t)gsub * ESC_PROF_VAL_MAX;
}

static const esc_prof_pdo_t *find_pdo(const esc_profile_t *p, uint16_t index)
{
    for (int k = 0; k < p->npdo; k++)
        if (p->pdo[k].index == index) return &p->pdo[k];
    return NULL;
}

static uint16_t assigned(const esc_t *esc, uint16_t obj, int k)
{
    int g = esc_prof_sub(esc, obj, (uint8_t)k);
    if (g < 0) return 0;
    const uint8_t *v = esc_prof_value(esc, g);
    return (uint16_t)(v[0] | v[1] << 8);
}

static int assigned_n(const esc_t *esc, uint16_t obj)
{
    int g = esc_prof_sub(esc, obj, 0);
    return g < 0 ? 0 : esc_prof_value(esc, g)[0];
}

#define ABORT_VALUE_RANGE 0x06090030u

/* A whole assignment (n PDO indices) for 0x1C12 (dir 0) / 0x1C13 (dir 1):
 * every PDO exists with this direction, none twice, none excluded by
 * another one of the set (ESI <Exclude>). */
static uint32_t check_set(const esc_t *esc, int dir, int n, const uint16_t *list)
{
    for (int a = 0; a < n; a++) {
        const esc_prof_pdo_t *pa = find_pdo(esc->prof, list[a]);
        if (!pa || pa->dir != dir) return ABORT_VALUE_RANGE;
        for (int b = 0; b < n; b++) {
            if (a == b) continue;
            if (list[b] == pa->index) return ABORT_VALUE_RANGE;          /* twice */
            for (int x = 0; x < pa->nex; x++)
                if (pa->ex[x] == list[b]) return ABORT_VALUE_RANGE;      /* ESI Exclude */
        }
    }
    return 0;
}

uint32_t esc_prof_check_value(const esc_t *esc, uint16_t index, uint8_t sub,
                              const uint8_t *data, uint32_t len)
{
    if (index != 0x1C12 && index != 0x1C13) return 0;
    if (esc->prof_st->ca_bypass) return 0;      /* checked whole by esc_prof_check_ca() */
    int dir = index == 0x1C13;
    const esc_prof_obj_t *o = esc_prof_obj(esc, index);
    if (sub == 0) {
        if (len < 1 || data[0] > o->max_sub) return ABORT_VALUE_RANGE;
        /* the set that becomes active: SI1..SIn as stored now */
        uint16_t list[256];
        int n = data[0];
        for (int a = 1; a <= n; a++) list[a - 1] = assigned(esc, index, a);
        return check_set(esc, dir, n, list);
    }
    if (len < 2) return ABORT_VALUE_RANGE;
    uint16_t v = (uint16_t)(data[0] | data[1] << 8);
    /* ETG.1020: SIk may only change while SI0 = 0 */
    if (assigned_n(esc, index) != 0) return 0x08000022u;   /* wrong device state */
    if (v == 0) return 0;                       /* clearing an entry is fine */
    const esc_prof_pdo_t *q = find_pdo(esc->prof, v);
    if (!q || q->dir != dir) return ABORT_VALUE_RANGE;
    return 0;
}

uint32_t esc_prof_check_ca(const esc_t *esc, uint16_t index, uint8_t start,
                           const uint8_t *data, uint32_t len)
{
    if (index != 0x1C12 && index != 0x1C13) return 0;
    const esc_prof_obj_t *o = esc_prof_obj(esc, index);
    uint16_t list[256];
    int n, pos;
    if (start == 0) {
        if (len < 2) return 0;                  /* the CA code reports the length */
        n = data[0];
        pos = 2;
    } else {
        n = assigned_n(esc, index);
        pos = 0;
    }
    if (n > o->max_sub) return ABORT_VALUE_RANGE;
    for (int a = 1; a <= n; a++, pos += 2)
        list[a - 1] = (uint32_t)pos + 2 <= len ? (uint16_t)(data[pos] | data[pos + 1] << 8)
                                               : assigned(esc, index, a);
    return check_set(esc, index == 0x1C13, n, list);
}

uint32_t esc_prof_assigned_bits(const esc_t *esc, int dir)
{
    uint16_t obj = dir ? 0x1C13 : 0x1C12;
    uint32_t bits = 0;
    int n = assigned_n(esc, obj);
    for (int a = 1; a <= n; a++) {
        uint16_t pdo = assigned(esc, obj, a);
        int g0 = esc_prof_sub(esc, pdo, 0);
        if (g0 < 0) continue;
        int m = esc_prof_value(esc, g0)[0];
        for (int e = 1; e <= m; e++) {
            int g = esc_prof_sub(esc, pdo, (uint8_t)e);
            if (g >= 0) bits += esc_prof_value(esc, g)[0];   /* low byte = bit length */
        }
    }
    return bits;
}

uint16_t esc_prof_check_pd(const esc_t *esc)
{
    for (int dir = 0; dir < 2; dir++) {
        const uint8_t *sm = esc->regs + REG_SM_BASE + (2 + dir) * REG_SM_ENTRY_SIZE;
        uint16_t len = (uint16_t)(sm[SM_OFF_LENGTH] | sm[SM_OFF_LENGTH + 1] << 8);
        int enabled = sm[SM_OFF_ACTIVATE] & SM_ACT_ENABLE;
        uint32_t want = esc_prof_assigned_bits(esc, dir);
        if (want % 8) want += 8;              /* SM sizes are whole bytes */
        if ((enabled ? len : 0) != want / 8)
            return dir ? 0x001E : 0x001D;     /* invalid input / output configuration */
    }
    return 0;
}
