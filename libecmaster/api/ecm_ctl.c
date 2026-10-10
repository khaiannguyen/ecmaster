/* ecm_ctl.c -- Phase 11.2, see ecm_ctl.h and docs/api.md §10 */
#define _GNU_SOURCE
#include "ecm_ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ecm_json.h"

/* Negative controls (make negctl): each bit removes one protection, and the
 * offline test that covers it MUST fail. Never set in a real build. */
#ifndef ECM_CTL_BROKEN
#define ECM_CTL_BROKEN 0
#endif
#define BROKEN_RXPDO   1
#define BROKEN_RATE    2
#define BROKEN_DENY    4
#define BROKEN_RANGE   8
#define BROKEN_INITCMD 16

/* ------------------------------------------------------------------ types --- */
static const struct { const char *name; int size; } TYPES[ECM_CT_COUNT] = {
    [ECM_CT_U8] = { "u8", 1 },   [ECM_CT_I8] = { "i8", 1 },   [ECM_CT_U16] = { "u16", 2 },
    [ECM_CT_I16] = { "i16", 2 }, [ECM_CT_U32] = { "u32", 4 }, [ECM_CT_I32] = { "i32", 4 },
    [ECM_CT_U64] = { "u64", 8 }, [ECM_CT_I64] = { "i64", 8 }, [ECM_CT_F32] = { "f32", 4 },
    [ECM_CT_RAW] = { "raw", 0 },
};

int ecm_ctl_type_parse(const char *name)
{
    for (int t = 0; t < ECM_CT_COUNT; t++) if (!strcmp(name, TYPES[t].name)) return t;
    return -1;
}

const char *ecm_ctl_type_name(int t) { return t >= 0 && t < ECM_CT_COUNT ? TYPES[t].name : "?"; }
int         ecm_ctl_type_size(int t) { return t >= 0 && t < ECM_CT_COUNT ? TYPES[t].size : 0; }

static int is_signed(int t) { return t == ECM_CT_I8 || t == ECM_CT_I16 || t == ECM_CT_I32 || t == ECM_CT_I64; }
static int is_unsigned(int t) { return t == ECM_CT_U8 || t == ECM_CT_U16 || t == ECM_CT_U32 || t == ECM_CT_U64; }

int ecm_ctl_val_fits(int t, const ecm_ctl_val_t *v)
{
    if (is_signed(t)) {
        int64_t x;
        if (v->kind == 0) x = v->i;
        else if (v->kind == 1 && v->u <= (uint64_t)INT64_MAX) x = (int64_t)v->u;
        else return -1;
        int bits = 8 * TYPES[t].size;
        if (bits == 64) return 0;
        int64_t lo = -((int64_t)1 << (bits - 1)), hi = ((int64_t)1 << (bits - 1)) - 1;
        return x >= lo && x <= hi ? 0 : -1;
    }
    if (is_unsigned(t)) {
        uint64_t x;
        if (v->kind == 1) x = v->u;
        else if (v->kind == 0 && v->i >= 0) x = (uint64_t)v->i;
        else return -1;
        int bits = 8 * TYPES[t].size;
        return bits == 64 || x <= (((uint64_t)1 << bits) - 1) ? 0 : -1;
    }
    if (t == ECM_CT_F32) {
        double d = v->kind == 2 ? v->d : v->kind == 0 ? (double)v->i : (double)v->u;
        return isfinite(d) && fabs(d) <= 3.4028234663852886e38 ? 0 : -1;
    }
    return -1;
}

static int64_t as_i(const ecm_ctl_val_t *v) { return v->kind == 0 ? v->i : (int64_t)v->u; }
static uint64_t as_u(const ecm_ctl_val_t *v) { return v->kind == 1 ? v->u : (uint64_t)v->i; }
static double as_d(const ecm_ctl_val_t *v) { return v->kind == 2 ? v->d : v->kind == 0 ? (double)v->i : (double)v->u; }

int ecm_ctl_encode(int t, const ecm_ctl_val_t *v, uint8_t *b)
{
    if (ecm_ctl_val_fits(t, v)) return -1;
    uint64_t raw;
    if (t == ECM_CT_F32) {
        float f = (float)as_d(v);
        uint32_t w;
        memcpy(&w, &f, 4);
        raw = w;
    } else {
        raw = is_signed(t) ? (uint64_t)as_i(v) : as_u(v);
    }
    for (int i = 0; i < TYPES[t].size; i++) b[i] = (uint8_t)(raw >> (8 * i));
    return 0;
}

int ecm_ctl_val_cmp(int t, const ecm_ctl_val_t *v, const ecm_ctl_val_t *lim)
{
    if (is_signed(t)) { int64_t a = as_i(v), b = as_i(lim); return a < b ? -1 : a > b; }
    if (is_unsigned(t)) { uint64_t a = as_u(v), b = as_u(lim); return a < b ? -1 : a > b; }
    double a = as_d(v), b = as_d(lim);
    return a < b ? -1 : a > b;
}

size_t ecm_ctl_decode_json(int t, const uint8_t *b, int n, char *out, size_t cap)
{
    if (t < 0 || t >= ECM_CT_COUNT || t == ECM_CT_RAW || n != TYPES[t].size) return 0;
    uint64_t raw = 0;
    for (int i = 0; i < n; i++) raw |= (uint64_t)b[i] << (8 * i);
    int r;
    switch (t) {
    case ECM_CT_I8:  r = snprintf(out, cap, "%d", (int)(int8_t)raw); break;
    case ECM_CT_I16: r = snprintf(out, cap, "%d", (int)(int16_t)raw); break;
    case ECM_CT_I32: r = snprintf(out, cap, "%" PRId32, (int32_t)raw); break;
    case ECM_CT_I64: r = snprintf(out, cap, "%" PRId64, (int64_t)raw); break;
    case ECM_CT_F32: {
        uint32_t w = (uint32_t)raw;
        float f;
        memcpy(&f, &w, 4);
        r = isfinite(f) ? snprintf(out, cap, "%.9g", (double)f) : snprintf(out, cap, "null");
        break;
    }
    default:         r = snprintf(out, cap, "%" PRIu64, raw); break;
    }
    return r > 0 && (size_t)r < cap ? (size_t)r : 0;
}

/* --------------------------------------------------------- reply writer --- */
typedef struct { char *b; size_t len, cap; int ovf; } rw_t;

static void rput(rw_t *w, const char *s, size_t n)
{
    if (w->ovf) return;
    if (w->len + n + 2 > w->cap) { w->ovf = 1; return; }     /* keep room for "\n\0" */
    memcpy(w->b + w->len, s, n);
    w->len += n;
    w->b[w->len] = '\0';
}

static void rs(rw_t *w, const char *s) { rput(w, s, strlen(s)); }

static void rf(rw_t *w, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= sizeof(tmp)) { w->ovf = 1; return; }
    rput(w, tmp, (size_t)r);
}

static void rstr(rw_t *w, const char *s)
{
    rput(w, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; rput(w, e, 2); }
        else if (*p < 0x20) rf(w, "\\u%04x", *p);
        else rput(w, (const char *)p, 1);
    }
    rput(w, "\"", 1);
}

static void rhex(rw_t *w, const uint8_t *b, int n)
{
    rput(w, "\"", 1);
    for (int i = 0; i < n; i++) rf(w, "%02X", b[i]);
    rput(w, "\"", 1);
}

static void rval(rw_t *w, int t, const ecm_ctl_val_t *v)
{
    if (t == ECM_CT_F32) rf(w, "%.9g", as_d(v));
    else if (is_signed(t)) rf(w, "%" PRId64, as_i(v));
    else rf(w, "%" PRIu64, as_u(v));
}

static size_t rend(rw_t *w)
{
    if (w->ovf) return 0;
    w->b[w->len++] = '\n';
    w->b[w->len] = '\0';
    return w->len;
}

/* ------------------------------------------------------------- allowlist --- */
static int comm_area(uint16_t index) { return index >= 0x1000 && index <= 0x1FFF; }

static int tok_val(const char *s, const ecm_jtok_t *tk, int type, ecm_ctl_val_t *v)
{
    memset(v, 0, sizeof(*v));
    if (type == ECM_CT_F32) { v->kind = 2; return ecm_json_f64(s, tk, &v->d); }
    if (is_signed(type)) { v->kind = 0; return ecm_json_i64(s, tk, &v->i); }
    v->kind = 1;
    return ecm_json_u64(s, tk, &v->u);
}

static int get_u32(const char *s, const ecm_jtok_t *t, int n, int obj, const char *k, uint32_t max, uint32_t *out)
{
    int v = ecm_json_get(s, t, n, obj, k);
    uint64_t x;
    if (v < 0 || ecm_json_u64(s, &t[v], &x) || x > max) return -1;
    *out = (uint32_t)x;
    return 0;
}

int ecm_ctl_allow_load(const char *path, ecm_ctl_allowlist_t *al, const ecm_ctl_backend_t *be,
                       char *err, size_t errlen)
{
    memset(al, 0, sizeof(*al));
    if (!path) return 0;
    snprintf(al->path, sizeof(al->path), "%s", path);
    FILE *f = fopen(path, "re");
    if (!f) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); return -1; }
    static char text[64 * 1024];
    size_t len = fread(text, 1, sizeof(text) - 1, f);
    int big = !feof(f);
    fclose(f);
    if (big) { snprintf(err, errlen, "%s: larger than %zu bytes", path, sizeof(text) - 1); return -1; }
    text[len] = '\0';
    static ecm_jtok_t t[4096];
    int pos = 0, n = ecm_json_parse(text, len, t, 4096, &pos);
    if (n < 0) { snprintf(err, errlen, "%s: JSON error at byte %d", path, pos); return -1; }
    int sv = ecm_json_get(text, t, n, 0, "schema");
    uint64_t schema;
    if (sv < 0 || ecm_json_u64(text, &t[sv], &schema) || schema != 1) {
        snprintf(err, errlen, "%s: \"schema\": 1 expected", path); return -1;
    }
    int a = ecm_json_get(text, t, n, 0, "allow");
    if (a < 0 || t[a].type != ECM_J_ARR) { snprintf(err, errlen, "%s: \"allow\": [...] expected", path); return -1; }
    int i = a + 1;
    for (int k = 0; k < t[a].size; k++, i = ecm_json_skip(t, n, i)) {
        if (al->n >= ECM_CTL_ALLOW_MAX) { snprintf(err, errlen, "%s: more than %d entries", path, ECM_CTL_ALLOW_MAX); return -1; }
        ecm_ctl_allow_t *e = &al->e[al->n];
        uint32_t idx, sub;
        char tn[16];
        if (t[i].type != ECM_J_OBJ) { snprintf(err, errlen, "%s: allow[%d] is not an object", path, k); return -1; }
        if (get_u32(text, t, n, i, "vendor", UINT32_MAX, &e->vendor) || get_u32(text, t, n, i, "product", UINT32_MAX, &e->product)) {
            snprintf(err, errlen, "%s: allow[%d]: vendor and product (\"0x...\") required", path, k); return -1;
        }
        if (get_u32(text, t, n, i, "index", 0xFFFF, &idx) || get_u32(text, t, n, i, "sub", 0xFF, &sub)) {
            snprintf(err, errlen, "%s: allow[%d]: index (0..0xFFFF) and sub (0..255) required", path, k); return -1;
        }
        e->index = (uint16_t)idx; e->sub = (uint8_t)sub;
        int tv = ecm_json_get(text, t, n, i, "type");
        if (tv < 0 || ecm_json_str(text, &t[tv], tn, sizeof(tn)) || (e->type = ecm_ctl_type_parse(tn)) < 0 || e->type == ECM_CT_RAW) {
            snprintf(err, errlen, "%s: allow[%d]: type u8|i8|u16|i16|u32|i32|u64|i64|f32 required", path, k); return -1;
        }
        int mn = ecm_json_get(text, t, n, i, "min"), mx = ecm_json_get(text, t, n, i, "max");
        if (mn < 0 || mx < 0 || tok_val(text, &t[mn], e->type, &e->min) || tok_val(text, &t[mx], e->type, &e->max) ||
            ecm_ctl_val_fits(e->type, &e->min) || ecm_ctl_val_fits(e->type, &e->max) ||
            ecm_ctl_val_cmp(e->type, &e->min, &e->max) > 0) {
            snprintf(err, errlen, "%s: allow[%d] 0x%04X:%u: min <= max within %s required", path, k, idx, sub, tn); return -1;
        }
        int nt = ecm_json_get(text, t, n, i, "note");
        if (nt >= 0 && ecm_json_str(text, &t[nt], e->note, sizeof(e->note))) e->note[0] = '\0';
        char why[128] = "";
        if (comm_area(e->index)) {
            snprintf(err, errlen, "%s: allow[%d] 0x%04X:%u: the communication area 0x1000..0x1FFF is never written "
                     "through the API", path, k, idx, sub);
            return -1;
        }
        if (be && be->deny && be->deny(be->ctx, 0, e->index, e->sub, why, sizeof(why))) {
            snprintf(err, errlen, "%s: allow[%d] 0x%04X:%u: never written through the API: %s", path, k, idx, sub, why);
            return -1;
        }
        for (int j = 0; j < al->n; j++)
            if (al->e[j].vendor == e->vendor && al->e[j].product == e->product && al->e[j].index == e->index && al->e[j].sub == e->sub) {
                snprintf(err, errlen, "%s: allow[%d] 0x%04X:%u: listed twice", path, k, idx, sub); return -1;
            }
        al->n++;
    }
    return 0;
}

static const ecm_ctl_allow_t *allow_find(const ecm_ctl_allowlist_t *al, uint32_t vendor, uint32_t product,
                                         uint16_t index, uint8_t sub)
{
    for (int i = 0; al && i < al->n; i++) {
        const ecm_ctl_allow_t *e = &al->e[i];
        if (e->vendor == vendor && e->product == product && e->index == index && e->sub == sub) return e;
    }
    return NULL;
}

/* ---------------------------------------------------------------- handle --- */
void ecm_ctl_state_init(ecm_ctl_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->tokens = ECM_CTL_RATE;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
    const char *s;
    ecm_jtok_t  t[64];
    int         n;
    char        id[72];             /* raw JSON of "id", or "null" */
} req_t;

static void head(rw_t *w, const req_t *r, int ok) { rf(w, "{\"id\":%s,\"ok\":%s", r->id, ok ? "true" : "false"); }

static size_t fail(ecm_ctl_state_t *st, rw_t *w, const req_t *r, const char *code, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    w->len = 0; w->ovf = 0;
    head(w, r, 0);
    rs(w, ",\"error\":"); rstr(w, code);
    rs(w, ",\"msg\":"); rstr(w, msg);
    rs(w, "}");
    st->errors++;
    return rend(w);
}

/* slave, index, sub (+ type) of an SDO request; returns NULL or an error message */
static const char *sdo_args(const req_t *r, int *slave, uint16_t *index, uint8_t *sub, int *type, int need_type)
{
    uint64_t x;
    int v = ecm_json_get(r->s, r->t, r->n, 0, "slave");
    if (v < 0 || ecm_json_u64(r->s, &r->t[v], &x) || x < 1 || x > 0xFFFF) return "\"slave\": 1..65535 required";
    *slave = (int)x;
    v = ecm_json_get(r->s, r->t, r->n, 0, "index");
    if (v < 0 || ecm_json_u64(r->s, &r->t[v], &x) || x > 0xFFFF) return "\"index\": 0..0xFFFF required (\"0x6061\" or a number)";
    *index = (uint16_t)x;
    v = ecm_json_get(r->s, r->t, r->n, 0, "sub");
    if (v < 0 || ecm_json_u64(r->s, &r->t[v], &x) || x > 0xFF) return "\"sub\": 0..255 required";
    *sub = (uint8_t)x;
    char tn[16] = "raw";
    v = ecm_json_get(r->s, r->t, r->n, 0, "type");
    if (v >= 0 && ecm_json_str(r->s, &r->t[v], tn, sizeof(tn))) return "\"type\": a string";
    if (v < 0 && need_type) return "\"type\" required";
    if ((*type = ecm_ctl_type_parse(tn)) < 0) return "\"type\": u8 i8 u16 i16 u32 i32 u64 i64 f32 raw";
    if (ecm_json_get(r->s, r->t, r->n, 0, "ca") >= 0) return "Complete Access is not offered through the API";
    return NULL;
}

static void sdo_head(rw_t *w, const req_t *r, int slave, uint16_t index, uint8_t sub, int type)
{
    head(w, r, 1);
    rf(w, ",\"slave\":%d,\"index\":\"0x%04X\",\"sub\":%u,\"type\":\"%s\"", slave, index, sub, ecm_ctl_type_name(type));
}

static size_t sdo_fail(ecm_ctl_state_t *st, rw_t *w, const req_t *r, const ecm_ctl_backend_t *be,
                       int rc, uint32_t abort, int slave, uint16_t index, uint8_t sub)
{
    if (rc == ECM_CTL_SDO_BUSY) return fail(st, w, r, "busy", "mailbox queue full, retry");
    if (rc == ECM_CTL_SDO_TIMEOUT) return fail(st, w, r, "timeout", "slave %d 0x%04X:%u: no completion in time", slave, index, sub);
    if (rc == ECM_CTL_SDO_TOO_LARGE) return fail(st, w, r, "too_large", "more than %d bytes", ECM_CTL_DATA_MAX);
    w->len = 0; w->ovf = 0;
    head(w, r, 0);
    rs(w, ",\"error\":\"sdo_failed\"");
    if (abort) {
        rf(w, ",\"abort\":\"0x%08X\",\"msg\":", abort);
        rstr(w, be->abort_text ? be->abort_text(abort) : "");
    } else {
        rf(w, ",\"abort\":null,\"msg\":\"slave %d 0x%04X:%u: no answer, or no abort code seen\"", slave, index, sub);
    }
    rs(w, "}");
    st->errors++;
    return rend(w);
}

static size_t cmd_sdo_read(ecm_ctl_state_t *st, const ecm_ctl_backend_t *be, const req_t *r, rw_t *w)
{
    int slave, type;
    uint16_t index;
    uint8_t sub;
    const char *e = sdo_args(r, &slave, &index, &sub, &type, 0);
    if (e) return fail(st, w, r, "bad_request", "%s", e);
    uint32_t v, p;
    if (be->identity(be->ctx, slave, &v, &p)) return fail(st, w, r, "no_slave", "no slave %d", slave);
    char why[160] = "";
    if (be->ready(be->ctx, slave, why, sizeof(why))) return fail(st, w, r, "not_ready", "%s", why);
    uint8_t b[ECM_CTL_DATA_MAX];
    int size = sizeof(b);
    uint32_t abort = 0;
    uint64_t t0 = mono_ns();
    int rc = be->sdo_read(be->ctx, slave, index, sub, b, &size, &abort);
    uint64_t dt = mono_ns() - t0;
    if (dt > st->sdo_ns_max) st->sdo_ns_max = dt;
    st->sdo_reads++;
    if (rc) return sdo_fail(st, w, r, be, rc, abort, slave, index, sub);
    if (type != ECM_CT_RAW && size != ecm_ctl_type_size(type)) {
        w->len = 0; w->ovf = 0;
        head(w, r, 0);
        rf(w, ",\"error\":\"size_mismatch\",\"msg\":\"%s is %d bytes, the object has %d\",\"size\":%d,\"raw\":",
           ecm_ctl_type_name(type), ecm_ctl_type_size(type), size, size);
        rhex(w, b, size);
        rs(w, "}");
        st->errors++;
        return rend(w);
    }
    sdo_head(w, r, slave, index, sub, type);
    rf(w, ",\"size\":%d,\"raw\":", size);
    rhex(w, b, size);
    if (type != ECM_CT_RAW) {
        char num[64];
        if (ecm_ctl_decode_json(type, b, size, num, sizeof(num))) { rs(w, ",\"value\":"); rs(w, num); }
    }
    rf(w, ",\"ms\":%.1f}", (double)dt / 1e6);
    st->ok++;
    return rend(w);
}

static size_t refuse(ecm_ctl_state_t *st, rw_t *w, const req_t *r, const char *code, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    st->refused_writes++;
    return fail(st, w, r, code, "%s", msg);
}

static size_t cmd_sdo_write(ecm_ctl_state_t *st, const ecm_ctl_backend_t *be, const ecm_ctl_allowlist_t *al,
                            const req_t *r, rw_t *w)
{
    int slave, type;
    uint16_t index;
    uint8_t sub;
    const char *e = sdo_args(r, &slave, &index, &sub, &type, 1);
    if (e) return fail(st, w, r, "bad_request", "%s", e);
    if (type == ECM_CT_RAW) return fail(st, w, r, "bad_request", "sdo_write needs a numeric type, not raw");
    int vt = ecm_json_get(r->s, r->t, r->n, 0, "value");
    ecm_ctl_val_t val;
    if (vt < 0 || tok_val(r->s, &r->t[vt], type, &val)) return fail(st, w, r, "bad_request", "\"value\": a %s number required", ecm_ctl_type_name(type));
    uint32_t vendor, product;
    if (be->identity(be->ctx, slave, &vendor, &product)) return fail(st, w, r, "no_slave", "no slave %d", slave);
    char why[160] = "";
    if (!(ECM_CTL_BROKEN & BROKEN_DENY)) {
        if (comm_area(index))
            return refuse(st, w, r, "denied_comm_area", "0x%04X: the communication area 0x1000..0x1FFF is never written "
                          "through the API (PDO mapping/assignment, sync manager, sync)", index);
        if (be->deny && be->deny(be->ctx, slave, index, sub, why, sizeof(why)))
            return refuse(st, w, r, "denied_hard", "slave %d 0x%04X:%u: %s", slave, index, sub, why);
    }
    if (!(ECM_CTL_BROKEN & BROKEN_RXPDO)) {
        int m = be->rxpdo(be->ctx, slave, index, sub);
        if (m < 0) return refuse(st, w, r, "denied_no_pdo_table", "the PDO table is not known (no --eni / --pdo-scan): "
                                 "no write can be shown not to be in an RxPDO");
        if (m > 0) return refuse(st, w, r, "denied_rxpdo", "slave %d 0x%04X:%u is mapped in its RxPDO: the master "
                                 "writes it every cycle", slave, index, sub);
    }
    if (!(ECM_CTL_BROKEN & BROKEN_INITCMD) && be->initcmd && be->initcmd(be->ctx, slave, index, sub) > 0)
        return refuse(st, w, r, "denied_initcmd", "slave %d 0x%04X:%u is written by the ENI (CoE InitCmd): its value "
                      "is part of the configuration", slave, index, sub);
    const ecm_ctl_allow_t *a = allow_find(al, vendor, product, index, sub);
    if (!a) return refuse(st, w, r, "not_allowed", "slave %d (vendor 0x%08X product 0x%08X) 0x%04X:%u is not in the "
                          "allowlist%s%s", slave, vendor, product, index, sub, al && al->path[0] ? " " : " (none loaded)",
                          al && al->path[0] ? al->path : "");
    if (a->type != type) return refuse(st, w, r, "type_mismatch", "the allowlist gives %s for 0x%04X:%u",
                                       ecm_ctl_type_name(a->type), index, sub);
    if (ecm_ctl_val_fits(type, &val) ||
        (!(ECM_CTL_BROKEN & BROKEN_RANGE) && (ecm_ctl_val_cmp(type, &val, &a->min) < 0 || ecm_ctl_val_cmp(type, &val, &a->max) > 0))) {
        rw_t m = { why, 0, sizeof(why), 0 };
        rs(&m, "["); rval(&m, type, &a->min); rs(&m, ", "); rval(&m, type, &a->max); rs(&m, "]");
        return refuse(st, w, r, "out_of_range", "0x%04X:%u: value outside %s", index, sub, why);
    }
    if (be->ready(be->ctx, slave, why, sizeof(why))) return fail(st, w, r, "not_ready", "%s", why);
    uint8_t b[8];
    ecm_ctl_encode(type, &val, b);
    int size = ecm_ctl_type_size(type);
    uint32_t abort = 0;
    uint64_t t0 = mono_ns();
    int rc = be->sdo_write(be->ctx, slave, index, sub, b, size, &abort);
    st->sdo_writes++;
    if (rc) {
        uint64_t dt = mono_ns() - t0;
        if (dt > st->sdo_ns_max) st->sdo_ns_max = dt;
        return sdo_fail(st, w, r, be, rc, abort, slave, index, sub);
    }
    uint8_t rb[ECM_CTL_DATA_MAX];
    int rsize = sizeof(rb);
    uint32_t rabort = 0;
    int rrc = be->sdo_read(be->ctx, slave, index, sub, rb, &rsize, &rabort);
    uint64_t dt = mono_ns() - t0;
    if (dt > st->sdo_ns_max) st->sdo_ns_max = dt;
    st->sdo_reads++;
    sdo_head(w, r, slave, index, sub, type);
    rs(w, ",\"written\":"); rval(w, type, &val);
    rs(w, ",\"raw\":"); rhex(w, b, size);
    if (rrc) {
        rf(w, ",\"readback\":null,\"readback_ok\":false,\"readback_error\":\"%s\"",
           rrc == ECM_CTL_SDO_FAILED ? "sdo_failed" : rrc == ECM_CTL_SDO_TIMEOUT ? "timeout" : "busy");
    } else {
        char num[64] = "null";
        ecm_ctl_decode_json(type, rb, rsize, num, sizeof(num));
        int same = rsize == size && !memcmp(rb, b, (size_t)size);
        rs(w, ",\"readback\":"); rs(w, num);
        rf(w, ",\"readback_ok\":%s", same ? "true" : "false");
    }
    rf(w, ",\"ms\":%.1f}", (double)dt / 1e6);
    st->ok++;
    return rend(w);
}

static size_t cmd_info(ecm_ctl_state_t *st, const ecm_ctl_backend_t *be, const ecm_ctl_allowlist_t *al,
                       const req_t *r, rw_t *w)
{
    head(w, r, 1);
    rs(w, ",\"info\":{");
    if (be->info && w->len < w->cap) {
        size_t n = be->info(be->ctx, w->b + w->len, w->cap - w->len - 2);
        if (n >= w->cap - w->len - 2) w->ovf = 1;
        else { w->len += n; rs(w, ","); }
    }
    rf(w, "\"ctl\":{\"version\":%d,\"max_clients\":%d,\"rate_per_s\":%d,\"line_max\":%d,"
          "\"cmds\":%" PRIu64 ",\"errors\":%" PRIu64 ",\"rate_limited\":%" PRIu64 ",\"refused_writes\":%" PRIu64
          ",\"sdo_reads\":%" PRIu64 ",\"sdo_writes\":%" PRIu64 ",\"sdo_ms_max\":%.1f}",
       ECM_CTL_VERSION, ECM_CTL_MAX_CLIENTS, ECM_CTL_RATE, ECM_CTL_LINE_MAX,
       st->cmds, st->errors, st->rate_limited, st->refused_writes, st->sdo_reads, st->sdo_writes, (double)st->sdo_ns_max / 1e6);
    rs(w, ",\"allow\":{\"path\":");
    if (al && al->path[0]) rstr(w, al->path); else rs(w, "null");
    rs(w, ",\"entries\":[");
    for (int i = 0; al && i < al->n; i++) {
        const ecm_ctl_allow_t *e = &al->e[i];
        rf(w, "%s{\"vendor\":\"0x%08X\",\"product\":\"0x%08X\",\"index\":\"0x%04X\",\"sub\":%u,\"type\":\"%s\",\"min\":",
           i ? "," : "", e->vendor, e->product, e->index, e->sub, ecm_ctl_type_name(e->type));
        rval(w, e->type, &e->min);
        rs(w, ",\"max\":"); rval(w, e->type, &e->max);
        rs(w, ",\"note\":"); rstr(w, e->note);
        rs(w, "}");
    }
    rs(w, "]}}}");
    if (w->ovf) return fail(st, w, r, "too_large", "info does not fit in %d bytes", (int)w->cap);
    st->ok++;
    return rend(w);
}

static size_t cmd_dump(ecm_ctl_state_t *st, const ecm_ctl_backend_t *be, const req_t *r, rw_t *w)
{
    char m[1024];
    int rc = be->dump ? be->dump(be->ctx, m, sizeof(m)) : -1;
    if (rc < 0) return fail(st, w, r, "dump_failed", "%s", be->dump ? m : "not available");
    head(w, r, 1);
    rs(w, ",");
    rs(w, m);
    rs(w, "}");
    st->ok++;
    return rend(w);
}

size_t ecm_ctl_handle(ecm_ctl_state_t *st, const ecm_ctl_backend_t *be, const ecm_ctl_allowlist_t *al,
                      const char *line, size_t len, uint64_t now_ns, char *out, size_t cap)
{
    static req_t r;
    rw_t w = { out, 0, cap, 0 };
    memset(&r, 0, sizeof(r));
    snprintf(r.id, sizeof(r.id), "null");
    r.s = line;
    st->cmds++;

    if (!(ECM_CTL_BROKEN & BROKEN_RATE)) {
        if (st->last_ns) {
            double dt = (double)(now_ns - st->last_ns) * 1e-9;
            st->tokens += dt * ECM_CTL_RATE;
            if (st->tokens > ECM_CTL_RATE) st->tokens = ECM_CTL_RATE;
        }
        st->last_ns = now_ns;
    }
    int pos = 0, bad_id = 0;
    r.n = ecm_json_parse(line, len, r.t, 64, &pos);
    if (r.n > 0 && r.t[0].type == ECM_J_OBJ) {
        int iv = ecm_json_get(line, r.t, r.n, 0, "id");
        if (iv >= 0) {
            const ecm_jtok_t *k = &r.t[iv];
            int a = k->type == ECM_J_STR ? k->start - 1 : k->start, b = k->type == ECM_J_STR ? k->end + 1 : k->end;
            if ((k->type == ECM_J_NUM || k->type == ECM_J_STR || k->type == ECM_J_NULL) && b - a < (int)sizeof(r.id))
                snprintf(r.id, sizeof(r.id), "%.*s", b - a, line + a);
            else
                bad_id = 1;
        }
    }
    if (!(ECM_CTL_BROKEN & BROKEN_RATE)) {        /* every line costs a token, valid or not */
        if (st->tokens < 1.0) {
            st->rate_limited++;
            return fail(st, &w, &r, "rate_limited", "more than %d commands per second", ECM_CTL_RATE);
        }
        st->tokens -= 1.0;
    }
    if (bad_id) return fail(st, &w, &r, "bad_request", "\"id\": a number or a string of at most %d characters", (int)sizeof(r.id) - 3);
    if (r.n < 0) return fail(st, &w, &r, "bad_request", "%s at byte %d",
                             r.n == -2 ? "too many JSON tokens" : r.n == -3 ? "nested too deep" : "not JSON", pos);
    if (r.t[0].type != ECM_J_OBJ) return fail(st, &w, &r, "bad_request", "one JSON object per line");
    char cmd[24];
    int cv = ecm_json_get(line, r.t, r.n, 0, "cmd");
    if (cv < 0 || ecm_json_str(line, &r.t[cv], cmd, sizeof(cmd))) return fail(st, &w, &r, "bad_request", "\"cmd\" required");
    if (!strcmp(cmd, "info"))      return cmd_info(st, be, al, &r, &w);
    if (!strcmp(cmd, "sdo_read"))  return cmd_sdo_read(st, be, &r, &w);
    if (!strcmp(cmd, "sdo_write")) return cmd_sdo_write(st, be, al, &r, &w);
    if (!strcmp(cmd, "diag_dump")) return cmd_dump(st, be, &r, &w);
    return fail(st, &w, &r, "unknown_cmd", "%s: info | sdo_read | sdo_write | diag_dump", cmd);
}

/* ---------------------------------------------------------------- server --- */
int ecm_ctl_srv_open(ecm_ctl_srv_t *s, const char *path, unsigned mode, int gid, char *err, size_t errlen)
{
    memset(s, 0, sizeof(*s));
    s->lfd = -1;
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) s->c[i].fd = -1;
    ecm_ctl_state_init(&s->st);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    size_t pl = strlen(path);
    if (pl >= sizeof(a.sun_path)) { snprintf(err, errlen, "socket path longer than %zu bytes", sizeof(a.sun_path) - 1); return -1; }
    memcpy(a.sun_path, path, pl + 1);
    memcpy(s->path, path, pl + 1);
    struct stat sb;
    if (lstat(path, &sb) == 0) {
        if (!S_ISSOCK(sb.st_mode)) { snprintf(err, errlen, "%s exists and is not a socket", path); return -1; }
        int t = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (t >= 0 && connect(t, (struct sockaddr *)&a, sizeof(a)) == 0) {
            close(t);
            snprintf(err, errlen, "%s: another process serves this socket", path);
            return -1;
        }
        if (t >= 0) close(t);
        unlink(path);                                   /* stale: nobody listens */
    }
    s->lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s->lfd < 0) { snprintf(err, errlen, "socket: %s", strerror(errno)); return -1; }
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof(a)) || chmod(path, (mode_t)mode) ||
        (gid >= 0 && chown(path, (uid_t)-1, (gid_t)gid)) || listen(s->lfd, 8)) {
        snprintf(err, errlen, "%s: %s", s->path, strerror(errno));
        close(s->lfd);
        s->lfd = -1;
        unlink(path);
        return -1;
    }
    return 0;
}

static void client_close(ecm_ctl_client_t *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->inlen = c->outlen = c->outoff = 0;
    c->discard = 0;
}

int ecm_ctl_srv_clients(const ecm_ctl_srv_t *s)
{
    int n = 0;
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) n += s->c[i].fd >= 0;
    return n;
}

int ecm_ctl_srv_fds(const ecm_ctl_srv_t *s, struct pollfd *p, int max)
{
    int n = 0;
    if (s->lfd < 0 || max < 1) return 0;
    p[n++] = (struct pollfd){ .fd = s->lfd, .events = POLLIN };
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS && n < max; i++) {
        const ecm_ctl_client_t *c = &s->c[i];
        if (c->fd < 0) continue;
        short ev = c->inlen < sizeof(c->in) ? POLLIN : 0;
        if (c->outlen > c->outoff) ev |= POLLOUT;
        p[n++] = (struct pollfd){ .fd = c->fd, .events = ev };
    }
    return n;
}

static int has_line(const ecm_ctl_client_t *c)
{
    return c->fd >= 0 && (memchr(c->in, '\n', c->inlen) != NULL || c->inlen == sizeof(c->in));
}

int ecm_ctl_srv_pending(const ecm_ctl_srv_t *s)
{
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) if (has_line(&s->c[i])) return 1;
    return 0;
}

static void client_send(ecm_ctl_srv_t *s, ecm_ctl_client_t *c)
{
    while (c->fd >= 0 && c->outlen > c->outoff) {
        ssize_t w = send(c->fd, c->out + c->outoff, c->outlen - c->outoff, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            client_close(c);
            return;
        }
        c->outoff += (size_t)w;
    }
    if (c->outoff == c->outlen) c->outoff = c->outlen = 0;
    (void)s;
}

static void client_queue(ecm_ctl_srv_t *s, ecm_ctl_client_t *c, const char *b, size_t n)
{
    if (c->outoff && c->outlen + n > sizeof(c->out)) {          /* compact */
        memmove(c->out, c->out + c->outoff, c->outlen - c->outoff);
        c->outlen -= c->outoff;
        c->outoff = 0;
    }
    if (c->outlen + n > sizeof(c->out)) {                     /* never reads its replies */
        s->dropped_slow++;
        client_close(c);
        return;
    }
    memcpy(c->out + c->outlen, b, n);
    c->outlen += n;
}

static void client_read(ecm_ctl_srv_t *s, ecm_ctl_client_t *c)
{
    for (;;) {
        if (c->inlen == sizeof(c->in)) return;                /* full: processed first */
        ssize_t r = recv(c->fd, c->in + c->inlen, sizeof(c->in) - c->inlen, MSG_DONTWAIT);
        if (r == 0) { client_close(c); return; }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            client_close(c);
            return;
        }
        if (c->discard) {                                     /* rest of an over-long line */
            char *nl = memchr(c->in + c->inlen, '\n', (size_t)r);
            if (!nl) continue;                                /* all dropped: inlen unchanged */
            size_t keep = (size_t)r - (size_t)(nl + 1 - (c->in + c->inlen));
            memmove(c->in, nl + 1, keep);
            c->inlen = keep;
            c->discard = 0;
            continue;
        }
        c->inlen += (size_t)r;
    }
    (void)s;
}

int ecm_ctl_srv_service(ecm_ctl_srv_t *s, const struct pollfd *p, int np, const ecm_ctl_backend_t *be,
                        const ecm_ctl_allowlist_t *al, uint64_t now_ns, int max_cmds)
{
    static char reply[ECM_CTL_REPLY_MAX];
    int run = 0;
    if (s->lfd < 0) return 0;
    /* accept */
    if (np > 0 && p[0].fd == s->lfd && (p[0].revents & POLLIN)) {
        for (;;) {
            int fd = accept4(s->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            int slot = -1;
            for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) if (s->c[i].fd < 0) { slot = i; break; }
            if (slot < 0) {
                static const char busy[] = "{\"id\":null,\"ok\":false,\"error\":\"too_many_clients\","
                                           "\"msg\":\"at most 4 clients\"}\n";
                if (send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) { }
                close(fd);
                s->refused_clients++;
                continue;
            }
            ecm_ctl_client_t *c = &s->c[slot];
            c->fd = fd;
            c->inlen = c->outlen = c->outoff = 0;
            c->discard = 0;
            s->accepted++;
        }
    }
    /* read what poll() reported */
    for (int k = 1; k < np; k++) {
        if (!p[k].revents) continue;
        for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) {
            ecm_ctl_client_t *c = &s->c[i];
            if (c->fd != p[k].fd) continue;
            if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) client_read(s, c);
            break;
        }
    }
    /* one request per client per call, at most max_cmds in all */
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS && run < max_cmds; i++) {
        ecm_ctl_client_t *c = &s->c[i];
        if (c->fd < 0 || !c->inlen) continue;
        char *nl = memchr(c->in, '\n', c->inlen);
        if (!nl) {
            if (c->inlen < sizeof(c->in)) continue;
            static const char lng[] = "{\"id\":null,\"ok\":false,\"error\":\"line_too_long\","
                                      "\"msg\":\"a request is one line of at most 1023 bytes\"}\n";
            s->dropped_long++;
            s->st.cmds++;
            s->st.errors++;
            c->inlen = 0;
            c->discard = 1;
            client_queue(s, c, lng, sizeof(lng) - 1);
            run++;
            continue;
        }
        size_t l = (size_t)(nl - c->in);
        size_t ll = l;
        if (ll && c->in[ll - 1] == '\r') ll--;
        size_t n = ecm_ctl_handle(&s->st, be, al, c->in, ll, now_ns, reply, sizeof(reply));
        memmove(c->in, nl + 1, c->inlen - l - 1);
        c->inlen -= l + 1;
        if (n) client_queue(s, c, reply, n);
        run++;
    }
    /* send */
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) if (s->c[i].fd >= 0 && s->c[i].outlen > s->c[i].outoff) client_send(s, &s->c[i]);
    return run;
}

void ecm_ctl_srv_close(ecm_ctl_srv_t *s)
{
    for (int i = 0; i < ECM_CTL_MAX_CLIENTS; i++) client_close(&s->c[i]);
    if (s->lfd >= 0) {
        close(s->lfd);
        s->lfd = -1;
        unlink(s->path);
    }
}
