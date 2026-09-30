/* ==========================================================================
 * l4_test.c — automated L4-01..L4-05 test tool (Giai doan 5, mailbox/CoE).
 *
 * Exercises the REAL SOEM ecx_SDOread/ecx_SDOwrite path (through
 * libecmaster/mailbox/ecm_mailbox.c, already validated end-to-end
 * against soft_bus in this project's chat log) rather than a synthetic
 * byte-level harness -- this is what proves the whole stack (SOEM ->
 * mailbox thread -> RT-style pump thread -> soft_bus's CoE responder ->
 * back) end to end, on top of the unit-level esc_coe.c smoke test that
 * already exists.
 *
 * Test table (master_plan_v2.md, roadmap_master_ethercat.md):
 *   L4-01  PREOP  SDOread 0x1018:01           -- matches SII Vendor ID
 *   L4-02  PREOP  SDOwrite 0x8000:01 (PID)     -- soft_bus stores it (read back)
 *   L4-03  OP     SDO while PDO cyclic runs    -- PDO not interrupted, WKC steady
 *   L4-04  --     SDOread unknown object       -- Abort, rc == -4
 *   L4-05  --     SDOread 0x8001:00 (200 byte) -- segmented, reassembled correctly
 *   L4-07  OP     SDOwrite 0x8002:00, 16 byte (normal) and 250 byte (init +
 *                 2 segments), read back identical; write to the read-only
 *                 0x8001 -> abort 0x06010002 (GD9.3)
 *   L4-08  PREOP/OP  --ca only (soft_bus --coe-ca): Complete Access read of
 *                 0x1C12 = 01 00 00 16, CA write of it in PREOP ok, in OP
 *                 refused with abort 0x08000022 (GD9.3)
 *
 * L4-06 (SM watchdog) is covered separately in ecm_run.c + a tshark
 * capture, not repeated here.
 *
 * Deliberately uses a SINGLE group (group 0, logstartaddr always 0) for
 * every slave -- this test tool cares about mailbox/CoE correctness,
 * not motion/io timing separation, so there is no reason to reproduce
 * ecm_run.c's two-group split (and its associated logstartaddr
 * workaround, which is a no-op for group 0 anyway but kept below,
 * commented, for anyone who copies this into a multi-group tool later).
 *
 * Build:  make            (see Makefile in this directory)
 * Run:    sudo ./l4_test --iface veth_m --n 8
 * ========================================================================== */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "soem/soem.h"
#include "libecmaster/mailbox/ecm_mailbox.h"

#define IOMAP_SIZE (256 * 1024)
static uint8 IOmap[IOMAP_SIZE];
static ecx_contextt ctx;
static ecm_mailbox_t *g_mbx = NULL;

static volatile sig_atomic_t g_mailbox_stop = 0;
static volatile sig_atomic_t g_cyclic_stop  = 0;

/* ---- shared cyclic-loop counters, read by the main/test thread,
 * written by cyclic_thread_fn(). A test tool, not the RT master -- a
 * plain mutex here is fine (no RT-thread-vs-non-RT constraint applies:
 * neither side of this lock is a SCHED_FIFO thread). ---- */
static pthread_mutex_t g_stats_mtx = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    uint64_t cycles;
    uint64_t wkc_mismatch;
    long     expected_wkc;
} cyclic_stats_t;
static cyclic_stats_t g_stats;

static void stats_snapshot(cyclic_stats_t *out)
{
    pthread_mutex_lock(&g_stats_mtx);
    *out = g_stats;
    pthread_mutex_unlock(&g_stats_mtx);
}

/* ---- pass/fail bookkeeping ---- */
static int g_pass = 0, g_fail = 0;

static void report(const char *test_id, bool ok, const char *detail)
{
    printf("[%s] %s -- %s\n", ok ? "PASS" : "FAIL", test_id, detail);
    if (ok) g_pass++; else g_fail++;
}

/* ==========================================================================
 * cyclic_thread_fn -- drives process data + mailbox pump for group 0,
 * continuously, from just before SAFEOP is requested until the test run
 * is done. This is what makes L4-03 meaningful: it is the SAME kind of
 * background cyclic activity ecm_run.c's RT thread provides, just
 * without RT scheduling (this tool doesn't need RT guarantees, only to
 * observe whether an in-flight SDO disturbs it).
 * ========================================================================== */
static void *cyclic_thread_fn(void *arg)
{
    (void)arg;
    struct timespec period = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 }; /* 2ms */

    while (!g_cyclic_stop) {
        ecx_send_processdata_group(&ctx, 0);
        int wkc = ecx_receive_processdata_group(&ctx, 0, EC_TIMEOUTRET);
        ecm_mailbox_rt_pump_group(&ctx, 0, NULL, NULL);

        pthread_mutex_lock(&g_stats_mtx);
        g_stats.cycles++;
        if (wkc != g_stats.expected_wkc) g_stats.wkc_mismatch++;
        pthread_mutex_unlock(&g_stats_mtx);

        nanosleep(&period, NULL);
    }
    return NULL;
}

static void *mailbox_thread_fn(void *arg)
{
    (void)arg;
    ecm_mailbox_run(g_mbx, &g_mailbox_stop);
    return NULL;
}

/* ==========================================================================
 * Individual tests. Each returns nothing -- outcome goes through
 * report(). slave is 1-based (SOEM convention), same slave used
 * throughout (slave 1) since every node in soft_bus carries the same
 * CoE object dictionary (see tools/soft_bus/esc_coe.c).
 * ========================================================================== */

/* L4-01: expedited upload, Identity Vendor ID, compared against a
 * caller-supplied expected value (default matches
 * tools/soft_bus/esc_sii.h's SII_VENDOR_ID placeholder) -- NOT read
 * from ctx.slavelist[slave] directly: this tool does not depend on a
 * specific SOEM internal field name for the EEPROM-cached Vendor ID,
 * to avoid repeating the ecx_mbxinhandler/outhandler "used but not
 * declared" surprise from earlier in this project with an unverified
 * field name too. If you want a stronger, fully self-contained check
 * later, confirm the exact field name against ec_main.h first. */
static void test_l4_01(uint16_t slave, uint32_t expected_vendor_id)
{
    uint8_t buf[4] = {0};
    int size = sizeof(buf);
    int rc = ecm_mailbox_sdo_read(g_mbx, slave, 0x1018, 1, false, buf, &size, 500000);

    uint32_t got = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                 | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);

    char detail[128];
    snprintf(detail, sizeof(detail),
        "SDOread 0x1018:01 rc=%d size=%d got=0x%08x expected=0x%08x",
        rc, size, got, expected_vendor_id);
    report("L4-01", rc == 0 && size == 4 && got == expected_vendor_id, detail);
}

/* L4-02: expedited download to a writable test object (0x8000:01,
 * "Kp" in tools/soft_bus/esc_coe.c), then read it back on the SAME
 * object to confirm soft_bus actually stored it -- stronger than just
 * checking rc==0 on the write (which only proves the ack, not that the
 * value landed), and exercises the real SOEM path end to end rather
 * than the synthetic one esc_coe.c's own smoke test already covers. */
static void test_l4_02(uint16_t slave)
{
    const uint32_t test_value = 0xCAFEBABEu;
    uint8_t wbuf[4] = {
        (uint8_t)(test_value), (uint8_t)(test_value >> 8),
        (uint8_t)(test_value >> 16), (uint8_t)(test_value >> 24)
    };
    int wrc = ecm_mailbox_sdo_write(g_mbx, slave, 0x8000, 1, false, wbuf, 4, 500000);

    uint8_t rbuf[4] = {0};
    int rsize = sizeof(rbuf);
    int rrc = ecm_mailbox_sdo_read(g_mbx, slave, 0x8000, 1, false, rbuf, &rsize, 500000);
    uint32_t got = (uint32_t)rbuf[0] | ((uint32_t)rbuf[1] << 8)
                 | ((uint32_t)rbuf[2] << 16) | ((uint32_t)rbuf[3] << 24);

    char detail[160];
    snprintf(detail, sizeof(detail),
        "SDOwrite 0x8000:01=0x%08x wrc=%d, read back rrc=%d size=%d got=0x%08x",
        test_value, wrc, rrc, rsize, got);
    report("L4-02", wrc == 0 && rrc == 0 && rsize == 4 && got == test_value, detail);
}

/* L4-03: snapshots the cyclic thread's own counters immediately before
 * an in-flight SDO call, then again a short, fixed while after it
 * returns (not immediately after) -- this makes the test robust to how
 * FAST the SDO round-trip happens to be (on real hardware it's tens of
 * us; a naive "snapshot right after rc comes back" check could see
 * zero elapsed cycles purely because nothing had time to run yet, and
 * wrongly fail even though nothing was actually disturbed). PASS means
 * wkc_mismatch did not move across that whole window AND at least one
 * process-data cycle actually happened in it (proves the loop kept
 * running, not just that it stayed silent because it was stalled). */
static void test_l4_03(uint16_t slave)
{
    cyclic_stats_t before, after;
    stats_snapshot(&before);

    uint8_t buf[4] = {0};
    int size = sizeof(buf);
    int rc = ecm_mailbox_sdo_read(g_mbx, slave, 0x1018, 1, false, buf, &size, 500000);

    usleep(20000); /* 20ms, ~10 cyclic_thread_fn periods -- gives the
                     * loop room to prove it's still alive regardless
                     * of how quickly the SDO itself finished. */
    stats_snapshot(&after);

    uint64_t cycles_during = after.cycles - before.cycles;
    uint64_t new_mismatches = after.wkc_mismatch - before.wkc_mismatch;

    char detail[160];
    snprintf(detail, sizeof(detail),
        "SDOread rc=%d, PDO cyclic thread checked across the call + 20ms after -- "
        "cycles_during=%" PRIu64 " new_wkc_mismatch=%" PRIu64,
        rc, cycles_during, new_mismatches);
    report("L4-03", rc == 0 && cycles_during > 0 && new_mismatches == 0, detail);
}

/* L4-04: object 0x9999 exists nowhere in soft_bus's OD (see
 * coe_lookup_readable() in esc_coe.c) -- expect the Abort path,
 * surfaced by ecm_mailbox_sdo_read() as rc == -4 (see ecm_mailbox.h's
 * documented return codes).
 *
 * Giai doan 9.0: rc == -4 alone is not enough -- ecx_SDOread() returns a
 * WKC <= 0 for an Abort AND for other protocol failures (wrong service,
 * timeout on a segment). The Abort code itself is in SOEM's error list
 * (ecx_SDOerror -> ecx_pusherror). CiA 301 abort 0x06020000 = "object does
 * not exist in the object dictionary". In this tool main() is the only
 * reader of the error list; the cyclic thread may push to it (mailbox
 * emergencies), soft_bus sends none. ecm_run leaves draining it to one
 * thread (see ecm_mailbox.c execute_job). */
#ifndef L4_04_ABORT_EXPECTED
#define L4_04_ABORT_EXPECTED 0x06020000   /* -D override only for the negative control */
#endif
static void test_l4_04(uint16_t slave)
{
    uint8_t buf[4] = {0};
    int size = sizeof(buf);
    ec_errort e;
    while (ecx_poperror(&ctx, &e)) { }      /* start from an empty error list */
    int rc = ecm_mailbox_sdo_read(g_mbx, slave, 0x9999, 0, false, buf, &size, 500000);

    int found = 0, others = 0;
    uint32_t code = 0;
    while (ecx_poperror(&ctx, &e)) {
        if (e.Etype == EC_ERR_TYPE_SDO_ERROR && e.Slave == slave && e.Index == 0x9999 && e.SubIdx == 0) {
            found++;
            code = (uint32_t)e.AbortCode;
        } else {
            others++;
        }
    }
    char detail[160];
    snprintf(detail, sizeof(detail),
             "SDOread 0x9999:00 (unknown object) rc=%d (expect -4), abort 0x%08" PRIX32
             " (expect 0x%08X, %d SDO error(s) for it, %d other error(s))",
             rc, code, (unsigned)L4_04_ABORT_EXPECTED, found, others);
    report("L4-04", rc == -4 && found == 1 && code == L4_04_ABORT_EXPECTED, detail);
}

/* L4-05: object 0x8001:00 is a fixed 200 byte, i%256 pattern blob in
 * soft_bus (see COE_SEGTEST_BLOB_SIZE / coe_od_init() in esc_coe.c),
 * deliberately larger than one mailbox buffer so this can ONLY
 * complete via genuine multi-frame segmentation, transparent to the
 * caller through ecx_SDOread's own segmented-transfer loop. */
static void test_l4_05(uint16_t slave)
{
    uint8_t buf[256] = {0};
    int size = sizeof(buf);
    int rc = ecm_mailbox_sdo_read(g_mbx, slave, 0x8001, 0, false, buf, &size, 500000);

    int pattern_ok = 1;
    for (int i = 0; i < size; i++) {
        if (buf[i] != (uint8_t)(i & 0xFF)) { pattern_ok = 0; break; }
    }

    char detail[128];
    snprintf(detail, sizeof(detail),
        "SDOread 0x8001:00 rc=%d size=%d (expect 200) pattern_ok=%d",
        rc, size, pattern_ok);
    report("L4-05", rc == 0 && size == 200 && pattern_ok, detail);
}

/* Pops every queued error; returns how many were SDO aborts for
 * slave/index and the last such abort code. */
static int pop_sdo_aborts(uint16_t slave, uint16_t index, uint32_t *code, int *others)
{
    ec_errort e;
    int found = 0;
    *others = 0;
    while (ecx_poperror(&ctx, &e)) {
        if (e.Etype == EC_ERR_TYPE_SDO_ERROR && e.Slave == slave && e.Index == index) {
            found++;
            *code = (uint32_t)e.AbortCode;
        } else {
            (*others)++;
        }
    }
    return found;
}

/* L4-07 (GD9.3): normal and segmented SDO download. 0x8002 is a RW
 * OCTET_STRING in soft_bus (1..400 byte). 16 byte fit the init frame
 * (normal transfer); 250 byte need the init frame (112) and two download
 * segments (119 + 19) with 128 byte mailboxes. Runs in OP, so the
 * segments share the bus with cyclic process data. */
static void l4_07_one(uint16_t slave, int len, const char *id)
{
    uint8_t w[ECM_MBX_MAX_DATA], r[ECM_MBX_MAX_DATA];
    for (int i = 0; i < len; i++) w[i] = (uint8_t)(0x5A ^ (i * 13 + len));
    int wrc = ecm_mailbox_sdo_write(g_mbx, slave, 0x8002, 0, false, w, len, 500000);
    int size = sizeof(r);
    memset(r, 0, sizeof(r));
    int rrc = ecm_mailbox_sdo_read(g_mbx, slave, 0x8002, 0, false, r, &size, 500000);
    int same = size == len && memcmp(r, w, (size_t)len) == 0;
    char detail[160];
    snprintf(detail, sizeof(detail), "SDOwrite 0x8002:00 %d byte wrc=%d, read back rrc=%d size=%d identical=%d",
             len, wrc, rrc, size, same);
    report(id, wrc == 0 && rrc == 0 && same, detail);
}

static void test_l4_07(uint16_t slave)
{
    l4_07_one(slave, 16, "L4-07 normal download (16 byte)");
    l4_07_one(slave, 250, "L4-07 segmented download (250 byte, 2 segments)");

    uint8_t w[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ec_errort e;
    while (ecx_poperror(&ctx, &e)) { }
    int wrc = ecm_mailbox_sdo_write(g_mbx, slave, 0x8001, 0, false, w, sizeof(w), 500000);
    uint32_t code = 0;
    int others = 0;
    int found = pop_sdo_aborts(slave, 0x8001, &code, &others);
    char detail[160];
    snprintf(detail, sizeof(detail), "SDOwrite 0x8001 (read-only) wrc=%d abort 0x%08" PRIX32
             " (expect 0x06010002, %d abort(s), %d other error(s))", wrc, code, found, others);
    report("L4-07 write to read-only object refused", wrc != 0 && found == 1 && code == 0x06010002u, detail);
}

/* L4-08 (GD9.3, --ca): Complete Access against soft_bus --coe-ca. */
static void test_l4_08_preop(uint16_t slave)
{
    uint8_t r[8] = { 0 };
    int size = sizeof(r);
    int rrc = ecm_mailbox_sdo_read(g_mbx, slave, 0x1C12, 0, true, r, &size, 500000);
    char detail[160];
    snprintf(detail, sizeof(detail), "CA SDOread 0x1C12 rc=%d size=%d bytes %02x %02x %02x %02x (expect 01 00 00 16)",
             rrc, size, r[0], r[1], r[2], r[3]);
    report("L4-08 CA read 0x1C12 (SI0 16 bit)", rrc == 0 && size == 4 && r[0] == 1 && r[1] == 0 &&
                                                r[2] == 0x00 && r[3] == 0x16, detail);

    uint8_t w[4] = { 1, 0, 0x00, 0x16 };
    int wrc = ecm_mailbox_sdo_write(g_mbx, slave, 0x1C12, 0, true, w, 4, 500000);
    snprintf(detail, sizeof(detail), "CA SDOwrite 0x1C12 = 01 00 00 16 in PREOP rc=%d", wrc);
    report("L4-08 CA write PDO assign in PREOP", wrc == 0, detail);
}

static void test_l4_08_op(uint16_t slave)
{
    uint8_t w[4] = { 1, 0, 0x00, 0x16 };
    ec_errort e;
    while (ecx_poperror(&ctx, &e)) { }
    int wrc = ecm_mailbox_sdo_write(g_mbx, slave, 0x1C12, 0, true, w, 4, 500000);
    uint32_t code = 0;
    int others = 0;
    int found = pop_sdo_aborts(slave, 0x1C12, &code, &others);
    char detail[160];
    snprintf(detail, sizeof(detail), "CA SDOwrite 0x1C12 in OP rc=%d abort 0x%08" PRIX32
             " (expect 0x08000022, %d abort(s), %d other error(s))", wrc, code, found, others);
    report("L4-08 CA write PDO assign in OP refused", wrc != 0 && found == 1 && code == 0x08000022u, detail);
}

/* ==========================================================================
 * Bring-up, sequencing, teardown.
 * ========================================================================== */
/* L5-12 (Giai doan 7.4): mailbox repeat request and duplicated responses.
 * Round i writes 0x8000:01 = 0x10000+i and reads it back. Every 10 rounds
 * soft_bus is told (control FIFO) to post the next response of node 0
 * twice (mbx_dup) and, 5 rounds later, to lose the frame carrying the next
 * one (mbx_repeat -> SOEM's repeat request). A read that returns another
 * round's value took a response twice; the master must never do that. */
static void test_l5_12(uint16_t slave, int rounds, const char *ctl)
{
    int fd = ctl ? open(ctl, O_WRONLY | O_NONBLOCK) : -1;
    if (ctl && fd < 0) { perror("open ctl fifo"); }
    int ok = 0, wrong = 0, err = 0, dup_inj = 0, rep_inj = 0, first_bad = -1;
    for (int i = 1; i <= rounds; i++) {
        if (fd >= 0 && i % 10 == 5) { if (write(fd, "mbx_dup 0\n", 10) == 10) dup_inj++; }
        if (fd >= 0 && i % 10 == 0) { if (write(fd, "mbx_repeat 0\n", 13) == 13) rep_inj++; }
        uint32_t v = 0x10000u + (uint32_t)i;
        uint8_t wbuf[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        int wrc = ecm_mailbox_sdo_write(g_mbx, slave, 0x8000, 1, false, wbuf, 4, 500000);
        uint8_t rbuf[4] = { 0 };
        int rsize = sizeof(rbuf);
        int rrc = ecm_mailbox_sdo_read(g_mbx, slave, 0x8000, 1, false, rbuf, &rsize, 500000);
        uint32_t got = (uint32_t)rbuf[0] | ((uint32_t)rbuf[1] << 8) | ((uint32_t)rbuf[2] << 16) | ((uint32_t)rbuf[3] << 24);
        if (wrc != 0 || rrc != 0) { err++; if (first_bad < 0) first_bad = i; }
        else if (got != v) {
            wrong++;
            if (first_bad < 0) first_bad = i;
            if (wrong <= 5) printf("  round %d: read 0x%x, expected 0x%x (a response of round %d)\n",
                                   i, got, v, (int)(got - 0x10000u));
        } else ok++;
    }
    if (fd >= 0) close(fd);
    char detail[200];
    snprintf(detail, sizeof(detail), "rounds=%d ok=%d WRONG_VALUE=%d errors=%d (injected: dup=%d repeat=%d; first bad round %d)",
             rounds, ok, wrong, err, dup_inj, rep_inj, first_bad);
    report("L5-12 no response processed twice", wrong == 0, detail);
    report("L5-12 every round completes", err == 0, detail);
}

static int request_all_state(int target, int timeout_us)
{
    ctx.slavelist[0].state = target;
    ecx_writestate(&ctx, 0);
    return ecx_statecheck(&ctx, 0, target, timeout_us);
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    int n = 8;
    uint32_t expected_vendor_id = 0x00000499u; /* SII_VENDOR_ID placeholder default */
    int l512_rounds = 0;               /* Giai doan 7.4: --l512 N */
    const char *ctl = NULL;            /* soft_bus control FIFO for --l512 */
    int ca = 0;                        /* GD9.3: --ca, L4-08 (soft_bus --coe-ca) */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--expected-vendor") == 0 && i + 1 < argc) {
            expected_vendor_id = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--l512") == 0 && i + 1 < argc) {
            l512_rounds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            ctl = argv[++i];
        } else if (strcmp(argv[i], "--ca") == 0) {
            ca = 1;
        } else {
            fprintf(stderr, "Unrecognized argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!ifname) {
        fprintf(stderr, "Usage: %s --iface <veth_m> [--n <slaves>] "
                        "[--expected-vendor 0xHEX] [--l512 ROUNDS [--ctl SOFT_BUS_FIFO]] [--ca]\n", argv[0]);
        return 2;
    }

    if (!ecx_init(&ctx, ifname)) {
        fprintf(stderr, "ecx_init failed on %s (root/setcap needed?)\n", ifname);
        return 1;
    }
    if (ecx_config_init(&ctx) <= 0) {
        fprintf(stderr, "No slaves found\n");
        ecx_close(&ctx);
        return 1;
    }
    if (ctx.slavecount < n) {
        fprintf(stderr, "Found %d slave(s), expected at least %d\n", ctx.slavecount, n);
    }

    /* Single group (0) for every slave -- see file header. */
    ecx_config_map_group(&ctx, IOmap, 0);
    g_stats.expected_wkc = 3L * ctx.slavecount; /* see ecm_run.c for the same formula's derivation */

    if (!request_all_state(EC_STATE_PRE_OP, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach PREOP\n");
        ecx_close(&ctx);
        return 1;
    }

    g_mbx = ecm_mailbox_create(&ctx);
    if (!g_mbx) {
        fprintf(stderr, "ecm_mailbox_create failed\n");
        ecx_close(&ctx);
        return 1;
    }
    int mbx_enabled = ecm_mailbox_enable_cyclic(&ctx);
    printf("l4_test: cyclic mailbox enabled on %d/%d slave(s)\n", mbx_enabled, ctx.slavecount);

    /* Defensive, no-op here: group 0's logstartaddr is always 0, so
     * this subtracts 0 -- kept only so anyone copying this file into a
     * multi-group tool doesn't lose the fix. See ecm_run.c's own
     * comment for the full derivation (SOEM ec_config.c bug, confirmed
     * by gdb + reading the source directly). */
    ctx.grouplist[0].mbxstatuslength -= ctx.grouplist[0].logstartaddr;

    pthread_t mailbox_tid;
    pthread_t cyclic_tid;
    pthread_create(&mailbox_tid, NULL, mailbox_thread_fn, NULL);

    /* Start the cyclic pump thread HERE, before the PREOP tests, not
     * after reaching OP -- ecm_mailbox_rt_pump_group() (called inside
     * cyclic_thread_fn) is the ONLY thing that actually drives
     * ecx_mbxinhandler()/ecx_mbxouthandler(); with nothing pumping it
     * yet, an SDO request just sits in the queue until its own timeout
     * expires. Confirmed the hard way: L4-01/L4-02 both came back
     * rc=-4 the first time this tool ran, purely because this thread
     * used to start only right before the OP tests. Sending process
     * data this early is harmless for this test tool (soft_bus's FMMU
     * logic doesn't gate on ESM state), even though a real production
     * master (ecm_run.c) only starts its RT loop from OP onward. */
    pthread_create(&cyclic_tid, NULL, cyclic_thread_fn, NULL);
    usleep(20000); /* let a few cycles run before the first SDO probe */

    printf("\n=== PREOP tests ===\n");
    test_l4_01(1, expected_vendor_id);
    test_l4_02(1);
    if (ca) test_l4_08_preop(1);

    if (!request_all_state(EC_STATE_SAFE_OP, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach SAFEOP\n");
        goto stop_cyclic;
    }
    /* one real cycle before requesting OP, same reasoning as ecm_run.c */
    ecx_send_processdata_group(&ctx, 0);
    ecx_receive_processdata_group(&ctx, 0, EC_TIMEOUTRET);

    if (!request_all_state(EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE)) {
        fprintf(stderr, "Failed to reach OPERATIONAL\n");
        goto stop_cyclic;
    }
    usleep(50000); /* let a few OP cycles run before the OP-phase tests */

    printf("\n=== OP tests ===\n");
    test_l4_03(1);
    test_l4_04(1);
    test_l4_05(1);
    test_l4_07(1);
    if (ca) test_l4_08_op(1);
    if (l512_rounds > 0) {
        printf("\n=== L5-12 (mailbox repeat / duplicate), %d rounds%s ===\n", l512_rounds,
               ctl ? "" : " -- no --ctl: nothing injected");
#ifdef ECMASTER_SOEM_MBXCNT_PATCH
        printf("SOEM mailbox Cnt patch: present\n");
#else
        printf("SOEM mailbox Cnt patch: ABSENT (stock SOEM: duplicated responses are processed twice)\n");
#endif
        test_l5_12(1, l512_rounds, ctl);
#ifdef ECMASTER_SOEM_MBXCNT_PATCH
        printf("SOEM dropped %d duplicated mailbox response(s) from slave 1\n", ctx.slavelist[1].mbxindup);
#endif
    }

stop_cyclic:
    g_cyclic_stop = 1;
    pthread_join(cyclic_tid, NULL);

    g_mailbox_stop = 1;
    pthread_join(mailbox_tid, NULL);
    ecm_mailbox_destroy(g_mbx);

    request_all_state(EC_STATE_INIT, EC_TIMEOUTSTATE);
    ecx_close(&ctx);

    printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}