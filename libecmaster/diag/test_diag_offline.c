/* ==========================================================================
 * test_diag_offline.c — unit tests for ecm_diag.c (Phase 7.2), no network.
 *
 * Raw reads are synthesised with the SAME counter patterns soft_bus's
 * fault injection produces (esc_fault.c), so a pass here plus a pass of
 * soft_bus's test_fault means the two ends agree on the semantics.
 *
 * Build & run:  make test
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ecm_diag.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, long got, long want)
{
    if (got == want) { printf("  [PASS] %-62s = %ld\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-62s = %ld (expected %ld)\n", name, got, want); g_fail++; }
}

#define DL_MID  0x5A33u     /* soft_bus middle node: link0+link1, comm, loops 2/3 closed */
#define DL_LAST 0x5613u     /* last node: port1 loop closed, no link */
#define OP 0x08

static ecm_diag_raw_t R;

/* A healthy N-slave read in FPRW (clear-on-read) mode. */
static void raw_clean(int n, uint64_t seq)
{
    memset(&R, 0, sizeof(R));
    R.seq = seq; R.t_ns = seq * 1000000000ull; R.frame_ok = 1; R.clear_on_read = 1;
    R.n = n; R.first = 0; R.count = n; R.brd_count = n; R.brd_al_or = OP;
    for (int i = 0; i < n; i++) {
        ecm_diag_raw_slave_t *s = &R.s[i];
        s->station_addr = (uint16_t)(0x1001 + i);
        s->wkc_err = 3; s->wkc_dl = 1; s->wkc_al = 1;
        s->dl_status = (i == n - 1) ? DL_LAST : DL_MID;
        s->al_status = OP;
    }
}

/* soft_bus bad_cable <node k> x cnt, as seen by FPRW (0-based node k). */
static void raw_bad_cable(int n, int k, uint8_t cnt)
{
    R.s[k].err[0] += cnt; R.s[k].err[1] += cnt; R.s[k].err[12] += cnt;
    for (int i = k + 1; i < n; i++) { R.s[i].err[8] += cnt; R.s[i].err[12] += cnt; }
    for (int i = 0; i < n - 1; i++) R.s[i].err[9] += cnt;
}

static int has(const ecm_diag_finding_t *f, int nf, ecm_find_type_t t, int a)
{
    for (int i = 0; i < nf; i++) if (f[i].type == t && f[i].a == a) return i + 1;
    return 0;
}
static int count_type(const ecm_diag_finding_t *f, int nf, ecm_find_type_t t)
{
    int c = 0;
    for (int i = 0; i < nf; i++) c += f[i].type == t;
    return c;
}

static ecm_diag_t D;
static ecm_diag_finding_t F[64];

static void t_wkc(void)
{
    printf("\n[D1] WKC classification\n");
    check("-1 (EC_NOFRAME) -> NOFRAME", ecm_wkc_classify(-1, 12), ECM_WKC_NOFRAME);
    check("0 -> ZERO", ecm_wkc_classify(0, 12), ECM_WKC_ZERO);
    check("9 of 12 -> PARTIAL", ecm_wkc_classify(9, 12), ECM_WKC_PARTIAL);
    check("13 of 12 -> OVER", ecm_wkc_classify(13, 12), ECM_WKC_OVER);
    check("12 of 12 -> OK", ecm_wkc_classify(12, 12), ECM_WKC_OK);
    check("0 of 0 (group without PD) -> OK", ecm_wkc_classify(0, 0), ECM_WKC_OK);
    ecm_wkc_stats_t st; memset(&st, 0, sizeof(st));
    int seq[] = { 12, 9, 9, 12, -1, -1, -1, 12, 0 };
    for (unsigned k = 0; k < sizeof(seq) / sizeof(seq[0]); k++) ecm_wkc_account(&st, seq[k], 12);
    check("counts: OK", (long)st.count[ECM_WKC_OK], 3);
    check("counts: PARTIAL", (long)st.count[ECM_WKC_PARTIAL], 2);
    check("counts: NOFRAME", (long)st.count[ECM_WKC_NOFRAME], 3);
    check("last_missing = 3", st.last_missing, 3);
    check("max consecutive bad = 3", st.max_run_bad, 3);
    check("current run of bad = 1", st.run_bad, 1);
}

static void t_clean(void)
{
    printf("\n[D2] Healthy bus: no findings\n");
    ecm_diag_init(&D, 8, OP);
    raw_clean(8, 1); ecm_diag_ingest(&D, &R);
    int nf = ecm_diag_analyze(&D, F, 64);
    check("no findings", nf, 0);
}

static void t_cable(void)
{
    printf("\n[D3] bad_cable into node 5 (slave 6), 2 frames (L5-06)\n");
    ecm_diag_init(&D, 8, OP);
    raw_clean(8, 1); raw_bad_cable(8, 5, 2); ecm_diag_ingest(&D, &R);
    int nf = ecm_diag_analyze(&D, F, 64);
    int k = has(F, nf, ECM_FIND_CABLE_FWD, 6);
    check("CABLE_FWD at slave 6 (between slave 5 and 6)", k != 0, 1);
    check("... count 2", k ? (long)F[k - 1].count : -1, 2);
    check("... active", k ? F[k - 1].active : -1, 1);
    check("exactly one cable finding (forwarded errors not blamed)",
          count_type(F, nf, ECM_FIND_CABLE_FWD) + count_type(F, nf, ECM_FIND_CABLE_RET), 1);
    check("no FWD_NO_ORIGIN (origin is known)", count_type(F, nf, ECM_FIND_FWD_NO_ORIGIN), 0);
    check("no PU_ONLY (processing unit errors come with port errors)", count_type(F, nf, ECM_FIND_PU_ONLY), 0);
    char line[256];
    ecm_diag_finding_str(&F[k - 1], NULL, line, sizeof(line));
    printf("         -> \"%s\"\n", line);
    check("text names slave 5 and slave 6", strstr(line, "between slave 5 and slave 6") != NULL, 1);

    raw_clean(8, 2); ecm_diag_ingest(&D, &R);   /* clean second read: history kept, not active */
    nf = ecm_diag_analyze(&D, F, 64);
    k = has(F, nf, ECM_FIND_CABLE_FWD, 6);
    check("after a clean read: still reported (total 2)", k ? (long)F[k - 1].count : -1, 2);
    check("... but no longer active", k ? F[k - 1].active : -1, 0);
    check("... and downgraded to WARN", k ? (long)F[k - 1].sev : -1, ECM_SEV_WARN);

    printf("  -- master -> slave 1 and return path --\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1); raw_bad_cable(4, 0, 1); ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("fault into node 0 -> CABLE_FWD slave 1 (master side)", has(F, nf, ECM_FIND_CABLE_FWD, 1) != 0, 1);
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1);
    R.s[2].err[2] = 4; R.s[2].err[3] = 4;                 /* slave 3 port 1 first detection */
    R.s[1].err[9] = 4; R.s[0].err[9] = 4;                 /* forwarded upstream on return */
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    k = has(F, nf, ECM_FIND_CABLE_RET, 3);
    check("return-path fault at slave 3 port 1 -> CABLE_RET slave 3", k != 0, 1);
    check("... port 1", k ? F[k - 1].b : -1, 1);

    printf("  -- negative control: forwarded errors only --\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1); R.s[2].err[8] = 5; R.s[3].err[8] = 5;
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("no CABLE finding without an origin", count_type(F, nf, ECM_FIND_CABLE_FWD), 0);
    check("FWD_NO_ORIGIN from slave 3", has(F, nf, ECM_FIND_FWD_NO_ORIGIN, 3) != 0, 1);
}

static void t_topology(void)
{
    printf("\n[D4] drop_node 2 (slave 3) of 4 -> chain broken after slave 2 (L5-11)\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1);
    R.brd_count = 2;
    R.s[1].dl_status = DL_LAST;                           /* port 1 link down, loop closed */
    R.s[1].err[17] = 1;                                   /* lost link port 1 */
    for (int i = 2; i < 4; i++) { R.s[i].wkc_err = R.s[i].wkc_dl = R.s[i].wkc_al = 0; }
    ecm_diag_ingest(&D, &R);
    int nf = ecm_diag_analyze(&D, F, 64);
    int k = has(F, nf, ECM_FIND_CHAIN_BROKEN, 2);
    check("CHAIN_BROKEN after slave 2", k != 0, 1);
    check("it is the first (most severe) finding", F[0].type, ECM_FIND_CHAIN_BROKEN);
    check("LOST_LINK slave 2 port 1", has(F, nf, ECM_FIND_LOST_LINK, 2) != 0, 1);
    check("slaves 3,4 beyond the break are NOT reported as NO_ADDRESS",
          count_type(F, nf, ECM_FIND_NO_ADDRESS), 0);
    char line[256];
    ecm_diag_finding_str(&F[k - 1], NULL, line, sizeof(line));
    printf("         -> \"%s\"\n", line);

    printf("  -- link up but slave missing --\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1); R.brd_count = 2;
    for (int i = 2; i < 4; i++) { R.s[i].wkc_err = R.s[i].wkc_dl = R.s[i].wkc_al = 0; }
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("MISSING_LINK_UP after slave 2", has(F, nf, ECM_FIND_MISSING_LINK_UP, 2) != 0, 1);

    printf("  -- restore_node: slave 3 back, station address 0 --\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1);
    R.s[2].wkc_err = R.s[2].wkc_dl = R.s[2].wkc_al = 0;
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    k = has(F, nf, ECM_FIND_NO_ADDRESS, 3);
    check("NO_ADDRESS slave 3 (BRD counts 4, FPRD 0x1003 unanswered)", k != 0, 1);
    check("... carries the expected address 0x1003", k ? (long)F[k - 1].c : -1, 0x1003);
    check("no CHAIN_BROKEN", count_type(F, nf, ECM_FIND_CHAIN_BROKEN), 0);

    printf("  -- nothing answers --\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1); R.brd_count = 0;
    for (int i = 0; i < 4; i++) { R.s[i].wkc_err = R.s[i].wkc_dl = R.s[i].wkc_al = 0; }
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("BRD 0 -> CHAIN_BROKEN a=0 (master side)", has(F, nf, ECM_FIND_CHAIN_BROKEN, 0) != 0, 1);
}

static const char *fake_al_str(uint16_t c) { return c == 0x001B ? "Sync manager watchdog" : "other"; }

static void t_state(void)
{
    printf("\n[D5] AL state: safeop 1 0x1A and SM watchdog 0x1B (L5-05, L5-13)\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1);
    R.s[1].al_status = 0x14; R.s[1].al_code = 0x001A;
    R.s[3].al_status = 0x14; R.s[3].al_code = 0x001B;
    ecm_diag_ingest(&D, &R);
    int nf = ecm_diag_analyze(&D, F, 64);
    int k2 = has(F, nf, ECM_FIND_STATE, 2), k4 = has(F, nf, ECM_FIND_STATE, 4);
    check("STATE slave 2", k2 != 0, 1);
    check("... code 0x001A", k2 ? (long)F[k2 - 1].c : -1, 0x1A);
    check("STATE slave 4 code 0x001B", k4 ? (long)F[k4 - 1].c : -1, 0x1B);
    char line[256];
    ecm_diag_finding_str(&F[k4 - 1], fake_al_str, line, sizeof(line));
    printf("         -> \"%s\"\n", line);
    check("text: SAFEOP+ERR and the decoded code",
          strstr(line, "SAFEOP+ERR") && strstr(line, "0x001B") && strstr(line, "watchdog"), 1);

    ecm_diag_init(&D, 2, 0);                             /* standalone: no expected state */
    raw_clean(2, 1); R.s[0].al_status = 0x02;
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("expected_state 0: PREOP without ERR is not a finding", nf, 0);
}

static void t_accumulate(void)
{
    printf("\n[D6] Counter accumulation\n");
    ecm_diag_init(&D, 1, OP);
    raw_clean(1, 1); R.s[0].err[0] = 3; ecm_diag_ingest(&D, &R);
    raw_clean(1, 2); R.s[0].err[0] = 2; ecm_diag_ingest(&D, &R);
    check("FPRW (clear on read): 3 + 2 = 5", (long)D.s[0].tot.inv[0], 5);

    ecm_diag_init(&D, 1, 0);
    raw_clean(1, 1); R.clear_on_read = 0; R.s[0].wkc_err = 1; R.s[0].err[0] = 3; ecm_diag_ingest(&D, &R);
    check("FPRD first read: value taken as is (3)", (long)D.s[0].tot.inv[0], 3);
    R.seq = 2; R.s[0].err[0] = 5; ecm_diag_ingest(&D, &R);
    check("FPRD 3 -> 5: +2 (total 5)", (long)D.s[0].tot.inv[0], 5);
    R.seq = 3; R.s[0].err[0] = 1; ecm_diag_ingest(&D, &R);
    check("FPRD 5 -> 1 (cleared / power cycle): +1 (total 6)", (long)D.s[0].tot.inv[0], 6);
    R.seq = 4; R.s[0].err[14] = 0x55; ecm_diag_ingest(&D, &R);
    check("PDI error code bytes are not counted", (long)D.s[0].tot.pdi, 0);

    ecm_diag_init(&D, 1, OP);
    raw_clean(1, 1); R.s[0].err[1] = 0xFF; ecm_diag_ingest(&D, &R);
    int nf = ecm_diag_analyze(&D, F, 64);
    check("0xFF -> SATURATED finding", has(F, nf, ECM_FIND_SATURATED, 1) != 0, 1);

    ecm_diag_init(&D, 2, OP);
    raw_clean(2, 1); R.frame_ok = 0; R.frames_lost = 3; ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("lost diag frame: nothing judged, DIAG_LOST info only", nf == 1 && F[0].type == ECM_FIND_DIAG_LOST, 1);

    printf("  -- chunked read (slaves 3..4 of 4 only) --\n");
    ecm_diag_init(&D, 4, OP);
    raw_clean(4, 1); R.first = 2; R.count = 2; R.s[0].wkc_err = 0; R.s[0].wkc_al = 0;
    ecm_diag_ingest(&D, &R);
    nf = ecm_diag_analyze(&D, F, 64);
    check("slaves outside the chunk are not flagged NO_ADDRESS", count_type(F, nf, ECM_FIND_NO_ADDRESS), 0);
}

static void t_handoff(void)
{
    printf("\n[D7] RT -> monitor handoff (2 slots, never blocks the producer)\n");
    static ecm_diag_handoff_t h;
    static ecm_diag_raw_t a, out;
    ecm_diag_handoff_init(&h);
    check("empty -> get 0", ecm_diag_handoff_get(&h, &out), 0);
    memset(&a, 0, sizeof(a));
    a.seq = 1; check("put 1", ecm_diag_handoff_put(&h, &a), 1);
    a.seq = 2; check("put 2", ecm_diag_handoff_put(&h, &a), 1);
    a.seq = 3; check("put 3 with both slots full -> dropped", ecm_diag_handoff_put(&h, &a), 0);
    check("drops = 1", (long)h.drops, 1);
    ecm_diag_handoff_get(&h, &out); check("get returns the oldest (seq 1)", (long)out.seq, 1);
    a.seq = 4; ecm_diag_handoff_put(&h, &a);
    ecm_diag_handoff_get(&h, &out); check("then seq 2", (long)out.seq, 2);
    ecm_diag_handoff_get(&h, &out); check("then seq 4", (long)out.seq, 4);
    check("empty again", ecm_diag_handoff_get(&h, &out), 0);
}

static void t_format(void)
{
    printf("\n[D8] Snapshot text + atomic file write\n");
    ecm_diag_init(&D, 8, OP);
    raw_clean(8, 7); raw_bad_cable(8, 5, 2);
    R.ngroups = 2;
    ecm_wkc_account(&R.wkc[0], 12, 12); ecm_wkc_account(&R.wkc[0], -1, 12);
    ecm_diag_ingest(&D, &R);
    int nf = ecm_diag_analyze(&D, F, 64);
    static char buf[16384];
    const char *names[2] = { "motion", "io" };
    size_t len = ecm_diag_format(&D, F, nf, names, "test", NULL, buf, sizeof(buf));
    printf("%s", buf);
    check("header with t_mono", ecm_diag_snapshot_time(buf) > 6.9 && ecm_diag_snapshot_time(buf) < 7.1, 1);
    check("wkc line for motion with noframe=1", strstr(buf, "wkc motion  ok=1 noframe=1") != NULL, 1);
    check("8 slave rows", (long)(strstr(buf, "\n    8 0x1008") != NULL), 1);
    check("finding line present", strstr(buf, "between slave 5 and slave 6") != NULL, 1);
    size_t small = ecm_diag_format(&D, F, nf, names, "test", NULL, buf, 64);
    check("tiny buffer: truncated, still terminated", small == 63 && buf[63] == '\0', 1);

    const char *path = "/tmp/test_diag_snapshot.txt";
    len = ecm_diag_format(&D, F, nf, names, "test", NULL, buf, sizeof(buf));
    check("write_file returns 0", ecm_diag_write_file(path, buf, len), 0);
    FILE *fp = fopen(path, "r");
    static char rd[16384];
    size_t got = fp ? fread(rd, 1, sizeof(rd), fp) : 0;
    if (fp) fclose(fp);
    check("file content == snapshot", got == len && memcmp(rd, buf, len) == 0, 1);
    fp = fopen("/tmp/test_diag_snapshot.txt.tmp", "r");
    check("no .tmp left behind", fp == NULL, 1);
    if (fp) fclose(fp);
    remove(path);
}

int main(void)
{
    printf("=========================================================\n");
    printf(" test_diag_offline — ecm_diag model, NO network required\n");
    printf("=========================================================\n");
    t_wkc();
    t_clean();
    t_cable();
    t_topology();
    t_state();
    t_accumulate();
    t_handoff();
    t_format();
    printf("\n=========================================================\n");
    printf(" RESULT: %d pass, %d fail\n", g_pass, g_fail);
    printf("=========================================================\n");
    return g_fail == 0 ? 0 : 1;
}
