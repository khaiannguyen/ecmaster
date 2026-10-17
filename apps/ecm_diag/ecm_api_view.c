/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* ==========================================================================
 * ecm_api_view.c -- Phase 11.3: ecm_diag as a client of ecm_run's API
 * (docs/api.md): stats.json rendered for a terminal, and ctl.sock commands.
 * No SOEM, no bus: only files and the AF_UNIX socket.
 * ========================================================================== */
#define _GNU_SOURCE
#include "ecm_api_view.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "libecmaster/api/ecm_json.h"

#define MAX_TOK 65536
static char       g_doc[1 << 20];
static ecm_jtok_t g_t[MAX_TOK];
static int        g_n;

/* ---------------------------------------------------------- JSON helpers --- */
static int get(int obj, const char *key) { return ecm_json_get(g_doc, g_t, g_n, obj, key); }

static int path_get(const char *path)        /* "bus.ports" from the root */
{
    char k[64];
    int cur = 0;
    const char *p = path;
    while (*p && cur >= 0) {
        size_t l = strcspn(p, ".");
        if (l >= sizeof(k)) return -1;
        memcpy(k, p, l);
        k[l] = '\0';
        cur = get(cur, k);
        p += l;
        if (*p == '.') p++;
    }
    return cur;
}

static const char *s_of(int i, char *buf, size_t cap)
{
    if (i < 0 || g_t[i].type != ECM_J_STR || ecm_json_str(g_doc, &g_t[i], buf, cap)) {
        snprintf(buf, cap, "%s", i >= 0 && g_t[i].type == ECM_J_NULL ? "null" : "?");
    }
    return buf;
}

static const char *s_key(int obj, const char *key, char *buf, size_t cap) { return s_of(get(obj, key), buf, cap); }

static int64_t i_key(int obj, const char *key, int64_t dflt)
{
    int v = get(obj, key);
    int64_t x;
    if (v < 0 || ecm_json_i64(g_doc, &g_t[v], &x)) {
        uint64_t u;
        if (v >= 0 && !ecm_json_u64(g_doc, &g_t[v], &u)) return (int64_t)u;
        return dflt;
    }
    return x;
}

static uint64_t u_key(int obj, const char *key)
{
    int v = get(obj, key);
    uint64_t x = 0;
    if (v >= 0) ecm_json_u64(g_doc, &g_t[v], &x);
    return x;
}

static int b_key(int obj, const char *key) { int v = get(obj, key); return v >= 0 && g_t[v].type == ECM_J_TRUE; }
static int is_null(int i) { return i < 0 || g_t[i].type == ECM_J_NULL; }

/* i-th element (0-based) of array token a */
static int elem(int a, int k)
{
    if (a < 0 || g_t[a].type != ECM_J_ARR || k >= g_t[a].size) return -1;
    int i = a + 1;
    while (k-- > 0) i = ecm_json_skip(g_t, g_n, i);
    return i;
}

static int count(int a) { return a >= 0 && g_t[a].type == ECM_J_ARR ? g_t[a].size : 0; }

/* "a/b/c/d" of an array of 4 integers */
static const char *quad(int obj, const char *key, char *buf, size_t cap)
{
    int a = get(obj, key);
    size_t o = 0;
    buf[0] = '\0';
    for (int k = 0; k < count(a) && o < cap; k++) {
        uint64_t x = 0;
        ecm_json_u64(g_doc, &g_t[elem(a, k)], &x);
        o += (size_t)snprintf(buf + o, cap - o, "%s%" PRIu64, k ? "/" : "", x);
    }
    return buf;
}

/* ------------------------------------------------------------ histograms --- */
/* percentile p of hist object h; value in us; -1 when empty */
static double pct(int h, double p, int *overflow)
{
    *overflow = 0;
    if (is_null(h)) return -1;
    char kind[32];
    s_key(h, "buckets", kind, sizeof(kind));
    uint64_t total = u_key(h, "count");
    int c = get(h, "counts");
    if (!total || c < 0) return -1;
    uint64_t want = (uint64_t)(p * (double)total), acc = 0;
    if (want >= total) want = total - 1;
    int lin = !strcmp(kind, "lin1us_v1");
    int up = path_get("buckets.log20_100ns_v1.upper");
    for (int k = 0; k < count(c); k++) {
        uint64_t x = 0;
        ecm_json_u64(g_doc, &g_t[elem(c, k)], &x);
        acc += x;
        if (acc > want) {
            if (lin) return (double)k;
            if (k >= count(up)) {               /* above the last bound: the max */
                *overflow = 1;
                return (double)i_key(h, "max", 0) / 1000.0;
            }
            uint64_t ub = 0;
            ecm_json_u64(g_doc, &g_t[elem(up, k)], &ub);
            return (double)ub / 1000.0;
        }
    }
    return -1;
}

static void hist_row(const char *label, int h)
{
    static const double P[] = { 0.50, 0.99, 0.999, 0.9999 };
    if (is_null(h)) { printf("  %-12s (none)\n", label); return; }
    printf("  %-12s", label);
    int of;
    for (int k = 0; k < 4; k++) {
        double v = pct(h, P[k], &of);
        if (v < 0) printf(" %9s", "-"); else printf(" %9.1f", v);
    }
    char kind[32];
    s_key(h, "buckets", kind, sizeof(kind));
    if (!strcmp(kind, "lin1us_v1")) printf("         -");
    else printf(" %9.1f", (double)i_key(h, "max", 0) / 1000.0);
    printf("  n=%" PRIu64 "\n", u_key(h, "count"));
}

/* -------------------------------------------------------------- stats.json --- */
static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int ecm_view_load(const char *path, double *age, int *final)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    size_t n = fread(g_doc, 1, sizeof(g_doc) - 1, f);
    fclose(f);
    g_doc[n] = '\0';
    g_n = ecm_json_parse(g_doc, n, g_t, MAX_TOK, NULL);
    if (g_n <= 0 || g_t[0].type != ECM_J_OBJ || i_key(0, "schema", 0) != 1) return -2;
    *age = mono_s() - (double)u_key(0, "mono_ns") * 1e-9;
    *final = b_key(0, "final");
    return 0;
}

static int bad_state(const char *st) { return !strcmp(st, "FAULT") || !strcmp(st, "FAULT_REACTION_ACTIVE"); }

int ecm_view_print(const char *path, double age, int final, double max_age, int json)
{
    char a[128], b[128], c[128], d[128];
    if (json) {
        fputs(g_doc, stdout);
        return final || age > max_age ? 2 : 0;
    }
    int errors = 0;
    printf("ecm_run pid %" PRId64 "  %s  pub_seq %" PRIu64 "  age %.1f s%s\n", i_key(0, "pid", 0), path,
           u_key(0, "pub_seq"), age, final ? "  FINAL (ecm_run has stopped)" : age > max_age ? "  STALE" : "");

    int bus = get(0, "bus");
    s_key(bus, "fsm", a, sizeof(a));
    if (!strcmp(a, "LOST") || !strcmp(a, "RECOVER")) errors++;
    printf("bus %s  cycle %" PRId64 " us  link %s  slaves %" PRId64 "  path %s\n", a, i_key(bus, "cycle_ns", 0) / 1000,
           s_key(bus, "link", b, sizeof(b)), i_key(bus, "n_slaves", 0), s_key(bus, "path", c, sizeof(c)));
    int ports = get(bus, "ports");
    for (int k = 0; k < count(ports); k++) {
        int p = elem(ports, k);
        printf("  port %s %s %s", s_key(p, "name", a, sizeof(a)), s_key(p, "iface", b, sizeof(b)), s_key(p, "link", c, sizeof(c)));
        if (get(p, "tx") >= 0) printf("  tx %" PRIu64 " noframe %" PRIu64, u_key(p, "tx"), u_key(p, "noframe"));
        static const char *const NIC[] = { "crc_err", "rx_missed", "carrier_changes" };
        for (int q = 0; q < 3; q++) {
            int v = get(p, NIC[q]);
            if (is_null(v)) printf(" %s -", NIC[q]); else printf(" %s %" PRIu64, NIC[q], u_key(p, NIC[q]));
        }
        printf("\n");
    }

    int rt = get(0, "rt");
    s_key(rt, "isolated_cpus", a, sizeof(a));
    printf("rt: core %" PRId64 ", isolated %s", i_key(rt, "rt_cpu", -1), a[0] ? a : "none");
    int ci = get(rt, "cpuidle");
    if (ci >= 0) {
        int dis = get(ci, "states_disabled");
        printf(", cpuidle %s%s", s_key(ci, "driver", b, sizeof(b)), b_key(ci, "off") ? " (off)" : "");
        if (count(dis)) {
            printf(" (disabled on core %" PRId64 ":", i_key(rt, "rt_cpu", -1));
            for (int k = 0; k < count(dis); k++) printf(" %s", s_of(elem(dis, k), c, sizeof(c)));
            printf(")");
        }
    }
    printf(", kernel %s\n    cmdline:", s_key(rt, "kernel", a, sizeof(a)));
    int fl = get(rt, "cmdline_flags");
    if (!count(fl)) printf(" (no RT flag)");
    for (int k = 0; k < count(fl); k++) printf(" %s", s_of(elem(fl, k), a, sizeof(a)));
    printf("\n    cpu_dma_latency 0 %s, hook %s, api cpus %s\n", b_key(rt, "dma_latency_held") ? "held" : "NOT held",
           s_key(rt, "hook", a, sizeof(a)), s_key(rt, "api_cpus", b, sizeof(b)));

    int groups = get(0, "groups");
    for (int k = 0; k < count(groups); k++) {
        int g = elem(groups, k), sl = get(g, "slaves");
        printf("group %-6s slaves", s_key(g, "name", a, sizeof(a)));
        for (int q = 0; q < count(sl); q++) {
            int64_t x = 0;
            ecm_json_i64(g_doc, &g_t[elem(sl, q)], &x);
            printf("%s%" PRId64, q ? "," : " ", x);
        }
        printf("  cycle %" PRId64 " us", i_key(g, "cycle_ns", 0) / 1000);
        if (get(g, "cycles") >= 0)
            printf("  cycles %" PRIu64 " ok %" PRIu64 " noframe %" PRIu64 " zero %" PRIu64 " partial %" PRIu64 " over %" PRIu64
                   " max_run_bad %" PRIu64 " overrun %" PRIu64, u_key(g, "cycles"), u_key(g, "ok"), u_key(g, "noframe"),
                   u_key(g, "zero"), u_key(g, "partial"), u_key(g, "over"), u_key(g, "max_run_bad"), u_key(g, "overrun"));
        printf("\n");
    }
    int dc = get(0, "dc");
    if (is_null(dc)) printf("dc off\n");
    else printf("dc %s  ref %" PRId64 "  locks %" PRIu64 " unlocks %" PRIu64 " clamps %" PRIu64 " wraps %" PRIu64 " stale %" PRIu64 "  e %.1f us\n",
                s_key(dc, "state", a, sizeof(a)), i_key(dc, "ref_pos", 0), u_key(dc, "locks"), u_key(dc, "unlocks"),
                u_key(dc, "clamps"), u_key(dc, "wraps"), u_key(dc, "stale"), (double)i_key(dc, "err_ns", 0) / 1000.0);
    int ln = get(0, "link");
    printf("link: etf missed %" PRIu64 " invalid %" PRIu64, u_key(ln, "etf_missed"), u_key(ln, "etf_invalid"));
    if (get(ln, "txtime_late") >= 0)
        printf("  txtime late %" PRIu64 " bypass %" PRIu64 " send_err %" PRIu64 " tai_steps %" PRIu64, u_key(ln, "txtime_late"),
               u_key(ln, "txtime_bypass"), u_key(ln, "send_err"), u_key(ln, "tai_steps"));
    printf("\n");

    int h = get(0, "hist");
    printf("timing (us)          p50       p99     p99.9    p99.99       max\n");
    hist_row("wake_jitter", get(h, "wake_jitter"));
    hist_row("prep_send", get(h, "prep_send"));
    hist_row("turnaround", get(h, "turnaround"));
    hist_row("occupancy", get(h, "occupancy"));
    hist_row("hook", get(h, "hook"));
    hist_row("dc |e|", get(h, "dc_err"));

    int slaves = get(0, "slaves");
    printf("slaves:\n");
    for (int k = 0; k < count(slaves); k++) {
        int s = elem(slaves, k);
        printf("  %2" PRId64 " %-12s %s:%s", i_key(s, "pos", 0), s_key(s, "name", a, sizeof(a)), s_key(s, "vendor", b, sizeof(b)),
               s_key(s, "product", c, sizeof(c)));
        if (get(s, "al") >= 0) printf("  %s%s code %s", s_key(s, "al", a, sizeof(a)), b_key(s, "al_err") ? "+ERR" : "", s_key(s, "al_code", b, sizeof(b)));
        if (get(s, "rx_err") >= 0) {
            printf("  rx %s", quad(s, "rx_err", a, sizeof(a)));
            printf(" fwd %s", quad(s, "fwd_err", a, sizeof(a)));
            printf(" lost %s", quad(s, "lost_link", a, sizeof(a)));
            printf(" pu %" PRIu64 " pdi %" PRIu64, u_key(s, "pu_err"), u_key(s, "pdi_err"));
        }
        if (get(s, "recover") >= 0)
            printf("  recover %s (%s, %" PRIu64 " recoveries%s)  emcy %" PRIu64, s_key(s, "recover", a, sizeof(a)), s_key(s, "policy", b, sizeof(b)),
                   u_key(s, "recoveries"), u_key(s, "initcmd_failed") ? ", InitCmd failed" : "", u_key(s, "emcy_count"));
        printf("\n");
        int em = get(s, "emcy");
        for (int q = 0; q < count(em); q++) {
            int e = elem(em, q);
            printf("       EMCY %s [%s] reg %s data %s\n", s_key(e, "code", a, sizeof(a)), s_key(e, "class", b, sizeof(b)),
                   s_key(e, "reg", c, sizeof(c)), s_key(e, "data", d, sizeof(d)));
        }
    }

    int axes = get(0, "axes");
    if (count(axes)) printf("axes:\n");
    for (int k = 0; k < count(axes); k++) {
        int x = elem(axes, k);
        printf("  %-6s slave %" PRId64 "  ", s_key(x, "name", a, sizeof(a)), i_key(x, "slave", 0));
        if (get(x, "state") < 0) { printf("(state not readable)\n"); continue; }
        s_key(x, "state", a, sizeof(a));
        s_key(x, "err", b, sizeof(b));
        if (bad_state(a) || strcmp(b, "none")) errors++;
        printf("%-22s mode %" PRId64 "/%" PRId64 "  sw %s  err %s  pos %" PRId64 " vel %" PRId64 "  used %" PRIu64 " late %" PRIu64
               " underruns %" PRIu64 " dropped %" PRIu64 " step_refused %" PRIu64 "\n", a, i_key(x, "mode", 0), i_key(x, "mode_display", 0),
               s_key(x, "sw", c, sizeof(c)), b, i_key(x, "pos", 0), i_key(x, "vel", 0), u_key(x, "used"), u_key(x, "late"),
               u_key(x, "underruns"), u_key(x, "dropped"), u_key(x, "step_refused"));
        s_key(x, "error_src", a, sizeof(a));
        if (strcmp(a, "none"))
            printf("         code %s %s (from %s)\n", s_key(x, "error_code", b, sizeof(b)), s_key(x, "error_text", c, sizeof(c)),
                   !strcmp(a, "603F") ? "0x603F" : "EMCY");
    }

    int fd = get(0, "findings");
    printf("findings:%s\n", count(fd) ? "" : " none");
    for (int k = 0; k < count(fd); k++) {
        int f = elem(fd, k);
        s_key(f, "sev", a, sizeof(a));
        if (!strcmp(a, "error")) errors++;
        for (char *p = a; *p; p++) *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        printf("  [%s] %s%s\n", a, s_key(f, "text", b, sizeof(b)), b_key(f, "active") ? "" : " (not in the last read)");
    }
    int src = get(0, "sources");
    if (u_key(src, "handoff_drops")) printf("note: %" PRIu64 " handoff updates dropped\n", u_key(src, "handoff_drops"));
    if (final || age > max_age) return 2;
    return errors ? 1 : 0;
}

/* -------------------------------------------------------------- ctl.sock --- */
int ecm_view_ctl(const char *sock, const char *request, char *reply, size_t cap)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -4;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { close(fd); return -2; }
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t l = strlen(request);
    if (send(fd, request, l, MSG_NOSIGNAL) != (ssize_t)l || send(fd, "\n", 1, MSG_NOSIGNAL) != 1) {
        /* refused at once (too_many_clients): its reply may still be readable */
    }
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, reply + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        if (memchr(reply, '\n', n)) break;
    }
    close(fd);
    reply[n] = '\0';
    char *nl = strchr(reply, '\n');
    if (nl) *nl = '\0';
    if (!n) return -4;
    g_n = ecm_json_parse(reply, strlen(reply), g_t, MAX_TOK, NULL);
    if (g_n <= 0) return -4;
    memcpy(g_doc, reply, strlen(reply) + 1);       /* the helpers read g_doc */
    return b_key(0, "ok") ? 0 : -1;
}

int ecm_view_ctl_alive(const char *sock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

/* a command-line value as JSON: "0x.." stays a string (ctl.sock reads it as hex),
 * decimal integers and floats as numbers */
static void jval(const char *v, char *out, size_t cap)
{
    char *e;
    if (!strncasecmp(v, "0x", 2) || !strncasecmp(v, "-0x", 3)) { snprintf(out, cap, "\"%s\"", v); return; }
    strtod(v, &e);
    if (*v && !*e) snprintf(out, cap, "%s", v);
    else snprintf(out, cap, "\"%s\"", v);       /* not a number: ctl.sock refuses it clearly */
}

static int ctl_result(int rc, const char *sock, int json, const char *reply)
{
    char a[256], b[512];
    if (rc == -2) {
        fprintf(stderr, "ecm_diag: %s: %s (is ecm_run running with --api?)\n", sock, strerror(errno));
        return 2;
    }
    if (rc == -4) { fprintf(stderr, "ecm_diag: %s: no valid reply\n", sock); return 4; }
    if (json) { printf("%s\n", reply); return rc ? 1 : 0; }
    if (rc) {
        fprintf(stderr, "refused: %s: %s", s_key(0, "error", a, sizeof(a)), s_key(0, "msg", b, sizeof(b)));
        if (get(0, "abort") >= 0 && !is_null(get(0, "abort"))) fprintf(stderr, " (abort %s)", s_key(0, "abort", a, sizeof(a)));
        fprintf(stderr, "\n");
        return 1;
    }
    return 0;
}

int ecm_view_sdo(const char *sock, int write, int argc, char **argv, int json)
{
    char req[512], v[128], a[128], b[128];
    if (write) {
        if (argc != 5) return -1;
        jval(argv[4], v, sizeof(v));
        snprintf(req, sizeof(req), "{\"id\":1,\"cmd\":\"sdo_write\",\"slave\":%s,\"index\":\"%s\",\"sub\":%s,\"type\":\"%s\",\"value\":%s}",
                 argv[0], argv[1], argv[2], argv[3], v);
    } else {
        if (argc < 3 || argc > 4) return -1;
        snprintf(req, sizeof(req), "{\"id\":1,\"cmd\":\"sdo_read\",\"slave\":%s,\"index\":\"%s\",\"sub\":%s,\"type\":\"%s\"}",
                 argv[0], argv[1], argv[2], argc == 4 ? argv[3] : "raw");
    }
    static char reply[1 << 16];
    int rc = ecm_view_ctl(sock, req, reply, sizeof(reply));
    int r = ctl_result(rc, sock, json, reply);
    if (r || json) return r;
    char val[64] = "";
    int vv = get(0, write ? "readback" : "value");
    if (vv >= 0) snprintf(val, sizeof(val), "%.*s", g_t[vv].end - g_t[vv].start, g_doc + g_t[vv].start);
    if (write)
        printf("slave %s %s:%s %s written %.*s, read back %s (%s)  %.1f ms\n", argv[0], s_key(0, "index", a, sizeof(a)), argv[2],
               argv[3], g_t[get(0, "written")].end - g_t[get(0, "written")].start, g_doc + g_t[get(0, "written")].start, val,
               b_key(0, "readback_ok") ? "same" : "DIFFERENT", (double)i_key(0, "ms", 0));
    else
        printf("slave %s %s:%s %s = %s (raw %s, %" PRId64 " byte)\n", argv[0], s_key(0, "index", a, sizeof(a)), argv[2],
               s_key(0, "type", b, sizeof(b)), val[0] ? val : "-", s_key(0, "raw", v, sizeof(v)), i_key(0, "size", 0));
    return write && !b_key(0, "readback_ok") ? 1 : 0;
}

int ecm_view_simple(const char *sock, const char *cmd, int json)
{
    char req[96], a[512], b[512], c[128], d[512];
    snprintf(req, sizeof(req), "{\"id\":1,\"cmd\":\"%s\"}", cmd);
    static char reply[1 << 18];
    int rc = ecm_view_ctl(sock, req, reply, sizeof(reply));
    int r = ctl_result(rc, sock, json, reply);
    if (r || json) return r;
    if (!strcmp(cmd, "diag_dump")) {
        int f = get(0, "files");
        for (int k = 0; k < count(f); k++) printf("%s\n", s_of(elem(f, k), a, sizeof(a)));
        return 0;
    }
    int in = get(0, "info"), er = get(in, "ecm_run"), eni = get(in, "eni"), pdo = get(in, "pdo"), al = get(in, "allow");
    printf("ecm_run %s pid %" PRId64 "  cycle %" PRId64 " us  link %s\n", s_key(er, "version", a, sizeof(a)), i_key(er, "pid", 0),
           i_key(in, "cycle_ns", 0) / 1000, s_key(in, "link", b, sizeof(b)));
    if (is_null(eni)) printf("eni: none\n");
    else printf("eni: %s (from %s)\n     sha256 %s, %" PRId64 " CoE InitCmds (%" PRId64 " downloads), %" PRId64 " register InitCmds\n",
                s_key(eni, "path", a, sizeof(a)), s_key(eni, "source", b, sizeof(b)), s_key(eni, "sha256", c, sizeof(c)),
                i_key(eni, "coe_initcmds", 0), i_key(eni, "coe_downloads", 0), i_key(eni, "reg_initcmds", 0));
    int sl = get(in, "slaves");
    for (int k = 0; k < count(sl); k++) {
        int s = elem(sl, k);
        char e1[64], e2[64];
        printf("  slave %" PRId64 " %-12s %s:%s rev %s  %s%s\n", i_key(s, "pos", 0), s_key(s, "name", a, sizeof(a)),
               s_key(s, "vendor", b, sizeof(b)), s_key(s, "product", c, sizeof(c)), s_key(s, "revision", e1, sizeof(e1)),
               s_key(s, "group", e2, sizeof(e2)), b_key(s, "coe") ? ", CoE" : "");
    }
    printf("pdo table: %s, %d entries\n", s_key(pdo, "source", a, sizeof(a)), count(get(pdo, "entries")));
    int ae = get(al, "entries");
    printf("sdo_write allowlist: %s, %d entries\n", s_key(al, "path", a, sizeof(a)), count(ae));
    for (int k = 0; k < count(ae); k++) {
        int e = elem(ae, k);
        char e1[16];
        printf("  %s:%s %s:%" PRId64 " %s [%.*s, %.*s] %s\n", s_key(e, "vendor", a, sizeof(a)), s_key(e, "product", b, sizeof(b)),
               s_key(e, "index", c, sizeof(c)), i_key(e, "sub", 0), s_key(e, "type", e1, sizeof(e1)),
               g_t[get(e, "min")].end - g_t[get(e, "min")].start, g_doc + g_t[get(e, "min")].start,
               g_t[get(e, "max")].end - g_t[get(e, "max")].start, g_doc + g_t[get(e, "max")].start, s_key(e, "note", d, sizeof(d)));
    }
    int rt = get(in, "rt");
    if (rt >= 0) {
        s_key(rt, "isolated_cpus", a, sizeof(a));
        printf("rt: isolated %s, kernel %s\n", a[0] ? a : "none", s_key(rt, "kernel", b, sizeof(b)));
    }
    return 0;
}
