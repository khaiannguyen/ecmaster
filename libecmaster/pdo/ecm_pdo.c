/*
 * ecm_pdo.c -- GD9.10 PDO entry table and bind (no SOEM dependency).
 */
#include "ecm_pdo.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void adderr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen) return;
    size_t u = strlen(err);
    if (u + 1 >= errlen) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + u, errlen - u, fmt, ap);
    va_end(ap);
}

void ecm_pdo_table_init(ecm_pdo_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

int ecm_pdo_add(ecm_pdo_table_t *t, int slave, ecm_pdo_dir_t dir, uint16_t pdo,
                uint16_t index, uint8_t sub, uint16_t bits)
{
    if (t->n >= ECM_PDO_MAX_ENTRIES || slave < 1 || slave > ECM_PDO_MAX_SLAVES ||
        (dir != ECM_PDO_OUT && dir != ECM_PDO_IN) || bits == 0)
        return -1;
    ecm_pdo_entry_t *e = &t->e[t->n++];
    e->slave = (uint16_t)slave;
    e->dir = (uint8_t)dir;
    e->pdo = pdo;
    e->index = index;
    e->sub = sub;
    e->bits = bits;
    e->bit_off = t->bits[dir][slave];
    t->bits[dir][slave] += bits;
    return 0;
}

int ecm_pdo_table_compare(const ecm_pdo_table_t *a, const ecm_pdo_table_t *b,
                          char *err, size_t errlen)
{
    int bad = 0;
    if (err && errlen) err[0] = '\0';
    int n = a->n < b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        const ecm_pdo_entry_t *x = &a->e[i], *y = &b->e[i];
        if (x->slave != y->slave || x->dir != y->dir || x->pdo != y->pdo || x->index != y->index ||
            x->sub != y->sub || x->bits != y->bits || x->bit_off != y->bit_off) {
            adderr(err, errlen, "entry %d: slave %u %s PDO 0x%04X 0x%04X:%02X %u bit @%u  vs  "
                   "slave %u %s PDO 0x%04X 0x%04X:%02X %u bit @%u\n", i,
                   x->slave, x->dir ? "in" : "out", x->pdo, x->index, x->sub, x->bits, x->bit_off,
                   y->slave, y->dir ? "in" : "out", y->pdo, y->index, y->sub, y->bits, y->bit_off);
            bad++;
        }
    }
    if (a->n != b->n) {
        adderr(err, errlen, "entry count %d vs %d\n", a->n, b->n);
        bad++;
    }
    return bad;
}

int ecm_pdo_check_sizes(const ecm_pdo_table_t *t, int n, const uint32_t *obits,
                        const uint32_t *ibits, char *err, size_t errlen)
{
    int bad = 0;
    if (err && errlen) err[0] = '\0';
    for (int s = 1; s <= n && s <= ECM_PDO_MAX_SLAVES; s++) {
        if (t->bits[ECM_PDO_OUT][s] != obits[s] || t->bits[ECM_PDO_IN][s] != ibits[s]) {
            adderr(err, errlen, "slave %d: PDO table %u out / %u in bits, mapped %u / %u\n", s,
                   t->bits[ECM_PDO_OUT][s], t->bits[ECM_PDO_IN][s], obits[s], ibits[s]);
            bad++;
        }
    }
    return bad;
}

int ecm_pdo_bind(const ecm_pdo_table_t *t, const ecm_pdo_loc_t *loc, int slave,
                 uint16_t index, uint8_t sub, ecm_pdo_handle_t *h, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (index == 0) {
        adderr(err, errlen, "slave %d: index 0 is padding, not an object", slave);
        return -1;
    }
    for (int i = 0; i < t->n; i++) {
        const ecm_pdo_entry_t *e = &t->e[i];
        if (e->slave != slave || e->index != index || e->sub != sub) continue;
        if (e->bits > 64) {
            adderr(err, errlen, "slave %d 0x%04X:%02X: %u bit, more than 64", slave, index, sub, e->bits);
            return -1;
        }
        h->slave = (uint16_t)slave;
        h->group = loc[slave].group;
        h->dir = e->dir;
        h->bit = (e->dir == ECM_PDO_OUT ? loc[slave].out_bit : loc[slave].in_bit) + e->bit_off;
        h->bits = e->bits;
        return 0;
    }
    adderr(err, errlen, "slave %d does not map 0x%04X:%02X; it maps:", slave, index, sub);
    int any = 0;
    for (int i = 0; i < t->n; i++) {
        const ecm_pdo_entry_t *e = &t->e[i];
        if (e->slave != slave || !e->index) continue;
        adderr(err, errlen, " %s 0x%04X:%02X", e->dir ? "in" : "out", e->index, e->sub);
        any = 1;
    }
    if (!any) adderr(err, errlen, " nothing");
    return -1;
}

uint64_t ecm_pdo_get(const ecm_pdo_handle_t *h, const uint8_t *iomap)
{
    uint64_t v = 0;
    uint32_t bit = h->bit;
    for (int k = 0; k < h->bits;) {
        uint32_t byte = bit >> 3, sh = bit & 7;
        int take = 8 - (int)sh;
        if (take > h->bits - k) take = h->bits - k;
        uint64_t part = ((uint64_t)iomap[byte] >> sh) & ((1u << take) - 1u);
        v |= part << k;
        k += take;
        bit += (uint32_t)take;
    }
    return v;
}

void ecm_pdo_set(const ecm_pdo_handle_t *h, uint8_t *iomap, uint64_t v)
{
    uint32_t bit = h->bit;
    for (int k = 0; k < h->bits;) {
        uint32_t byte = bit >> 3, sh = bit & 7;
        int take = 8 - (int)sh;
        if (take > h->bits - k) take = h->bits - k;
        uint8_t mask = (uint8_t)(((1u << take) - 1u) << sh);
        uint8_t part = (uint8_t)(((v >> k) & ((1u << take) - 1u)) << sh);
        iomap[byte] = (uint8_t)((iomap[byte] & ~mask) | part);
        k += take;
        bit += (uint32_t)take;
    }
}

int ecm_pdo_parse_ref(const char *s, int *slave, uint16_t *index, uint8_t *sub)
{
    char *end;
    long sl = strtol(s, &end, 0);
    if (end == s || *end != ':' || sl < 1 || sl > ECM_PDO_MAX_SLAVES) return -1;
    const char *p = end + 1;
    unsigned long ix = strtoul(p, &end, 0);
    if (end == p || *end != ':' || ix == 0 || ix > 0xFFFF) return -1;
    p = end + 1;
    unsigned long sb = strtoul(p, &end, 0);
    if (end == p || *end || sb > 0xFF) return -1;
    *slave = (int)sl;
    *index = (uint16_t)ix;
    *sub = (uint8_t)sb;
    return 0;
}

int ecm_pdo_scan_required(const uint32_t *vendor, int n, const uint16_t *bound, int nbound,
                          const uint32_t *own, int nown, char *err, size_t errlen)
{
    uint8_t seen[ECM_PDO_MAX_SLAVES + 1] = { 0 };
    size_t len = 0;
    int cnt = 0;
    if (err && errlen) err[0] = '\0';
    for (int i = 0; i < nbound; i++) {
        int s = bound[i];
        if (s < 1 || s > n || s > ECM_PDO_MAX_SLAVES || seen[s]) continue;
        seen[s] = 1;
        int mine = 0;
        for (int k = 0; k < nown; k++)
            if (vendor[s] == own[k]) { mine = 1; break; }
        if (mine) continue;
        cnt++;
        if (err && len + 1 < errlen) {
            int r = snprintf(err + len, errlen - len, "%sslave %d (vendor 0x%08X)",
                             len ? ", " : "", s, (unsigned)vendor[s]);
            if (r > 0) len += (size_t)r;
            if (len >= errlen) len = errlen - 1;
        }
    }
    return cnt;
}

size_t ecm_pdo_table_format(const ecm_pdo_table_t *t, char *buf, size_t cap)
{
    size_t len = 0;
    if (!cap) return 0;
    buf[0] = '\0';
    for (int i = 0; i < t->n && len + 1 < cap; i++) {
        const ecm_pdo_entry_t *e = &t->e[i];
        int r = snprintf(buf + len, cap - len, "pdo slave %u %-3s PDO 0x%04X 0x%04X:%02X %2u bit @%u\n",
                         e->slave, e->dir ? "in" : "out", e->pdo, e->index, e->sub, e->bits, e->bit_off);
        if (r < 0) break;
        len += (size_t)r;
        if (len >= cap) { len = cap - 1; break; }
    }
    return len;
}
