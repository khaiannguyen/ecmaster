/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* ==========================================================================
 * ecm_diag — bus diagnostics CLI (Phase 7.2).
 *
 *   ecm_diag                         print the snapshot ecm_run writes every
 *                                    second (default /tmp/ecm_diag.txt)
 *   ecm_diag --watch [sec]           same, refreshed
 *   ecm_diag --standalone <iface>    read the bus directly, ONLY when
 *                                    ecm_run is not running (plan §3.2)
 *
 * Why a snapshot and not its own socket: two SOEM masters on one interface
 * share the datagram index space (EC_MAXBUF=16) and pick up each other's
 * replies — Phase 6 already saw SOEM match a reply to the wrong request.
 * Same model as IgH's `ethercat` tool asking the kernel module.
 *
 * Standalone reads by POSITION (APRD), with no ecx_config_init(): no
 * station address is written, no state is requested, counters are read but
 * not cleared. So it can look at a bus left in any state, including a
 * slave that power-cycled and has station address 0.
 *
 * Phase 11.3: ecm_run's API (docs/api.md) as the preferred source:
 *   ecm_diag --api [DIR]             stats.json of `ecm_run --api` (default
 *                                    /run/ecmaster), rendered; --watch works too
 *   ecm_diag [--api DIR] info        ctl.sock: identity, ENI, PDO table, allowlist
 *   ecm_diag [--api DIR] sdo read SLAVE INDEX SUB [TYPE]
 *   ecm_diag [--api DIR] sdo write SLAVE INDEX SUB TYPE VALUE   (allowlist only)
 *   ecm_diag [--api DIR] dump        diag_dump: timestamped copies in DIR
 *   --json                           print the raw JSON instead
 * --standalone is also refused while ecm_run's API is alive (ctl.sock answers,
 * or stats.json is fresh): ecm_run may run with --no-diag, no snapshot then.
 *
 * Exit codes: 0 = no ERROR finding, 1 = at least one ERROR finding,
 *             2 = no/stale snapshot, 3 = refused (ecm_run running), 4 = bus I/O failed.
 *   --api: 1 also for bus LOST/RECOVER, an axis in fault or with an axis error;
 *          2 also when stats.json is final (ecm_run has stopped).
 *   ctl.sock commands: 0 ok, 1 refused/failed, 2 no socket, 4 bad reply.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "soem/soem.h"
#include "libecmaster/diag/ecm_diag.h"
#include "libecmaster/diag/ecm_diag_soem.h"
#include "ecm_api_view.h"

#define DEFAULT_FILE "/tmp/ecm_diag.txt"
#define DEFAULT_API  "/run/ecmaster"

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static char g_text[65536];

/* Returns snapshot age in seconds, or -1 if unreadable. Text in g_text. */
static double load_snapshot(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1.0;
    size_t n = fread(g_text, 1, sizeof(g_text) - 1, fp);
    fclose(fp);
    g_text[n] = '\0';
    double t = ecm_diag_snapshot_time(g_text);
    return t < 0 ? -1.0 : mono_s() - t;
}

static int exit_code_of(const char *text) { return strstr(text, "[ERROR]") ? 1 : 0; }

static int show_snapshot(const char *path, double max_age)
{
    double age = load_snapshot(path);
    if (age < 0) {
        fprintf(stderr, "ecm_diag: no snapshot at %s (is ecm_run running? or use --standalone)\n", path);
        return 2;
    }
    fputs(g_text, stdout);
    if (age > max_age) {
        printf("WARNING: snapshot is %.1f s old — ecm_run is probably not running any more\n", age);
        return 2;
    }
    printf("(snapshot age %.1f s)\n", age);
    return exit_code_of(g_text);
}

static const char *al_str(uint16_t code) { return ec_ALstatuscode2string(code); }

static int standalone(const char *iface, int n, const char *path, double max_age, int force, const char *api_dir)
{
    char sp[512], sk[512];
    snprintf(sp, sizeof(sp), "%s/stats.json", api_dir);
    snprintf(sk, sizeof(sk), "%s/ctl.sock", api_dir);
    double sage;
    int fin;
    if (!force && (ecm_view_ctl_alive(sk) || (ecm_view_load(sp, &sage, &fin) == 0 && !fin && sage <= max_age))) {
        fprintf(stderr,
            "ecm_diag: refusing --standalone: ecm_run's API is alive in %s (ctl.sock or a fresh stats.json),\n"
            "          so ecm_run is running on the bus. Use ecm_diag --api %s instead, or pass --force.\n", api_dir, api_dir);
        return 3;
    }
    double age = load_snapshot(path);
    if (age >= 0 && age <= max_age && !force) {
        fprintf(stderr,
            "ecm_diag: refusing --standalone: %s is %.1f s old, so ecm_run is running on the bus.\n"
            "          Two SOEM masters on one interface pick up each other's frames.\n"
            "          Read the snapshot instead (ecm_diag), or pass --force.\n", path, age);
        return 3;
    }
    static ecx_contextt ctx;
    if (!ecx_init(&ctx, iface)) {
        fprintf(stderr, "ecm_diag: ecx_init(%s) failed (capabilities? interface name?)\n", iface);
        return 4;
    }
    static ecm_diag_raw_t raw;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int brd = ecm_diag_soem_read_positional(&ctx, n, &raw,
                                            (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
    ecx_close(&ctx);
    if (brd < 0) {
        printf("# ecm_diag standalone on %s: no reply to BRD at all (frame lost: link down, "
               "or frames corrupted on the way)\n", iface);
        return 1;
    }
    if (brd == 0) {
        printf("# ecm_diag standalone on %s: frame came back but no slave processed it (BRD WKC 0)\n", iface);
        return 1;
    }
    static ecm_diag_t d;
    static ecm_diag_finding_t f[128];
    /* n given: judge against the expected count (topology); otherwise take
     * what answers. No expected AL state: the bus may be idle in any state. */
    ecm_diag_init(&d, n > 0 ? n : brd, 0);
    raw.n = d.n;
    if (n > brd) {                         /* slaves beyond the break: not read */
        raw.count = brd;
    }
    ecm_diag_ingest(&d, &raw);
    int nf = ecm_diag_analyze(&d, f, 128);
    ecm_diag_format(&d, f, nf, NULL, "standalone", al_str, g_text, sizeof(g_text));
    fputs(g_text, stdout);
    printf("(standalone: position-addressed, counters read but NOT cleared)\n");
    return exit_code_of(g_text);
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [--file PATH] [--max-age SEC] [--watch [SEC]]\n"
        "       %s --api [DIR] [--max-age SEC] [--watch [SEC]] [--json]          (Phase 11.3, default DIR %s)\n"
        "       %s [--api DIR] [--json] info | dump | sdo read SLAVE INDEX SUB [TYPE] | sdo write SLAVE INDEX SUB TYPE VALUE\n"
        "       %s --standalone IFACE [--n EXPECTED_SLAVES] [--force] [--file PATH] [--api DIR]\n", p, p, DEFAULT_API, p, p);
}

static int show_api(const char *dir, double max_age, int json)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/stats.json", dir);
    double age;
    int fin, r = ecm_view_load(p, &age, &fin);
    if (r == -1) { fprintf(stderr, "ecm_diag: no %s (is ecm_run running with --api --api-dir %s?)\n", p, dir); return 2; }
    if (r == -2) { fprintf(stderr, "ecm_diag: %s is not a schema-1 stats.json\n", p); return 2; }
    return ecm_view_print(p, age, fin, max_age, json);
}

int main(int argc, char **argv)
{
    const char *path = DEFAULT_FILE, *iface = NULL, *api_dir = NULL;
    double max_age = 3.0, watch = 0;
    int n = 0, force = 0, json = 0, use_api = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "info") || !strcmp(a, "dump") || !strcmp(a, "sdo")) {   /* ctl.sock commands */
            char sock[512];
            snprintf(sock, sizeof(sock), "%s/ctl.sock", api_dir ? api_dir : DEFAULT_API);
            if (!strcmp(a, "info")) return i + 1 == argc ? ecm_view_simple(sock, "info", json) : (usage(argv[0]), 2);
            if (!strcmp(a, "dump")) return i + 1 == argc ? ecm_view_simple(sock, "diag_dump", json) : (usage(argv[0]), 2);
            if (v && (!strcmp(v, "read") || !strcmp(v, "write"))) {
                int r = ecm_view_sdo(sock, !strcmp(v, "write"), argc - i - 2, argv + i + 2, json);
                if (r >= 0) return r;
            }
            usage(argv[0]);
            return 2;
        }
        if      (!strcmp(a, "--api"))             { use_api = 1; if (v && v[0] != '-' && strcmp(v, "info") && strcmp(v, "dump") && strcmp(v, "sdo")) { api_dir = v; i++; } }
        else if (!strcmp(a, "--json"))            { json = 1; }
        else if (!strcmp(a, "--file") && v)       { path = v; i++; }
        else if (!strcmp(a, "--max-age") && v)    { max_age = atof(v); i++; }
        else if (!strcmp(a, "--watch"))           { watch = (v && v[0] != '-') ? atof(argv[++i]) : 1.0; }
        else if (!strcmp(a, "--standalone") && v) { iface = v; i++; }
        else if (!strcmp(a, "--n") && v)          { n = atoi(v); i++; }
        else if (!strcmp(a, "--force"))           { force = 1; }
        else { usage(argv[0]); return 2; }
    }
    if (!api_dir) api_dir = DEFAULT_API;
    if (iface) return standalone(iface, n, path, max_age, force, api_dir);
    if (use_api && watch <= 0) return show_api(api_dir, max_age, json);
    if (watch <= 0) return show_snapshot(path, max_age);
    for (;;) {
        printf("\033[H\033[2J");
        if (use_api) show_api(api_dir, max_age, json);
        else show_snapshot(path, max_age);
        fflush(stdout);
        usleep((useconds_t)(watch * 1e6));
    }
}
