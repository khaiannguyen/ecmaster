/* ecm_cia402_cfg.c — Phase 10.3, see ecm_cia402_cfg.h */
#include "ecm_cia402_cfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; uint32_t bit; } MODES[] = {
    { "pp", ECM_MODE_PP }, { "pv", ECM_MODE_PV }, { "hm", ECM_MODE_HM },
    { "csp", ECM_MODE_CSP }, { "csv", ECM_MODE_CSV }, { "cst", ECM_MODE_CST },
};
#define NMODES (sizeof(MODES) / sizeof(MODES[0]))

size_t ecm_axis_modes_str(uint32_t modes, char *buf, size_t cap)
{
    size_t len = 0;
    if (!cap) return 0;
    buf[0] = '\0';
    for (size_t k = 0; k < NMODES; k++) {
        if (!(modes & MODES[k].bit)) continue;
        int r = snprintf(buf + len, cap - len, "%s", len ? "," : "");
        if (r > 0) len += (size_t)r;
        for (const char *p = MODES[k].name; *p && len + 1 < cap; p++) buf[len++] = (char)toupper((unsigned char)*p);
        buf[len < cap ? len : cap - 1] = '\0';
        if (len + 1 >= cap) break;
    }
    return len;
}

int ecm_axis_parse_modes(const char *s, uint32_t *modes, char *err, size_t errlen)
{
    char tmp[128], *save = NULL;
    uint32_t m = 0;
    snprintf(tmp, sizeof(tmp), "%s", s ? s : "");
    for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        size_t k;
        for (k = 0; k < NMODES; k++) if (!strcasecmp(t, MODES[k].name)) break;
        if (k == NMODES) {
            snprintf(err, errlen, "unknown mode '%s' (pp, pv, hm, csp, csv, cst)", t);
            return -1;
        }
        m |= MODES[k].bit;
    }
    if (!m) { snprintf(err, errlen, "no mode given"); return -1; }
    *modes = m;
    return 0;
}

static int add_axis(ecm_axis_cfg_t *ax, int *nax, int max, long slave, long n, uint32_t modes,
                    const char *name, char *err, size_t errlen)
{
    if (slave < 1 || slave > ECM_PDO_MAX_SLAVES || n < 0 || n > 7) {
        snprintf(err, errlen, "axis %ld:%ld: slave 1..%d, axis 0..7", slave, n, ECM_PDO_MAX_SLAVES);
        return -1;
    }
    for (int k = 0; k < *nax; k++)
        if (ax[k].slave == slave && ax[k].n == n) {
            snprintf(err, errlen, "axis %ld:%ld given twice", slave, n);
            return -1;
        }
    if (*nax >= max) { snprintf(err, errlen, "more than %d axes", max); return -1; }
    ecm_axis_cfg_t *a = &ax[(*nax)++];
    memset(a, 0, sizeof(*a));
    a->slave = (uint16_t)slave;
    a->n = (uint8_t)n;
    a->modes = modes;
    if (name && *name) snprintf(a->name, sizeof(a->name), "%s", name);
    else snprintf(a->name, sizeof(a->name), "%ld:%ld", slave, n);
    return 0;
}

int ecm_axis_parse_list(const char *s, uint32_t modes, ecm_axis_cfg_t *ax, int *nax, int max,
                        char *err, size_t errlen)
{
    char tmp[256], *save = NULL;
    snprintf(tmp, sizeof(tmp), "%s", s ? s : "");
    int any = 0;
    for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char *end;
        long sl = strtol(t, &end, 0), n = 0;
        if (end == t) goto bad;
        if (*end == ':') {
            char *p = end + 1;
            n = strtol(p, &end, 0);
            if (end == p) goto bad;
        }
        if (*end) goto bad;
        if (add_axis(ax, nax, max, sl, n, modes, NULL, err, errlen)) return -1;
        any = 1;
        continue;
bad:
        snprintf(err, errlen, "axis '%s': expected SLAVE[:AXIS]", t);
        return -1;
    }
    if (!any) { snprintf(err, errlen, "no axis given"); return -1; }
    return 0;
}

int ecm_axis_load_cfg(const char *path, ecm_axis_cfg_t *ax, int *nax, int max, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "%s: cannot open", path); return -1; }
    char line[256], e2[160];
    int ln = 0, rc = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = '\0';
        char *tok[8], *save = NULL;
        int nt = 0;
        for (char *t = strtok_r(line, " \t\r\n", &save); t && nt < 8; t = strtok_r(NULL, " \t\r\n", &save))
            tok[nt++] = t;
        if (!nt) continue;
        const char *ref = NULL, *mstr = NULL, *name = NULL;
        if (strcmp(tok[0], "axis") || nt < 4) goto bad;
        ref = tok[1];
        for (int k = 2; k + 1 < nt; k += 2) {
            if (!strcmp(tok[k], "modes")) mstr = tok[k + 1];
            else if (!strcmp(tok[k], "name")) name = tok[k + 1];
            else goto bad;
        }
        if (!mstr) goto bad;
        uint32_t m;
        if (ecm_axis_parse_modes(mstr, &m, e2, sizeof(e2))) {
            snprintf(err, errlen, "%s:%d: %s", path, ln, e2);
            rc = -1; break;
        }
        int before = *nax;
        if (ecm_axis_parse_list(ref, m, ax, nax, max, e2, sizeof(e2)) || *nax != before + 1) {
            snprintf(err, errlen, "%s:%d: %s", path, ln, *nax != before + 1 && !e2[0] ? "one axis per line" : e2);
            rc = -1; break;
        }
        if (name) snprintf(ax[*nax - 1].name, sizeof(ax[0].name), "%s", name);
        continue;
bad:
        snprintf(err, errlen, "%s:%d: expected 'axis SLAVE[:AXIS] modes LIST [name NAME]'", path, ln);
        rc = -1;
        break;
    }
    fclose(f);
    if (!rc && *nax == 0) { snprintf(err, errlen, "%s: no axis", path); rc = -1; }
    return rc;
}

uint16_t ecm_axis_index(const ecm_axis_cfg_t *a, uint16_t base)
{
    return (uint16_t)(base + ECM_AXIS_OFFSET * a->n);
}

/* one object: bind it, check its direction */
static int need(const ecm_axis_cfg_t *a, const ecm_pdo_table_t *t, const ecm_pdo_loc_t *loc,
                uint16_t base, int dir, const char *why, ecm_pdo_handle_t *h, int optional,
                char *err, size_t errlen)
{
    char e2[512];
    uint16_t idx = ecm_axis_index(a, base);
    memset(h, 0, sizeof(*h));
    if (ecm_pdo_bind(t, loc, a->slave, idx, 0, h, e2, sizeof(e2)) != 0) {
        memset(h, 0, sizeof(*h));
        if (optional) return 0;
        snprintf(err, errlen, "axis %s (slave %u): %s needs 0x%04X:00 (%s) in the process data: %s",
                 a->name, a->slave, why, idx, dir ? "input" : "output", e2);
        return -1;
    }
    if (h->dir != dir) {
        snprintf(err, errlen, "axis %s (slave %u): 0x%04X:00 is mapped as an %s, %s needs it as an %s",
                 a->name, a->slave, idx, h->dir ? "input" : "output", why, dir ? "input" : "output");
        memset(h, 0, sizeof(*h));
        return -1;
    }
    return 0;
}

int ecm_axis_bind(const ecm_axis_cfg_t *a, const ecm_pdo_table_t *t, const ecm_pdo_loc_t *loc,
                  ecm_axis_bind_t *b, char *err, size_t errlen)
{
    memset(b, 0, sizeof(*b));
    uint32_t m = a->modes;
    if (!m) { snprintf(err, errlen, "axis %s: no mode", a->name); return -1; }
    if (m & ~ECM_MODE_KNOWN) { snprintf(err, errlen, "axis %s: mode not handled by the master", a->name); return -1; }
    char mm[64];
    ecm_axis_modes_str(m, mm, sizeof(mm));
    if (need(a, t, loc, 0x6040, ECM_PDO_OUT, "every mode", &b->cw, 0, err, errlen)) return -1;
    if (need(a, t, loc, 0x6041, ECM_PDO_IN, "every mode", &b->sw, 0, err, errlen)) return -1;
    if (m & (ECM_MODE_CSP | ECM_MODE_PP)) {
        const char *w = (m & ECM_MODE_CSP) ? "CSP" : "PP";
        if (need(a, t, loc, 0x607A, ECM_PDO_OUT, w, &b->tpos, 0, err, errlen)) return -1;
    }
    if (m & (ECM_MODE_CSP | ECM_MODE_PP | ECM_MODE_HM)) {
        const char *w = (m & ECM_MODE_CSP) ? "CSP" : (m & ECM_MODE_PP) ? "PP" : "HM";
        if (need(a, t, loc, 0x6064, ECM_PDO_IN, w, &b->apos, 0, err, errlen)) return -1;
    }
    if (m & (ECM_MODE_CSV | ECM_MODE_PV)) {
        const char *w = (m & ECM_MODE_CSV) ? "CSV" : "PV";
        if (need(a, t, loc, 0x60FF, ECM_PDO_OUT, w, &b->tvel, 0, err, errlen)) return -1;
        if (need(a, t, loc, 0x606C, ECM_PDO_IN, w, &b->avel, 0, err, errlen)) return -1;
    }
    if (m & ECM_MODE_CST) {
        if (need(a, t, loc, 0x6071, ECM_PDO_OUT, "CST", &b->ttq, 0, err, errlen)) return -1;
        if (need(a, t, loc, 0x6077, ECM_PDO_IN, "CST", &b->atq, 0, err, errlen)) return -1;
    }
    int several = (m & (m - 1)) != 0;
    char why[96];
    snprintf(why, sizeof(why), "changing mode at run time (%s)", mm);
    if (need(a, t, loc, 0x6060, ECM_PDO_OUT, why, &b->mode, !several, err, errlen)) return -1;
    if (need(a, t, loc, 0x6061, ECM_PDO_IN, why, &b->mode_disp, !several, err, errlen)) return -1;
    b->mode_by_sdo = b->mode.bits == 0;
    need(a, t, loc, 0x603F, ECM_PDO_IN, "error code", &b->err, 1, err, errlen);
    /* Phase 10.5: actual position / velocity whenever the slave maps them,
     * even if the modes do not need them (state of a CSV-only axis) */
    if (!b->apos.bits) need(a, t, loc, 0x6064, ECM_PDO_IN, "position actual", &b->apos, 1, err, errlen);
    if (!b->avel.bits) need(a, t, loc, 0x606C, ECM_PDO_IN, "velocity actual", &b->avel, 1, err, errlen);
    return 0;
}

int ecm_axis_check_modes(const ecm_axis_cfg_t *a, uint32_t supported, char *err, size_t errlen)
{
    uint32_t miss = a->modes & ~supported;
    if (!miss) return 0;
    char ms[64], ss[64];
    ecm_axis_modes_str(miss, ms, sizeof(ms));
    ecm_axis_modes_str(supported & ECM_MODE_KNOWN, ss, sizeof(ss));
    snprintf(err, errlen, "axis %s (slave %u): drive does not support %s (0x%04X:00 supported drive modes = 0x%08X: %s)",
             a->name, a->slave, ms, ecm_axis_index(a, 0x6502), (unsigned)supported, ss[0] ? ss : "none the master knows");
    return -1;
}
