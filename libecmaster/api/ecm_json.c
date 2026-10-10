/* ecm_json.c -- Phase 11.2, see ecm_json.h */
#include "ecm_json.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t      len, pos;
    ecm_jtok_t *t;
    int         max, n, err;
} jp_t;

static void ws(jp_t *p)
{
    while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' ||
                               p->s[p->pos] == '\n' || p->s[p->pos] == '\r'))
        p->pos++;
}

static int tok(jp_t *p, int type, int start, int parent)
{
    if (p->n >= p->max) { p->err = -2; return -1; }
    ecm_jtok_t *k = &p->t[p->n];
    k->type = (uint8_t)type; k->start = start; k->end = start; k->size = 0; k->parent = parent;
    return p->n++;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int value(jp_t *p, int parent, int depth);

static int string(jp_t *p, int parent)
{
    /* at the opening quote */
    int k = tok(p, ECM_J_STR, (int)p->pos + 1, parent);
    if (k < 0) return -1;
    p->pos++;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == '"') { p->t[k].end = (int)p->pos; p->pos++; return k; }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (++p->pos >= p->len) return -1;
            char e = p->s[p->pos];
            if (e == 'u') {
                if (p->pos + 4 >= p->len) return -1;
                for (int i = 1; i <= 4; i++) if (!is_hex(p->s[p->pos + i])) return -1;
                p->pos += 4;
            } else if (!strchr("\"\\/bfnrt", e)) {
                return -1;
            }
        }
        p->pos++;
    }
    return -1;
}

static int number(jp_t *p, int parent)
{
    size_t i = p->pos;
    int k = tok(p, ECM_J_NUM, (int)i, parent);
    if (k < 0) return -1;
    if (i < p->len && p->s[i] == '-') i++;
    if (i >= p->len) return -1;
    if (p->s[i] == '0') i++;
    else if (p->s[i] >= '1' && p->s[i] <= '9') { while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') i++; }
    else return -1;
    if (i < p->len && p->s[i] == '.') {
        i++;
        if (i >= p->len || p->s[i] < '0' || p->s[i] > '9') return -1;
        while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    if (i < p->len && (p->s[i] == 'e' || p->s[i] == 'E')) {
        i++;
        if (i < p->len && (p->s[i] == '+' || p->s[i] == '-')) i++;
        if (i >= p->len || p->s[i] < '0' || p->s[i] > '9') return -1;
        while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') i++;
    }
    p->t[k].end = (int)i;
    p->pos = i;
    return k;
}

static int literal(jp_t *p, int parent, const char *w, int type)
{
    size_t l = strlen(w);
    if (p->pos + l > p->len || memcmp(p->s + p->pos, w, l)) return -1;
    int k = tok(p, type, (int)p->pos, parent);
    if (k < 0) return -1;
    p->pos += l;
    p->t[k].end = (int)p->pos;
    return k;
}

static int container(jp_t *p, int parent, int depth, int obj)
{
    if (depth >= ECM_JSON_MAX_DEPTH) { p->err = -3; return -1; }
    int k = tok(p, obj ? ECM_J_OBJ : ECM_J_ARR, (int)p->pos, parent);
    if (k < 0) return -1;
    p->pos++;
    ws(p);
    char close = obj ? '}' : ']';
    if (p->pos < p->len && p->s[p->pos] == close) { p->pos++; p->t[k].end = (int)p->pos; return k; }
    for (;;) {
        ws(p);
        if (obj) {
            if (p->pos >= p->len || p->s[p->pos] != '"') return -1;
            if (string(p, k) < 0) return -1;
            p->t[k].size++;
            ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') return -1;
            p->pos++;
            ws(p);
        }
        if (value(p, k, depth + 1) < 0) return -1;
        p->t[k].size++;
        ws(p);
        if (p->pos >= p->len) return -1;
        if (p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->s[p->pos] == close) { p->pos++; p->t[k].end = (int)p->pos; return k; }
        return -1;
    }
}

static int value(jp_t *p, int parent, int depth)
{
    ws(p);
    if (p->pos >= p->len) return -1;
    char c = p->s[p->pos];
    if (c == '{') return container(p, parent, depth, 1);
    if (c == '[') return container(p, parent, depth, 0);
    if (c == '"') return string(p, parent);
    if (c == '-' || (c >= '0' && c <= '9')) return number(p, parent);
    if (c == 't') return literal(p, parent, "true", ECM_J_TRUE);
    if (c == 'f') return literal(p, parent, "false", ECM_J_FALSE);
    if (c == 'n') return literal(p, parent, "null", ECM_J_NULL);
    return -1;
}

int ecm_json_parse(const char *s, size_t len, ecm_jtok_t *t, int max, int *errpos)
{
    jp_t p = { .s = s, .len = len, .t = t, .max = max };
    int r = value(&p, -1, 0);
    if (r >= 0) {
        ws(&p);
        if (p.pos != p.len) r = -1;
    }
    if (r < 0) {
        if (errpos) *errpos = (int)p.pos;
        return p.err ? p.err : -1;
    }
    return p.n;
}

int ecm_json_skip(const ecm_jtok_t *t, int n, int i)
{
    int j = i + 1;
    while (j < n) {
        int a = t[j].parent;
        while (a >= 0 && a != i) a = t[a].parent;
        if (a != i) break;
        j++;
    }
    return j;
}

int ecm_json_get(const char *s, const ecm_jtok_t *t, int n, int obj, const char *key)
{
    if (obj < 0 || obj >= n || t[obj].type != ECM_J_OBJ) return -1;
    size_t kl = strlen(key);
    int i = obj + 1;
    for (int m = 0; m < t[obj].size / 2 && i < n; m++) {
        int v = i + 1;
        if (v >= n) return -1;
        if (t[i].type == ECM_J_STR && (size_t)(t[i].end - t[i].start) == kl && !memcmp(s + t[i].start, key, kl))
            return v;
        i = ecm_json_skip(t, n, v);
    }
    return -1;
}

int ecm_json_str(const char *s, const ecm_jtok_t *tk, char *out, size_t cap)
{
    if (tk->type != ECM_J_STR || !cap) return -1;
    size_t o = 0;
    for (int i = tk->start; i < tk->end; i++) {
        char c = s[i];
        if (c == '\\') {
            char e = s[++i];
            switch (e) {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                char h[5] = { s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0 };
                long u = strtol(h, NULL, 16);
                c = u > 0 && u < 0x80 ? (char)u : '?';
                i += 4;
                break;
            }
            default: c = e; break;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

/* digits of an integer token or integer string, in a NUL-terminated copy */
static int int_text(const char *s, const ecm_jtok_t *tk, char *b, size_t cap)
{
    if (tk->type != ECM_J_NUM && tk->type != ECM_J_STR) return -1;
    int l = tk->end - tk->start;
    if (l <= 0 || (size_t)l >= cap) return -1;
    memcpy(b, s + tk->start, (size_t)l);
    b[l] = '\0';
    if (tk->type == ECM_J_NUM && strpbrk(b, ".eE")) return -1;
    return 0;
}

int ecm_json_i64(const char *s, const ecm_jtok_t *tk, int64_t *v)
{
    char b[40], *e;
    if (int_text(s, tk, b, sizeof(b))) return -1;
    errno = 0;
    long long x = strtoll(b, &e, (tk->type == ECM_J_STR) ? 0 : 10);
    if (errno || *e || e == b) return -1;
    if (tk->type == ECM_J_STR && b[0] == '0' && b[1] >= '0' && b[1] <= '9') return -1;   /* no octal surprises */
    *v = x;
    return 0;
}

int ecm_json_u64(const char *s, const ecm_jtok_t *tk, uint64_t *v)
{
    char b[40], *e;
    if (int_text(s, tk, b, sizeof(b))) return -1;
    if (b[0] == '-') return -1;
    if (tk->type == ECM_J_STR && b[0] == '0' && b[1] >= '0' && b[1] <= '9') return -1;
    errno = 0;
    unsigned long long x = strtoull(b, &e, (tk->type == ECM_J_STR) ? 0 : 10);
    if (errno || *e || e == b) return -1;
    *v = x;
    return 0;
}

int ecm_json_f64(const char *s, const ecm_jtok_t *tk, double *v)
{
    if (tk->type == ECM_J_STR) {
        int64_t i;
        if (ecm_json_i64(s, tk, &i)) return -1;
        *v = (double)i;
        return 0;
    }
    if (tk->type != ECM_J_NUM) return -1;
    char b[64], *e;
    int l = tk->end - tk->start;
    if (l <= 0 || (size_t)l >= sizeof(b)) return -1;
    memcpy(b, s + tk->start, (size_t)l);
    b[l] = '\0';
    errno = 0;
    double x = strtod(b, &e);
    if (errno || *e || !isfinite(x)) return -1;
    *v = x;
    return 0;
}
