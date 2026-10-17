/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* test_api_offline.c -- Phase 11.1: libecmaster/api without SOEM or network.
 *
 *   ./test_api_offline                 unit tests (handoff, keys, JSON, sysfs, cpus, publish)
 *   ./test_api_offline --dump DIR      also write sample documents DIR/full.json,
 *                                      DIR/empty.json for tools/gd11/stats_check.py
 *   ./test_api_offline --torn N        publish race only: N writes while a
 *                                      reader thread checks every read; prints
 *                                      "torn: K of R reads", exit 0 if K == 0
 * Build with -DECM_API_PUBLISH_INPLACE for the negative control of --torn
 * (the file written in place must be seen half-written). */
#define _GNU_SOURCE
#include "ecm_api.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/ecm_dc.h"
#include "../diag/ecm_diag.h"
#include "../policy/ecm_policy.h"

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------ handoff --- */
typedef struct { uint64_t v; char pad[100]; } item_t;

static void test_handoff(void)
{
    static item_t s0, s1, out;
    ecm_api_ho_t h;
    ecm_api_ho_init(&h, &s0, &s1, sizeof(item_t));
    CHECK(!ecm_api_ho_get(&h, &out), "empty handoff gives nothing");
    item_t a = { .v = 1 }, b = { .v = 2 }, c = { .v = 3 };
    CHECK(ecm_api_ho_put(&h, &a) == 1, "put 1");
    CHECK(ecm_api_ho_put(&h, &b) == 1, "put 2");
    CHECK(ecm_api_ho_put(&h, &c) == 0 && h.drops == 1, "third put with both slots full is dropped, never waits");
    CHECK(ecm_api_ho_get(&h, &out) && out.v == 1, "older first (got %" PRIu64 ")", out.v);
    CHECK(ecm_api_ho_get(&h, &out) && out.v == 2, "then newer");
    CHECK(!ecm_api_ho_get(&h, &out), "then empty");
    /* begin / commit: no second copy */
    item_t *p = ecm_api_ho_begin(&h);
    CHECK(p != NULL, "begin on a free slot");
    if (p) { p->v = 7; ecm_api_ho_commit(&h); }
    CHECK(ecm_api_ho_get(&h, &out) && out.v == 7, "begin/commit value");
    /* a consumer that drains every time always ends on the newest */
    for (uint64_t i = 10; i < 20; i++) { item_t x = { .v = i }; ecm_api_ho_put(&h, &x); }
    uint64_t last = 0; int n = 0;
    while (ecm_api_ho_get(&h, &out)) { last = out.v; n++; }
    CHECK(n == 2 && last == 11, "two slots kept, newer of them last (n=%d last=%" PRIu64 ")", n, last);
    CHECK(h.drops == 1 + 8, "drops counted (%" PRIu64 ")", h.drops);
}

/* ---------------------------------------------------------------- keys --- */
static void test_keys(void)
{
    for (int a = 0; a <= ECM_FIND_EMCY_LOST; a++) {
        CHECK(strcmp(ecm_api_finding_key(a), "unknown") != 0, "finding %d has a key", a);
        for (int b = 0; b < a; b++)
            CHECK(strcmp(ecm_api_finding_key(a), ecm_api_finding_key(b)) != 0, "finding keys %d %d distinct", a, b);
    }
    CHECK(!strcmp(ecm_api_finding_key(99), "unknown"), "out of range finding");
    CHECK(!strcmp(ecm_api_bus_key(ECM_BUS_RUN), "RUN") && !strcmp(ecm_api_bus_key(ECM_BUS_RECOVER), "RECOVER"), "bus keys");
    CHECK(!strcmp(ecm_api_bus_key(-1), "UNKNOWN"), "bus key out of range");
    CHECK(!strcmp(ecm_api_dc_key(ECM_DC_LOCKED), "LOCKED"), "dc key");
    CHECK(!strcmp(ecm_api_sact_key(ECM_SACT_RECONFIG), "reconfig"), "sact key");
    CHECK(!strcmp(ecm_api_al_key(0x0008), "OP") && !strcmp(ecm_api_al_key(0x0014), "SAFEOP"), "AL keys, ERR bit ignored");
    ecm_api_mon_slave_t s = { 0 };
    CHECK(!strcmp(ecm_api_recover_key(&s), "ok"), "recover ok");
    s.unhealthy = 1; CHECK(!strcmp(ecm_api_recover_key(&s), "retrying"), "recover retrying");
    s.failed = 1;    CHECK(!strcmp(ecm_api_recover_key(&s), "failed"), "recover failed");
    s.failed_config = 1; CHECK(!strcmp(ecm_api_recover_key(&s), "failed_config"), "recover failed_config wins");
}

/* ------------------------------------------------------------ document --- */
static ecm_api_stats_t g_st;

static void fill_full(ecm_api_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->pid = 4242; st->cycle_ns = 1000000;
    snprintf(st->link, sizeof(st->link), "af_packet");
    snprintf(st->path, sizeof(st->path), "single");
    snprintf(st->fsm, sizeof(st->fsm), "RUN");
    snprintf(st->hook, sizeof(st->hook), "cia402");
    snprintf(st->api_cpus, sizeof(st->api_cpus), "1-2");
    st->dma_latency_held = 1;
    st->rtcfg = (ecm_api_rtcfg_t){ .rt_cpu = 3, .have_isolated = 1, .isolated = "3", .idle_driver = "psci_idle",
                                   .nstates = 2, .kernel = "6.8.12-1021-rt-tegra", .nflags = 2,
                                   .flags = { "isolcpus=3", "irqaffinity=0-2" } };
    st->n_slaves = 2; st->nports = 1; st->ngroups = 2; st->dc_ref_pos = 1;
    snprintf(st->ports[0].name, 12, "primary"); snprintf(st->ports[0].iface, 32, "enP1p1s0");
    st->ports[0].link = 1; st->ports[0].crc_err = 3; st->ports[0].rx_missed = -1; st->ports[0].carrier_changes = 2;
    snprintf(st->gcfg[0].name, 12, "motion"); st->gcfg[0].nslaves = 2; st->gcfg[0].slaves[0] = 1; st->gcfg[0].slaves[1] = 2;
    st->gcfg[0].cycle_ns = 1000000;
    snprintf(st->gcfg[1].name, 12, "io"); st->gcfg[1].cycle_ns = 8000000;
    for (int i = 0; i < 2; i++) {
        st->scfg[i].pos = i + 1; st->scfg[i].vendor = 0x00100000; st->scfg[i].product = 0x000C0108;
        snprintf(st->scfg[i].name, 48, i ? "IS620N \"B\"\t" : "IS620N");   /* escaping */
    }
    st->pub_seq = 77; st->mono_ns = 123456789; st->final = 1;
    st->have_rt = 1;
    st->rt.seq = 5; st->rt.tick = 100000; st->rt.ngroups = 2;
    st->rt.g[0].cycles = 100000; st->rt.g[0].wkc[ECM_WKC_OK] = 99990; st->rt.g[0].wkc[ECM_WKC_NOFRAME] = 10;
    st->rt.g[0].overrun = 1; st->rt.g[0].max_run_bad = 3;
    st->rt.g[1].cycles = 12500; st->rt.g[1].wkc[ECM_WKC_OK] = 12500;
    st->rt.dc_on = 1; st->rt.dc_state = ECM_DC_LOCKED; st->rt.dc_locks = 1; st->rt.dc_err_ns = -1234;
    st->rt.txtime_have = 1; st->rt.hook_calls = 100000;
    st->have_tel = 1;
    st->tel.wake_jitter.have = 1; st->tel.wake_jitter.count = 100000; st->tel.wake_jitter.b[10] = 99999; st->tel.wake_jitter.b[40] = 1;
    st->tel.wake_jitter.max_ns = 25000;
    st->tel.prep_send.have = st->tel.occupancy.have = st->tel.turnaround.have = 1;
    st->have_dc = 1; st->dc.h.have = 1; st->dc.h.n = 95000; st->dc.h.b[0] = 90000; st->dc.h.b[3] = 5000;
    st->have_hook = 1; st->hook_h.h.have = 1; st->hook_h.h.count = 100002; st->hook_h.h.b[3] = 100000; st->hook_h.h.b[HIST_OVERFLOW_IDX] = 2;
    st->have_mon = 1; st->mon.diag_on = 1; st->mon.n = 2; st->mon.diag_reads = 100;
    st->mon.s[0].ever_answered = st->mon.s[0].answered = 1; st->mon.s[0].al_status = 0x08; st->mon.s[0].station = 0x1001;
    st->mon.s[0].rx[1] = 4; st->mon.s[0].nemcy = 1; st->mon.s[0].emcy_count = 1;
    st->mon.s[0].emcy[0] = (ecm_api_emcy_t){ .mono_ns = 5, .tick = 6, .code = 0x2310, .reg = 0x02, .data = { 1, 2, 3, 4, 5 } };
    st->mon.s[1].failed_config = 1; st->mon.s[1].initcmd_failed = 1;
    st->mon.nf = 1;
    snprintf(st->mon.f[0].type, 24, "%s", ecm_api_finding_key(ECM_FIND_EMCY));
    snprintf(st->mon.f[0].sev, 8, "warn");
    snprintf(st->mon.f[0].text, 200, "slave 1: EMCY 0x2310");
    st->naxes = 2;
    snprintf(st->ax[0].name, 24, "1:0"); st->ax[0].slave = 1; st->ax[0].valid = 1;
    snprintf(st->ax[0].state, 24, "OPERATION_ENABLED"); snprintf(st->ax[0].err, 16, "none");
    snprintf(st->ax[0].error_src, 8, "none"); st->ax[0].sw = 0x1637; st->ax[0].mode = 8; st->ax[0].mode_display = 8;
    snprintf(st->ax[1].name, 24, "2:0"); st->ax[1].slave = 2; st->ax[1].valid = 0;   /* not readable yet */
}

static char g_buf[1 << 18];

static int write_file(const char *path, const char *s, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t w = fwrite(s, 1, n, f);
    fclose(f);
    return w == n ? 0 : -1;
}

static void test_json(const char *dump_dir)
{
    fill_full(&g_st);
    size_t n = ecm_api_stats_json(&g_st, g_buf, sizeof(g_buf));
    CHECK(n > 0 && n < sizeof(g_buf), "full document fits (%zu B)", n);
    CHECK(strstr(g_buf, "\"schema\":1") != NULL, "schema 1");
    CHECK(strstr(g_buf, "\"ports\":[{\"name\":\"primary\"") != NULL, "ports is a list");
    CHECK(strstr(g_buf, "\"path\":\"single\"") != NULL, "path single");
    CHECK(strstr(g_buf, "\"tx\":112500") != NULL && strstr(g_buf, "\"noframe\":10,\"crc_err\":3") != NULL,
          "one port: tx/noframe = sums over the groups");
    CHECK(strstr(g_buf, "\"rx_missed\":null") != NULL, "unreadable NIC counter -> null");
    CHECK(strstr(g_buf, "IS620N \\\"B\\\"\\u0009") != NULL, "string escaping");
    CHECK(strstr(g_buf, "\"rx\":") == NULL, "no ports[].rx");
    CHECK(strstr(g_buf, "\"recover\":\"failed_config\"") != NULL, "recover failed_config");
    CHECK(strstr(g_buf, "\"counts\":[0,0,0,0,0,0,0,0,0,0,99999") != NULL, "log histogram counts");
    CHECK(strstr(g_buf, "\"dc_err\":{\"unit\":\"us\",\"buckets\":\"lin1us_v1\",\"count\":95000,\"counts\":[90000,0,0,5000]}") != NULL,
          "dc_err trimmed after the last non-zero bin");
    CHECK(strstr(g_buf, "\"tracking_max\"") == NULL, "no tracking_max in v1");
    CHECK(strstr(g_buf, "\"final\":true") != NULL, "final flag");
    CHECK(strstr(g_buf, "\"api_cpus\":\"1-2\",\"rt_cpu\":3,\"isolated_cpus\":\"3\"") != NULL &&
          strstr(g_buf, "\"cmdline_flags\":[\"isolcpus=3\",\"irqaffinity=0-2\"]") != NULL, "rt config in the document");
    CHECK(strstr(g_buf, "\"error_code\":\"0x0000\"") != NULL && strstr(g_buf, "\"sw\":\"0x1637\"") != NULL, "hex as strings");
    CHECK(strstr(g_buf, "{\"name\":\"2:0\",\"slave\":2,\"n\":0}") != NULL, "unreadable axis: identity only");
    if (dump_dir) {
        char p[512];
        snprintf(p, sizeof(p), "%s/full.json", dump_dir);
        CHECK(write_file(p, g_buf, n) == 0, "dump %s", p);
    }
    /* nothing published yet: static part only */
    ecm_api_stats_t *e = calloc(1, sizeof(*e));
    *e = g_st;
    e->have_rt = e->have_tel = e->have_mon = e->have_dc = e->have_hook = 0; e->final = 0; e->naxes = 0;
    size_t m = ecm_api_stats_json(e, g_buf, sizeof(g_buf));
    CHECK(m > 0 && strstr(g_buf, "\"dc\":null") && strstr(g_buf, "\"wake_jitter\":null") && strstr(g_buf, "\"rt\":null"),
          "before the first producer publish: parts null");
    if (dump_dir) {
        char p[512];
        snprintf(p, sizeof(p), "%s/empty.json", dump_dir);
        CHECK(write_file(p, g_buf, m) == 0, "dump %s", p);
    }
    free(e);
    /* too small a buffer: 0, nothing half written */
    char small[256];
    CHECK(ecm_api_stats_json(&g_st, small, sizeof(small)) == 0 && small[0] == '\0', "overflow -> 0");
}

/* ------------------------------------------------------------- sysfs ----- */
static void mkp(const char *root, const char *rel, const char *content)
{
    char p[512], d[512];
    snprintf(p, sizeof(p), "%s/%s", root, rel);
    snprintf(d, sizeof(d), "%s", p);
    for (char *q = d + strlen(root) + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }
    if (content) write_file(p, content, strlen(content));
}

static void test_nic(void)
{
    char root[] = "/tmp/api_sysfs_XXXXXX";
    if (!mkdtemp(root)) { CHECK(0, "mkdtemp"); return; }
    mkp(root, "class/net/enX/statistics/rx_crc_errors", "17\n");
    mkp(root, "class/net/enX/carrier_changes", "4\n");
    mkp(root, "class/net/enX/operstate", "up\n");
    ecm_api_port_t p;
    ecm_api_nic_read(root, "enX", &p);
    CHECK(p.crc_err == 17 && p.carrier_changes == 4 && p.link == 1, "read crc/carrier/up");
    CHECK(p.rx_missed == -1, "missing rx_missed_errors -> -1");
    mkp(root, "class/net/enX/operstate", "down\n");
    ecm_api_nic_read(root, "enX", &p);
    CHECK(p.link == 0, "down");
    ecm_api_nic_read(root, "nope", &p);
    CHECK(p.crc_err == -1 && p.link == -1, "unknown interface -> all -1");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd)) {}
}

static void test_rtcfg(void)
{
    char root[] = "/tmp/api_rt_XXXXXX";
    if (!mkdtemp(root)) { CHECK(0, "mkdtemp"); return; }
    char sys[256], cl[256];
    snprintf(sys, sizeof(sys), "%s/sys", root);
    snprintf(cl, sizeof(cl), "%s/cmdline", root);
    mkp(root, "sys/devices/system/cpu/isolated", "3\n");
    mkp(root, "sys/devices/system/cpu/cpuidle/current_driver", "psci_idle\n");
    mkp(root, "sys/devices/system/cpu/cpu3/cpuidle/state0/name", "WFI\n");
    mkp(root, "sys/devices/system/cpu/cpu3/cpuidle/state0/disable", "0\n");
    mkp(root, "sys/devices/system/cpu/cpu3/cpuidle/state1/name", "c7\n");
    mkp(root, "sys/devices/system/cpu/cpu3/cpuidle/state1/disable", "1\n");
    mkp(root, "cmdline", "root=/dev/nvme0n1p1 rw isolcpus=3 quiet irqaffinity=0-2 transparent_hugepage=never nohz_full=3 console=ttyTCU0\n");
    ecm_api_rtcfg_t r;
    ecm_api_rtcfg_read(sys, cl, "6.8.12-test", 3, &r);
    CHECK(r.have_isolated && !strcmp(r.isolated, "3"), "rt: isolated 3");
    CHECK(!strcmp(r.idle_driver, "psci_idle") && !r.idle_off, "rt: cpuidle driver, not off");
    CHECK(r.nstates == 2 && r.ndisabled == 1 && !strcmp(r.disabled[0], "c7"), "rt: state c7 disabled on cpu3");
    CHECK(r.nflags == 4 && !strcmp(r.flags[0], "isolcpus=3") && !strcmp(r.flags[3], "nohz_full=3"),
          "rt: only RT flags of the cmdline, in order (%d)", r.nflags);
    char j[2048];
    size_t n = ecm_api_rtcfg_json(&r, j, sizeof(j));
    CHECK(n > 0 && strstr(j, "\"rt_cpu\":3,\"isolated_cpus\":\"3\",\"cpuidle\":{\"driver\":\"psci_idle\",\"off\":false,"
                          "\"states\":2,\"states_disabled\":[\"c7\"]},\"kernel\":\"6.8.12-test\"") != NULL &&
          !strstr(j, "root=") && !strstr(j, "console"), "rt: JSON members, no other cmdline word");
    /* cpuidle.off=1: no driver, off */
    mkp(root, "cmdline", "isolcpus=3 cpuidle.off=1\n");
    char rm[512];
    snprintf(rm, sizeof(rm), "%s/sys/devices/system/cpu/cpuidle/current_driver", root);
    unlink(rm);
    ecm_api_rtcfg_read(sys, cl, NULL, 3, &r);
    CHECK(r.idle_off && !r.idle_driver[0] && r.kernel[0], "rt: cpuidle.off=1 -> off, driver null; kernel from uname");
    n = ecm_api_rtcfg_json(&r, j, sizeof(j));
    CHECK(strstr(j, "\"driver\":null,\"off\":true") != NULL, "rt: JSON driver null");
    /* nothing readable: never fails */
    ecm_api_rtcfg_read("/nonexistent", "/nonexistent", "k", 3, &r);
    n = ecm_api_rtcfg_json(&r, j, sizeof(j));
    CHECK(n > 0 && strstr(j, "\"isolated_cpus\":null") && strstr(j, "\"cmdline_flags\":[]"), "rt: nothing readable -> nulls");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd)) {}
}

static void test_cpus(void)
{
    uint64_t m = 0;
    CHECK(ecm_api_parse_cpus("1-2", &m) == 0 && m == 0x6, "1-2");
    CHECK(ecm_api_parse_cpus("1,2", &m) == 0 && m == 0x6, "1,2");
    CHECK(ecm_api_parse_cpus("2", &m) == 0 && m == 0x4, "2");
    CHECK(ecm_api_parse_cpus("0-1,3", &m) == 0 && m == 0xB, "0-1,3");
    CHECK(ecm_api_parse_cpus("", &m) == -1, "empty");
    CHECK(ecm_api_parse_cpus("2-1", &m) == -1, "reversed range");
    CHECK(ecm_api_parse_cpus("1,", &m) == -1, "trailing comma");
    CHECK(ecm_api_parse_cpus("a", &m) == -1, "junk");
    CHECK(ecm_api_parse_cpus("64", &m) == -1, "out of range");
}

/* ------------------------------------------------- publish (tmp + rename) --- */
static char g_dir[64];
static char g_doc[2][1 << 17];
static size_t g_doclen[2];
static volatile int g_stop;
static uint64_t g_reads, g_torn;

static void *reader(void *arg)
{
    (void)arg;
    static char rb[1 << 18];
    char p[128];
    snprintf(p, sizeof(p), "%s/stats.json", g_dir);
    while (!g_stop) {
        FILE *f = fopen(p, "re");
        if (!f) continue;
        size_t n = fread(rb, 1, sizeof(rb), f);
        fclose(f);
        g_reads++;
        int same = (n == g_doclen[0] && !memcmp(rb, g_doc[0], n)) || (n == g_doclen[1] && !memcmp(rb, g_doc[1], n));
        if (!same) g_torn++;
    }
    return NULL;
}

static int test_torn(int writes)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/api_pub_XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 2; }
    for (int d = 0; d < 2; d++) {                       /* two documents, different sizes */
        size_t len = d ? 40000 : 90000;
        memset(g_doc[d], d ? 'b' : 'a', len);
        g_doc[d][0] = '{'; g_doc[d][len - 1] = '\n';
        g_doclen[d] = len;
    }
    if (ecm_api_publish(g_dir, "stats.json", g_doc[0], g_doclen[0]) != 0) { perror("publish"); return 2; }
    pthread_t t;
    pthread_create(&t, NULL, reader, NULL);
    for (int i = 0; i < writes; i++) {
        int r = ecm_api_publish(g_dir, "stats.json", g_doc[i & 1], g_doclen[i & 1]);
        if (r) { printf("publish: %s\n", strerror(-r)); break; }
    }
    g_stop = 1;
    pthread_join(t, NULL);
    printf("torn: %" PRIu64 " of %" PRIu64 " reads (%d writes)\n", g_torn, g_reads, writes);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s/.stats.json.tmp", g_dir);
    struct stat sb;
    int tmp_left = stat(tmp, &sb) == 0;
#ifndef ECM_API_PUBLISH_INPLACE
    if (tmp_left) printf("tmp file left behind\n");
#endif
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (system(cmd)) {}
    return g_torn == 0 && g_reads > 0 && !tmp_left ? 0 : 1;
}

static void test_publish_errors(void)
{
    CHECK(ecm_api_publish("/nonexistent_dir_api", "stats.json", "{}\n", 3) == -ENOENT, "missing dir -> -ENOENT");
    char d[] = "/tmp/api_mk_XXXXXX";
    if (!mkdtemp(d)) return;
    char sub[64];
    snprintf(sub, sizeof(sub), "%s/run", d);
    CHECK(ecm_api_mkdir(sub, 0755) == 0 && ecm_api_mkdir(sub, 0755) == 0, "mkdir, then exists = ok");
    char f[80];
    snprintf(f, sizeof(f), "%s/file", d);
    write_file(f, "x", 1);
    CHECK(ecm_api_mkdir(f, 0755) == -ENOTDIR, "a file in the way -> -ENOTDIR");
    char cmd[100];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", d);
    if (system(cmd)) {}
}

int main(int argc, char **argv)
{
    const char *dump = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--torn") && i + 1 < argc) return test_torn(atoi(argv[++i]));
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    }
    test_handoff();
    test_keys();
    test_json(dump);
    test_nic();
    test_cpus();
    test_rtcfg();
    test_publish_errors();
    int torn = test_torn(2000);
    CHECK(torn == 0, "publish: no torn read (tmp + rename)");
    printf("test_api_offline: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
