/* ecm_cia402_diag.c — Phase 10.8, see ecm_cia402_diag.h */
#include "ecm_cia402_diag.h"
#include "../libecmaster/diag/ecm_diag.h"     /* ecm_emcy_class_name (CiA 301 classes) */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CiA 402 (IEC 61800-7-201) drive error codes and the ETG.1000.6 EtherCAT
 * ones a drive sends; anything else falls back to its CiA 301 class. */
static const struct { uint16_t code; const char *text; } STD[] = {
    { 0x2310, "continuous over current" },
    { 0x2320, "short circuit / earth leakage (output side)" },
    { 0x2330, "earth leakage (output side)" },
    { 0x3110, "mains over-voltage" },
    { 0x3120, "mains under-voltage" },
    { 0x3130, "phase failure" },
    { 0x3210, "DC link over-voltage" },
    { 0x3220, "DC link under-voltage" },
    { 0x4210, "excess temperature device" },
    { 0x4310, "excess temperature drive" },
    { 0x6320, "parameter error" },
    { 0x7121, "motor blocked" },
    { 0x7122, "motor error or commutation malfunction" },
    { 0x7305, "incremental sensor 1 fault" },
    { 0x8311, "excess torque" },
    { 0x8611, "following error" },
    { 0x8612, "reference limit" },
    { 0x8700, "sync controller error (EtherCAT sync / communication)" },
    { 0xA000, "transition PREOP -> SAFEOP failed (ETG.1000.6)" },
    { 0xA001, "transition SAFEOP -> OP failed (ETG.1000.6)" },
};

static struct { uint32_t vendor; uint16_t code; char text[72]; } VT[ECM_EMCY_VENDOR_MAX];
static int NVT;

void ecm_cia402_emcy_clear(void) { NVT = 0; }
int  ecm_cia402_emcy_count(void) { return NVT; }

const char *ecm_cia402_emcy_text(uint16_t code, uint32_t vendor)
{
    for (int i = 0; i < NVT; i++)
        if (VT[i].vendor == vendor && VT[i].code == code) return VT[i].text;
    for (size_t i = 0; i < sizeof(STD) / sizeof(STD[0]); i++)
        if (STD[i].code == code) return STD[i].text;
    return NULL;
}

int ecm_cia402_emcy_load(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "%s: cannot open", path); return -1; }
    char line[256];
    int ln = 0, have_vendor = 0, rc = 0;
    uint32_t vendor = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        size_t n = strlen(p);
        while (n && isspace((unsigned char)p[n - 1])) p[--n] = '\0';
        if (!*p) continue;
        if (!strncmp(p, "vendor", 6) && isspace((unsigned char)p[6])) {
            char *e;
            unsigned long v = strtoul(p + 7, &e, 0);
            while (isspace((unsigned char)*e)) e++;
            if (*e || e == p + 7) { snprintf(err, errlen, "%s:%d: expected 'vendor 0xID'", path, ln); rc = -1; break; }
            vendor = (uint32_t)v;
            have_vendor = 1;
            continue;
        }
        char *e;
        unsigned long c = strtoul(p, &e, 0);
        if (e == p || c > 0xFFFF || !isspace((unsigned char)*e)) {
            snprintf(err, errlen, "%s:%d: expected 'CODE text'", path, ln); rc = -1; break;
        }
        if (!have_vendor) { snprintf(err, errlen, "%s:%d: code before any 'vendor' line", path, ln); rc = -1; break; }
        while (isspace((unsigned char)*e)) e++;
        if (!*e) { snprintf(err, errlen, "%s:%d: code 0x%04lX without a text", path, ln, c); rc = -1; break; }
        int k;
        for (k = 0; k < NVT; k++) if (VT[k].vendor == vendor && VT[k].code == c) break;
        if (k == NVT) {
            if (NVT == ECM_EMCY_VENDOR_MAX) { snprintf(err, errlen, "%s:%d: more than %d vendor codes", path, ln, ECM_EMCY_VENDOR_MAX); rc = -1; break; }
            NVT++;
        }
        VT[k].vendor = vendor;
        VT[k].code = (uint16_t)c;
        snprintf(VT[k].text, sizeof(VT[k].text), "%s", e);
    }
    fclose(f);
    return rc;
}

static int in_fault(const ecm_cia402_state_t *s) { return s->ds == ECM_DS_FAULT || s->ds == ECM_DS_FRA; }

ecm_code_src_t ecm_cia402_axis_code(const ecm_cia402_state_t *s, int axes_on_slave, int faulted_on_slave,
                                    const ecm_cia402_emcy_in_t *last, uint16_t *code)
{
    *code = 0;
    if (s->ecode) { *code = s->ecode; return ECM_CODE_603F; }
    if (in_fault(s) && last && last->have && last->code && (axes_on_slave == 1 || faulted_on_slave == 1)) {
        *code = last->code;
        return ECM_CODE_EMCY;
    }
    return ECM_CODE_NONE;
}

size_t ecm_cia402_axis_diag(const char *name, uint16_t slave, const ecm_cia402_state_t *s,
                            int axes_on_slave, int faulted_on_slave, const ecm_cia402_emcy_in_t *last,
                            uint32_t vendor, char *buf, size_t cap)
{
    size_t len = 0;
#define AP(...) do { if (len < cap) { int r_ = snprintf(buf + len, cap - len, __VA_ARGS__); if (r_ > 0) len += (size_t)r_; } } while (0)
    if (!cap) return 0;
    buf[0] = '\0';
    AP("axis %s (slave %u): %s, mode %s", name, slave, ecm_cia402_ds_str((ecm_ds_t)s->ds), ecm_cia402_mode_str(s->mode_disp));
    if (s->err) AP(", error %s", ecm_cia402_err_str(s->err));
    if (s->err == ECM_AXERR_TIMEOUT || s->err == ECM_AXERR_DROPPED) AP(" (%s)", ecm_cia402_ds_str((ecm_ds_t)s->err_ds));
    uint16_t code;
    ecm_code_src_t src = ecm_cia402_axis_code(s, axes_on_slave, faulted_on_slave, last, &code);
    if (src != ECM_CODE_NONE) {
        const char *t = ecm_cia402_emcy_text(code, vendor);
        AP("; code 0x%04X %s [%s] (%s", code, t ? t : "-", ecm_emcy_class_name(code), src == ECM_CODE_603F ? "0x603F" : "EMCY");
        if (src == ECM_CODE_603F && last && last->have && last->code == code) AP(", = EMCY");
        AP(")");
    } else if (in_fault(s)) {
        if (!last || !last->have || !last->code)
            AP("; no error code (0x603F %s, no EMCY from the slave)", s->ecode ? "0" : "not mapped or 0");
        else
            AP("; EMCY 0x%04X not attributable: %d of the slave's %d axes in fault", last->code, faulted_on_slave, axes_on_slave);
    }
    if (src != ECM_CODE_EMCY && last && last->have && last->code && !(src == ECM_CODE_603F && last->code == code) &&
        !(in_fault(s) && src == ECM_CODE_NONE)) {
        const char *t = ecm_cia402_emcy_text(last->code, vendor);
        AP("; slave's last EMCY 0x%04X %s", last->code, t ? t : ecm_emcy_class_name(last->code));
    }
#undef AP
    return len < cap ? len : cap - 1;
}
