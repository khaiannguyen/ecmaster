/*
 * test_cia402_diag.c -- Phase 10.8, offline: error code texts, the vendor
 * table file, and the attribution of a slave's EMCY to one of its axes.
 *   D-01  CiA 402 / ETG texts; unknown code -> NULL (class only)
 *   D-02  vendor file: codes for that vendor only; syntax errors named
 *   D-03  attribution: 0x603F first; EMCY when 1 axis or 1 axis in fault;
 *         else not attributable
 *   D-04  axis lines (what the diag file and the exit report show)
 */
#include "ecm_cia402_diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;
static void check(const char *name, int ok, const char *got)
{
    printf("  [%s] %-60s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : "  got: ", ok ? "" : (got ? got : "(null)"));
    if (ok) g_pass++; else g_fail++;
}
static int has(const char *s, const char *sub) { return s && strstr(s, sub) != NULL; }

static const char *tmpfile_with(const char *name, const char *text)
{
    static char path[4][256];
    static int k;
    char *p = path[k++ & 3];
    snprintf(p, 256, "/tmp/test_cia402_diag_%d_%s", (int)getpid(), name);
    FILE *f = fopen(p, "w");
    fputs(text, f);
    fclose(f);
    return p;
}

int main(void)
{
    char err[256], line[512];
    printf("[D-01 texts]\n");
    check("0x2310 continuous over current", has(ecm_cia402_emcy_text(0x2310, 0), "continuous over current"), ecm_cia402_emcy_text(0x2310, 0));
    check("0x8611 following error", has(ecm_cia402_emcy_text(0x8611, 0), "following error"), NULL);
    check("0x8700 sync controller error", has(ecm_cia402_emcy_text(0x8700, 0), "sync"), NULL);
    check("0x5555 unknown -> NULL", ecm_cia402_emcy_text(0x5555, 0) == NULL, ecm_cia402_emcy_text(0x5555, 0));

    printf("[D-02 vendor file]\n");
    const char *ok = tmpfile_with("ok.emcy",
        "# test vendor table\nvendor 0x00100000\n0xFF01 position deviation too large   # comment\n"
        "0x2310   overcurrent (vendor wording)\n\nvendor 0x00000002\n0xFF01 another vendor's 0xFF01\n");
    int rc = ecm_cia402_emcy_load(ok, err, sizeof(err));
    check("loads", rc == 0, err);
    check("3 codes", ecm_cia402_emcy_count() == 3, NULL);
    check("0xFF01 for vendor 0x00100000", has(ecm_cia402_emcy_text(0xFF01, 0x00100000), "position deviation too large") &&
          !has(ecm_cia402_emcy_text(0xFF01, 0x00100000), "comment"), ecm_cia402_emcy_text(0xFF01, 0x00100000));
    check("0xFF01 for vendor 2 is its own", has(ecm_cia402_emcy_text(0xFF01, 2), "another vendor"), NULL);
    check("0xFF01 for an unknown vendor -> NULL", ecm_cia402_emcy_text(0xFF01, 7) == NULL, NULL);
    check("vendor wording before the standard text", has(ecm_cia402_emcy_text(0x2310, 0x00100000), "vendor wording"), NULL);
    check("standard text for other vendors", has(ecm_cia402_emcy_text(0x2310, 5), "continuous over current"), NULL);
    rc = ecm_cia402_emcy_load(tmpfile_with("nov.emcy", "0xFF01 x\n"), err, sizeof(err));
    check("code before 'vendor' refused, line named", rc == -1 && has(err, ":1: code before any 'vendor'"), err);
    rc = ecm_cia402_emcy_load(tmpfile_with("bad.emcy", "vendor 1\n0x1FFFF big\n"), err, sizeof(err));
    check("code > 0xFFFF refused", rc == -1 && has(err, ":2: expected 'CODE text'"), err);
    rc = ecm_cia402_emcy_load(tmpfile_with("notext.emcy", "vendor 1\n0x1234   \n"), err, sizeof(err));
    check("code without text refused", rc == -1 && has(err, ":2:"), err);
    rc = ecm_cia402_emcy_load("/nonexistent.emcy", err, sizeof(err));
    check("missing file", rc == -1 && has(err, "cannot open"), err);

    printf("[D-03 attribution]\n");
    ecm_cia402_state_t f = { .ds = ECM_DS_FAULT, .err = ECM_AXERR_FAULT, .mode_disp = ECM_OPMODE_CSP };
    ecm_cia402_emcy_in_t em = { .have = 1, .code = 0x2310, .reg = 1, .tick = 100 };
    uint16_t code;
    check("1 axis, no 0x603F: EMCY", ecm_cia402_axis_code(&f, 1, 1, &em, &code) == ECM_CODE_EMCY && code == 0x2310, NULL);
    f.ecode = 0x8611;
    check("0x603F first", ecm_cia402_axis_code(&f, 1, 1, &em, &code) == ECM_CODE_603F && code == 0x8611, NULL);
    f.ecode = 0;
    check("4 axes, 2 in fault: not attributable", ecm_cia402_axis_code(&f, 4, 2, &em, &code) == ECM_CODE_NONE, NULL);
    check("4 axes, 1 in fault: EMCY", ecm_cia402_axis_code(&f, 4, 1, &em, &code) == ECM_CODE_EMCY, NULL);
    ecm_cia402_state_t r = { .ds = ECM_DS_OE, .mode_disp = ECM_OPMODE_CSP };
    check("axis running: an EMCY of the slave is not its code", ecm_cia402_axis_code(&r, 1, 0, &em, &code) == ECM_CODE_NONE, NULL);
    ecm_cia402_emcy_in_t reset = { .have = 1, .code = 0x0000 };
    check("EMCY 0x0000 (error reset) is no code", ecm_cia402_axis_code(&f, 1, 1, &reset, &code) == ECM_CODE_NONE, NULL);

    printf("[D-04 axis lines]\n");
    ecm_cia402_emcy_clear();
    ecm_cia402_axis_diag("1:0", 1, &f, 1, 1, &em, 0, line, sizeof(line));
    check("fault via EMCY: state, error, code, text, class, source",
          has(line, "axis 1:0 (slave 1): Fault, mode CSP, error drive fault; code 0x2310 continuous over current [current] (EMCY)"), line);
    f.ecode = 0x2310;
    ecm_cia402_axis_diag("1:0", 1, &f, 1, 1, &em, 0, line, sizeof(line));
    check("fault via 0x603F that equals the EMCY", has(line, "(0x603F, = EMCY)"), line);
    f.ecode = 0;
    ecm_cia402_axis_diag("2:1", 2, &f, 4, 2, &em, 0, line, sizeof(line));
    check("not attributable, reason given", has(line, "EMCY 0x2310 not attributable: 2 of the slave's 4 axes in fault"), line);
    ecm_cia402_axis_diag("1:0", 1, &f, 1, 1, NULL, 0, line, sizeof(line));
    check("fault without any code", has(line, "no error code"), line);
    ecm_cia402_emcy_in_t vend = { .have = 1, .code = 0xFF01 };
    ecm_cia402_emcy_load(ok, err, sizeof(err));
    ecm_cia402_axis_diag("1:0", 1, &f, 1, 1, &vend, 0x00100000, line, sizeof(line));
    check("vendor code with its text, class 'device specific'",
          has(line, "code 0xFF01 position deviation too large [device specific] (EMCY)"), line);
    ecm_cia402_axis_diag("1:0", 1, &r, 1, 0, &em, 0, line, sizeof(line));
    check("running axis: no error, the slave's last EMCY shown apart",
          has(line, "Operation enabled, mode CSP; slave's last EMCY 0x2310 continuous over current") && !has(line, "error"), line);
    ecm_cia402_state_t cf = { .ds = ECM_DS_SOD, .err = ECM_AXERR_CONFIG, .mode_disp = ECM_OPMODE_CSP };
    ecm_cia402_axis_diag("1:0", 1, &cf, 1, 0, NULL, 0, line, sizeof(line));
    check("config error of the slave", has(line, "error slave refused its configuration"), line);
    char tiny[16];
    size_t n = ecm_cia402_axis_diag("1:0", 1, &f, 1, 1, &em, 0, tiny, sizeof(tiny));
    check("truncates safely", n == sizeof(tiny) - 1 && tiny[15] == '\0', tiny);

    char pat[64];
    snprintf(pat, sizeof(pat), "/tmp/test_cia402_diag_%d_", (int)getpid());
    const char *names[] = { "ok.emcy", "nov.emcy", "bad.emcy", "notext.emcy" };
    for (size_t i = 0; i < 4; i++) { char p[256]; snprintf(p, sizeof(p), "%s%s", pat, names[i]); unlink(p); }
    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
