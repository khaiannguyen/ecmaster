/* ==========================================================================
 * test_fault.c — offline tests for Phase 7: fault injection (esc_fault.c),
 * process data watchdog, error counter semantics, mailbox repeat protocol.
 *
 * Same idea as test_offline.c: synthetic frames, explicit timestamps, no
 * network. The chain is configured the way SOEM configures it (station
 * addresses, SM2/SM3 at the SII addresses, one write FMMU + one read FMMU
 * per node, one LRW for the whole chain), then driven through the same
 * frame_begin / process_frame / frame_end / pop_due sequence soft_bus_main
 * uses. Where it makes sense a test carries a negative control: the same
 * scenario with the mechanism off must NOT show the effect.
 *
 * Build & run:  make test_fault && ./test_fault
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esc_types.h"
#include "esc_sii.h"
#include "esc_core.h"
#include "esc_dc.h"
#include "esc_fault.h"

static int g_pass = 0, g_fail = 0;

static void check(const char *name, long got, long want)
{
    if (got == want) { printf("  [PASS] %-60s = %ld\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-60s = %ld (expected %ld)\n", name, got, want); g_fail++; }
}
static void check_hex(const char *name, unsigned long got, unsigned long want)
{
    if (got == want) { printf("  [PASS] %-60s = 0x%04lX\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-60s = 0x%04lX (expected 0x%04lX)\n", name, got, want); g_fail++; }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

#define MS(x) ((uint64_t)((x) * 1000000.0))
#define LOG_BASE 0x00010000u

static uint8_t g_idx = 0;   /* datagram index, distinct per frame */

static size_t build_frame(uint8_t *buf, uint8_t cmd, uint16_t adp, uint16_t ado,
                          const uint8_t *data, uint16_t dlen)
{
    memset(buf, 0, FAULT_FRAME_MAX);
    memset(buf, 0xFF, 6);
    buf[12] = 0x88; buf[13] = 0xA4;
    uint16_t ec_len = (uint16_t)(DG_HDR_LEN + dlen + DG_WKC_LEN);
    wr16(buf + ETH_HDR_LEN, (uint16_t)(ec_len | (1u << 12)));
    uint8_t *dg = buf + ETH_HDR_LEN + EC_HDR_LEN;
    dg[0] = cmd;
    dg[1] = g_idx++;
    wr16(dg + 2, adp);
    wr16(dg + 4, ado);
    wr16(dg + 6, dlen);
    if (data) memcpy(dg + DG_HDR_LEN, data, dlen);
    return ETH_HDR_LEN + EC_HDR_LEN + ec_len;
}
static uint8_t *dg_data(uint8_t *buf) { return buf + ETH_HDR_LEN + EC_HDR_LEN + DG_HDR_LEN; }
static uint16_t dg_wkc(const uint8_t *buf) {
    uint16_t dlen = rd16(buf + ETH_HDR_LEN + EC_HDR_LEN + 6) & 0x7FF;
    return rd16(buf + ETH_HDR_LEN + EC_HDR_LEN + DG_HDR_LEN + dlen);
}
static uint8_t dg_idx(const uint8_t *buf) { return buf[ETH_HDR_LEN + EC_HDR_LEN + 1]; }

/* One frame through the whole soft_bus pipeline. Replies land in rx[]. */
typedef struct {
    esc_t *c; int n; esc_fault_bus_t *f;
    uint8_t rx[8][FAULT_FRAME_MAX]; int nrx;
} rig_t;

static void rig_pop(rig_t *r, uint64_t now)
{
    const uint8_t *p; uint16_t l;
    r->nrx = 0;
    while (esc_fault_pop_due(r->f, now, &p, &l) && r->nrx < 8)
        memcpy(r->rx[r->nrx++], p, l);
}

static int rig_send(rig_t *r, uint8_t *buf, size_t len, uint64_t now)
{
    int np = esc_fault_frame_begin(r->f, r->c, r->n, buf, len, now);
    if (np > 0) process_frame(r->c, np, buf, len);
    esc_fault_frame_end(r->f, r->c, r->n, buf, len, now);
    rig_pop(r, now);
    return r->nrx;
}

/* LRW over the whole chain: N*4 output bytes then N*4 input bytes.
 * Returns WKC of the (first) reply, -1 if no reply is due now. */
static int rig_cycle(rig_t *r, uint8_t out_val, uint64_t now)
{
    uint8_t pd[256] = { 0 }, buf[FAULT_FRAME_MAX];
    memset(pd, out_val, (size_t)r->n * 4);
    size_t len = build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, (uint16_t)(LOG_BASE >> 16),
                             pd, (uint16_t)(r->n * 8));
    rig_send(r, buf, len, now);
    return r->nrx ? dg_wkc(r->rx[0]) : -1;
}

static int rig_rw(rig_t *r, uint8_t cmd, uint16_t adp, uint16_t ado, uint8_t *data,
                  uint16_t dlen, uint64_t now)
{
    uint8_t buf[FAULT_FRAME_MAX];
    size_t len = build_frame(buf, cmd, adp, ado, data, dlen);
    rig_send(r, buf, len, now);
    if (!r->nrx) return -1;
    if (data) memcpy(data, dg_data(r->rx[0]), dlen);
    return dg_wkc(r->rx[0]);
}

static void set_al(rig_t *r, int i, uint16_t req, uint64_t now)
{
    uint8_t d[2]; wr16(d, req);
    rig_rw(r, CMD_FPWR, (uint16_t)(0x1001 + i), REG_AL_CONTROL, d, 2, now);
}
static uint16_t al(const rig_t *r, int i)      { return rd16(r->c[i].regs + REG_AL_STATUS); }
static uint16_t alcode(const rig_t *r, int i)  { return rd16(r->c[i].regs + REG_AL_STATUS_CODE); }

/* Build a chain configured like SOEM leaves it in OP. wd_ticks = 0x0420. */
static rig_t *rig_new(int n, uint16_t wd_ticks, int react, int app_seq)
{
    static rig_t rs[4];
    static int k = 0;
    rig_t *r = &rs[k++ % 4];
    memset(r, 0, sizeof(*r));
    r->n = n;
    r->c = calloc((size_t)n, sizeof(esc_t));
    r->f = calloc(1, sizeof(esc_fault_bus_t));
    esc_fault_init(r->f, NULL, app_seq);          /* silent in tests */
    for (int i = 0; i < n; i++) esc_init(&r->c[i], (uint8_t)i, 4);
    esc_chain_wire(r->c, n);
    for (int i = 0; i < n; i++) r->c[i].wd.react = (uint8_t)react;

    uint64_t t = 1;
    for (int i = 0; i < n; i++) {                 /* station addresses via APWR */
        uint8_t d[2]; wr16(d, (uint16_t)(0x1001 + i));
        rig_rw(r, CMD_APWR, (uint16_t)(0 - i), REG_STATION_ADDR, d, 2, t);
    }
    for (int i = 0; i < n; i++) {
        uint8_t *g = r->c[i].regs;
        /* SM2 outputs 0x1100 / SM3 inputs 0x1108, values from the SII */
        uint8_t sm2[8] = { 0x00, 0x11, 4, 0, SII_SM2_CONTROL, 0, 1, 0 };
        uint8_t sm3[8] = { 0x08, 0x11, 4, 0, SII_SM3_CONTROL, 0, 1, 0 };
        rig_rw(r, CMD_FPWR, (uint16_t)(0x1001 + i), REG_SM_BASE + 2 * 8, sm2, 8, t);
        rig_rw(r, CMD_FPWR, (uint16_t)(0x1001 + i), REG_SM_BASE + 3 * 8, sm3, 8, t);
        uint8_t *f0 = g + REG_FMMU_BASE, *f1 = f0 + REG_FMMU_ENTRY_SIZE;
        wr32(f0 + FMMU_OFF_LOG_START, LOG_BASE + (uint32_t)i * 4);
        wr16(f0 + FMMU_OFF_LENGTH, 4); f0[FMMU_OFF_LOG_STOP_BIT] = 7;
        wr16(f0 + FMMU_OFF_PHYS_START, 0x1100); f0[FMMU_OFF_TYPE] = 2; f0[FMMU_OFF_ACTIVATE] = 1;
        wr32(f1 + FMMU_OFF_LOG_START, LOG_BASE + (uint32_t)(n * 4 + i * 4));
        wr16(f1 + FMMU_OFF_LENGTH, 4); f1[FMMU_OFF_LOG_STOP_BIT] = 7;
        wr16(f1 + FMMU_OFF_PHYS_START, 0x1108); f1[FMMU_OFF_TYPE] = 1; f1[FMMU_OFF_ACTIVATE] = 1;
        uint8_t wd[2]; wr16(wd, wd_ticks);
        rig_rw(r, CMD_FPWR, (uint16_t)(0x1001 + i), REG_WD_TIME_PROCDATA, wd, 2, t);
    }
    for (int i = 0; i < n; i++) set_al(r, i, ESM_PREOP, t);
    for (int i = 0; i < n; i++) set_al(r, i, ESM_SAFEOP, t);
    rig_cycle(r, 0x11, t);
    for (int i = 0; i < n; i++) set_al(r, i, ESM_OP, t);
    return r;
}
static void rig_free(rig_t *r) { free(r->c); free(r->f); }

/* ====================================================================== */
static void t_watchdog(void)
{
    printf("\n[F1] Process data watchdog: 0x0400/0x0420, OP -> SAFEOP+ERR 0x001B (L5-13)\n");
    rig_t *r = rig_new(2, 30, 1, -1);             /* 30 x 100 us = 3 ms */
    check_hex("node0 in OP after setup", al(r, 0), ESM_OP);
    check_hex("0x0400 = reset value 0x09C2 (100 us tick)", rd16(r->c[0].regs + REG_WD_DIVIDER), 0x09C2);

    /* Setup ran at t = 1 ns, so the first cycle must come within 3 ms. */
    uint64_t t = MS(1);
    for (int k = 0; k < 10; k++, t += MS(1)) rig_cycle(r, 0x22, t);
    check_hex("1 ms cycles: still OP", al(r, 0), ESM_OP);
    check("0x0440 bit0 = 1 (watchdog running)", r->c[0].regs[REG_WD_STATUS_PD] & 1, 1);

    /* Negative control: a 2.9 ms gap (< 3 ms) must not expire. */
    uint8_t d[2] = { 0 };
    t += MS(1.9);
    rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t);
    check_hex("gap 2.9 ms (< 3 ms): still OP", al(r, 0), ESM_OP);
    rig_cycle(r, 0x22, t);

    /* Master stops sending PD: the next frame of any kind arrives 3.5 ms
     * after the last SM2 write. */
    t += MS(3.5);
    memset(d, 0, sizeof(d));                      /* BRD ORs into the data */
    int wkc = rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t);
    check_hex("gap 3.5 ms: node0 AL status = SAFEOP+ERR", al(r, 0), ESM_SAFEOP | 0x10);
    check_hex("gap 3.5 ms: node0 AL status code = 0x001B", alcode(r, 0), ALSTATUSCODE_SYNCMANWATCHDOG);
    check_hex("master sees it in the same frame (BRD 0x0130 OR = 0x14)", d[0], 0x14);
    check("BRD WKC still 2", wkc, 2);
    check("0x0440 bit0 = 0 (expired)", r->c[0].regs[REG_WD_STATUS_PD] & 1, 0);
    check("0x0442 watchdog counter = 1", r->c[0].regs[REG_WD_COUNTER_PD], 1);

    t += MS(10);
    rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t);
    check("expiry counted once per trigger gap (0x0442 still 1)", r->c[0].regs[REG_WD_COUNTER_PD], 1);

    /* Recovery the SOEM way: ack the error, send PD, request OP. */
    set_al(r, 0, ESM_SAFEOP | 0x10, t);
    check_hex("after error ack: SAFEOP, error bit cleared", al(r, 0), ESM_SAFEOP);
    rig_cycle(r, 0x33, t);
    set_al(r, 0, ESM_OP, t);
    check_hex("after PD + OP request: back in OP", al(r, 0), ESM_OP);

    uint8_t z = 0;
    rig_rw(r, CMD_FPWR, 0x1001, REG_WD_COUNTER_PD, &z, 1, t);
    check("write 0x0442 clears the watchdog counters", r->c[0].regs[REG_WD_COUNTER_PD], 0);
    uint8_t one = 1;
    rig_rw(r, CMD_FPWR, 0x1001, REG_WD_STATUS_PD, &one, 1, t);
    check("0x0440 is read-only for ECAT (write ignored)", r->c[0].regs[REG_WD_STATUS_PD] & 1, 1);
    rig_free(r);

    printf("  -- negative controls --\n");
    r = rig_new(1, 30, 0, -1);                    /* --no-sm-wd */
    t = MS(1); rig_cycle(r, 0x22, t);
    t += MS(5); rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t);
    check_hex("--no-sm-wd: stays OP after 5 ms gap", al(r, 0), ESM_OP);
    check("--no-sm-wd: the ESC still counts the expiry (0x0442)", r->c[0].regs[REG_WD_COUNTER_PD], 1);
    rig_free(r);

    r = rig_new(1, 0, 1, -1);                     /* 0x0420 = 0: disabled */
    t = MS(1); rig_cycle(r, 0x22, t);
    t += MS(500); rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t);
    check_hex("0x0420 = 0 disables the watchdog: OP after 500 ms", al(r, 0), ESM_OP);
    rig_free(r);

    r = rig_new(1, 30, 1, -1);                    /* partial write: no trigger */
    t = MS(1); rig_cycle(r, 0x22, t);
    uint8_t two[2] = { 0x55, 0x55 };
    for (int k = 0; k < 5; k++) { t += MS(1); rig_rw(r, CMD_FPWR, 0x1001, 0x1100, two, 2, t); }
    check_hex("writes not reaching the SM2 last byte don't trigger -> expired",
              al(r, 0), ESM_SAFEOP | 0x10);
    rig_free(r);
}

static void t_counters(void)
{
    printf("\n[F2] bad_cable + error counter semantics (L5-06)\n");
    rig_t *r = rig_new(4, 0, 1, -1);
    uint64_t t = MS(10);
    check("baseline LRW WKC (4 nodes x 3)", rig_cycle(r, 0x10, t), 12);

    esc_fault_command(r->f, r->c, r->n, "bad_cable 2 3", t);
    int noreply = 0;
    for (int k = 0; k < 3; k++) { t += MS(1); if (rig_cycle(r, (uint8_t)(0x20 + k), t) < 0) noreply++; }
    check("3 frames with bad cable: 3 lost at the master", noreply, 3);
    check("next frame gets through again", rig_cycle(r, 0x30, t += MS(1)), 12);

    const uint8_t *g0 = r->c[0].regs, *g1 = r->c[1].regs, *g2 = r->c[2].regs, *g3 = r->c[3].regs;
    check("node2 0x0300 invalid frame port0 = 3", g2[0x0300], 3);
    check("node2 0x0301 RX error port0 = 3", g2[0x0301], 3);
    check("node2 0x030C processing unit = 3", g2[0x030C], 3);
    check("node3 0x0308 forwarded port0 = 3", g3[0x0308], 3);
    check("node3 0x0300 = 0 (it is NOT the first to see it)", g3[0x0300], 0);
    check("node0/1 0x0300 = 0 (upstream of the fault)", g0[0x0300] + g1[0x0300], 0);
    check("node0,1,2 0x0309 forwarded port1 (return path) = 3 each", g0[0x0309] + g1[0x0309] + g2[0x0309], 9);
    check("node3 0x0309 = 0 (loop closed on the last port1)", g3[0x0309], 0);

    /* Only nodes upstream of the fault committed the writes of those frames.
     * (0x30 cycle afterwards wrote everyone, so look at the counters' frames
     * via a fresh scenario on SM2 content below.) */
    esc_fault_command(r->f, r->c, r->n, "bad_cable 2 1", t);
    rig_cycle(r, 0x77, t += MS(1));
    check_hex("bad frame: node1 (upstream) committed its outputs", r->c[1].regs[0x1100], 0x77);
    check_hex("bad frame: node2 (at the fault) did not", r->c[2].regs[0x1100], 0x30);

    uint8_t z = 0;
    rig_rw(r, CMD_FPWR, 0x1003, 0x0300, &z, 1, t += MS(1));
    check("write 0x0300 clears 0x0300..0x030B (node2 0x0300)", g2[0x0300] + g2[0x0301] + g2[0x0309], 0);
    check("... but not 0x030C", g2[0x030C], 4);
    esc_fault_command(r->f, r->c, r->n, "bad_cable 1 300", t);
    for (int k = 0; k < 300; k++) rig_cycle(r, 0x40, t += MS(1));
    check("counters saturate at 0xFF (node1 0x0300 after 300)", r->c[1].regs[0x0300], 0xFF);
    rig_free(r);
}

static void t_mute_wkc_short(void)
{
    printf("\n[F3] mute (L5-02) and wkc_short (L5-01)\n");
    rig_t *r = rig_new(4, 0, 1, -1);
    uint64_t t = MS(10);
    esc_fault_command(r->f, r->c, r->n, "mute 3", t);
    int noreply = 0;
    for (int k = 0; k < 3; k++) if (rig_cycle(r, 0x50, t += MS(1)) < 0) noreply++;
    check("mute 3: three frames without reply", noreply, 3);
    check_hex("muted frames never reached an ESC (outputs untouched)", r->c[0].regs[0x1100], 0x11);
    check("4th frame answered normally", rig_cycle(r, 0x51, t += MS(1)), 12);
    check("muted counter", (long)r->f->muted, 3);

    esc_fault_command(r->f, r->c, r->n, "wkc_short 1 2", t);
    uint8_t d[2] = { 0 };
    check("a non-PD frame does not consume wkc_short (BRD WKC 4)",
          rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t += MS(1)), 4);
    check("wkc_short 1 2: PD frame 1 WKC = 12 - 3", rig_cycle(r, 0x52, t += MS(1)), 9);
    check("wkc_short 1 2: PD frame 2 WKC = 9", rig_cycle(r, 0x53, t += MS(1)), 9);
    check("then WKC back to 12", rig_cycle(r, 0x54, t += MS(1)), 12);
    rig_free(r);
}

static void t_safeop(void)
{
    printf("\n[F4] safeop <node> <code> (L5-05)\n");
    rig_t *r = rig_new(3, 0, 1, -1);
    uint64_t t = MS(10);
    check("bad node index rejected", esc_fault_command(r->f, r->c, r->n, "safeop 3 0x1A", t), -1);
    check("hex code accepted", esc_fault_command(r->f, r->c, r->n, "safeop 1 0x001A", t), 0);
    check_hex("node1 AL status SAFEOP+ERR", al(r, 1), ESM_SAFEOP | 0x10);
    check_hex("node1 AL status code 0x001A", alcode(r, 1), 0x001A);
    check_hex("node0 untouched", al(r, 0), ESM_OP);
    set_al(r, 1, ESM_OP, t);
    check_hex("OP request without ack is refused (still error)", al(r, 1) & 0x10, 0x10);
    set_al(r, 1, ESM_SAFEOP | 0x10, t);
    rig_cycle(r, 0x60, t += MS(1));
    set_al(r, 1, ESM_OP, t);
    check_hex("ack + PD + OP: node1 back in OP", al(r, 1), ESM_OP);
    rig_free(r);
}

static void t_timing(void)
{
    printf("\n[F5] late / dup / reorder (L5-07, L5-08)\n");
    rig_t *r = rig_new(2, 0, 1, -1);
    uint64_t t = MS(10);
    uint8_t buf[FAULT_FRAME_MAX], pd[16] = { 0 };

    esc_fault_command(r->f, r->c, r->n, "late 1500 1", t);
    size_t la = build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, 1, pd, 16);
    uint8_t ia = dg_idx(buf);
    rig_send(r, buf, la, t);
    check("late: no reply at t", r->nrx, 0);
    check("late: next due = t + 1.5 ms", (long)(esc_fault_next_due(r->f) - t), 1500000);
    size_t lb = build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, 1, pd, 16);
    uint8_t ib = dg_idx(buf);
    rig_send(r, buf, lb, t + MS(0.5));
    check("frame B (not late) answered at t+0.5 ms, overtaking A", r->nrx == 1 && dg_idx(r->rx[0]) == ib, 1);
    rig_pop(r, t + MS(1.4));
    check("nothing at t+1.4 ms", r->nrx, 0);
    rig_pop(r, t + MS(1.5));
    check("A delivered at t+1.5 ms", r->nrx == 1 && dg_idx(r->rx[0]) == ia, 1);

    t += MS(10);
    esc_fault_command(r->f, r->c, r->n, "dup 1", t);
    size_t lc = build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, 1, pd, 16);
    rig_send(r, buf, lc, t);
    check("dup: two replies", r->nrx, 2);
    check("dup: byte-identical", memcmp(r->rx[0], r->rx[1], lc) == 0, 1);

    t += MS(10);
    esc_fault_command(r->f, r->c, r->n, "reorder 1", t);
    build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, 1, pd, 16);
    ia = dg_idx(buf);
    rig_send(r, buf, la, t);
    check("reorder: first reply held", r->nrx, 0);
    build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, 1, pd, 16);
    ib = dg_idx(buf);
    rig_send(r, buf, lb, t + MS(0.3));
    check("reorder: two replies after the second frame", r->nrx, 2);
    check("reorder: order is B then A", r->nrx == 2 && dg_idx(r->rx[0]) == ib && dg_idx(r->rx[1]) == ia, 1);

    t += MS(10);
    esc_fault_command(r->f, r->c, r->n, "reorder 1", t);
    rig_send(r, buf, lb, t);
    rig_pop(r, t + MS(49));
    check("reorder alone: still held at +49 ms", r->nrx, 0);
    rig_pop(r, t + MS(50));
    check("reorder alone: sent at +50 ms", r->nrx, 1);
    check("reorder_alone counter", (long)r->f->reorder_alone, 1);

    t += MS(100);
    esc_fault_command(r->f, r->c, r->n, "late 20000 40", t);
    for (int k = 0; k < 40; k++) rig_cycle(r, 0x70, t + MS(k));
    check("late 20 ms x 40 frames at 1 ms: 20 in flight, none lost", (long)r->f->q_overflow, 0);
    rig_pop(r, t + MS(60));
    check("... remaining replies delivered", r->nrx > 0, 1);
    rig_free(r);
}

static void t_stale(void)
{
    printf("\n[F6] stale <node> <n> with --app-seq (L5-09)\n");
    rig_t *r = rig_new(2, 0, 1, 0);               /* sequence counter at TxPDO byte 0 */
    uint64_t t = MS(10);
    uint8_t pd[16];
    uint16_t s0[7], s1[7];
    int wkc_ok = 1;
    for (int k = 0; k < 7; k++) {
        if (k == 2) esc_fault_command(r->f, r->c, r->n, "stale 1 3", t);
        memset(pd, 0, sizeof(pd));
        uint8_t buf[FAULT_FRAME_MAX];
        size_t l = build_frame(buf, CMD_LRW, (uint16_t)LOG_BASE, 1, pd, 16);
        rig_send(r, buf, l, t += MS(1));
        if (dg_wkc(r->rx[0]) != 6) wkc_ok = 0;
        s0[k] = rd16(dg_data(r->rx[0]) + 8);      /* node0 inputs */
        s1[k] = rd16(dg_data(r->rx[0]) + 12);     /* node1 inputs */
    }
    /* The value read in frame k is what the slave wrote after frame k-1:
     * stale frames 2,3,4 skip the update, so frames 3,4,5 read the value
     * from frame 2, and frame 6 reads a fresh one. */
    check("node0 counter advances by 1 every PD frame", s0[6] - s0[0], 6);
    check("node1 counter advances before stale", s1[2] - s1[1], 1);
    check("node1 counter frozen for 3 frames (read in frames 3..5)", s1[5] - s1[2], 0);
    check("node1 counter moves again afterwards", s1[6] - s1[5], 1);
    check("WKC correct (6) in every frame, stale or not", wkc_ok, 1);
    rig_free(r);

    r = rig_new(1, 0, 1, -1);                     /* negative control: feature off */
    rig_cycle(r, 0, MS(10)); rig_cycle(r, 0, MS(11));
    check("without --app-seq the TxPDO never changes", rd16(r->c[0].regs + 0x1108), 0);
    rig_free(r);
}

static void t_drop_restore(void)
{
    printf("\n[F7] drop_node / restore_node (L5-11)\n");
    rig_t *r = rig_new(4, 30, 1, -1);
    uint64_t t = MS(1);          /* within 3 ms of setup */
    uint8_t d[2] = { 0 };
    check("BRD before: WKC 4", rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t), 4);

    esc_fault_command(r->f, r->c, r->n, "drop_node 2", t);
    check("BRD after drop_node 2: WKC 2 (bus ends at node1)",
          rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t += MS(1)), 2);
    uint16_t dl1 = rd16(r->c[1].regs + REG_DL_STATUS);
    check("node1 DL: port1 link down", (dl1 & DLSTAT_LINK_PORT1) != 0, 0);
    check("node1 DL: port1 loop closed", (dl1 & DLSTAT_LOOP_PORT1) != 0, 1);
    check("node1 lost link counter port1 (0x0311) = 1", r->c[1].regs[0x0311], 1);
    check("node3 lost link counter port0 (0x0310) = 1", r->c[3].regs[0x0310], 1);
    check("LRW WKC = 2 nodes x 3", rig_cycle(r, 0x80, t += MS(1)), 6);

    for (int k = 0; k < 9; k++) rig_cycle(r, 0x81, t += MS(1));
    esc_fault_command(r->f, r->c, r->n, "restore_node 2", t);
    check_hex("restored node2: AL INIT", al(r, 2), ESM_INIT);
    check_hex("restored node2: station address 0", rd16(r->c[2].regs + REG_STATION_ADDR), 0);
    check("BRD after restore: WKC 4", rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t += MS(1)), 4);
    check("node1 DL: port1 link up again", (rd16(r->c[1].regs + REG_DL_STATUS) & DLSTAT_LINK_PORT1) != 0, 1);
    check_hex("node3 (isolated 10 ms, 3 ms watchdog): SAFEOP+ERR", al(r, 3), ESM_SAFEOP | 0x10);
    check_hex("node3 AL status code 0x001B", alcode(r, 3), ALSTATUSCODE_SYNCMANWATCHDOG);
    check_hex("node3 kept its station address 0x1004", rd16(r->c[3].regs + REG_STATION_ADDR), 0x1004);
    check_hex("node0 (kept receiving PD) still OP", al(r, 0), ESM_OP);

    esc_fault_command(r->f, r->c, r->n, "drop_node 0", t);
    check("drop_node 0: no reply at all", rig_rw(r, CMD_BRD, 0, REG_AL_STATUS, d, 2, t += MS(1)), -1);
    rig_free(r);
}

/* Put a fake mailbox response into node0's SM1 and mark it full, the way
 * esc_coe.c does after answering an SDO request. */
static void post_response(rig_t *r, uint8_t tag)
{
    uint8_t *g = r->c[0].regs;
    memset(g + SII_SM1_OFFSET, tag, SII_SM1_SIZE);
    g[REG_SM1_STATUS] |= SM_STATUS_MAILBOX_FULL;
}

static void t_mailbox(void)
{
    printf("\n[F8] mailbox repeat protocol + mbx_repeat / mbx_dup (L5-12)\n");
    rig_t *r = rig_new(1, 0, 1, -1);
    uint64_t t = MS(10);
    uint8_t mb[SII_SM1_SIZE], st[2];

    post_response(r, 0xA1);
    esc_fault_command(r->f, r->c, r->n, "mbx_repeat 0", t);
    int w = rig_rw(r, CMD_FPRD, 0x1001, SII_SM1_OFFSET, mb, SII_SM1_SIZE, t += MS(1));
    check("mbx_repeat: reply of the SM1 read is lost", w, -1);
    check("... but the slave considers it read (full flag cleared)",
          r->c[0].regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL, 0);

    /* SOEM ec_main.c: read 0x080D:0x080E, toggle bit 0x0200, write back. */
    rig_rw(r, CMD_FPRD, 0x1001, REG_SM1_STATUS, st, 2, t += MS(1));
    uint16_t v = (uint16_t)(rd16(st) ^ 0x0200);
    wr16(st, v);
    rig_rw(r, CMD_FPWR, 0x1001, REG_SM1_STATUS, st, 2, t += MS(1));
    check("repeat request: response re-posted (full flag set)",
          r->c[0].regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL, SM_STATUS_MAILBOX_FULL);
    check("repeat ack: 0x080F bit1 mirrors 0x080E bit1",
          (r->c[0].regs[0x080F] & 2) == (r->c[0].regs[0x080E] & 2), 1);
    w = rig_rw(r, CMD_FPRD, 0x1001, SII_SM1_OFFSET, mb, SII_SM1_SIZE, t += MS(1));
    check("master reads the same response again", w == 1 && mb[0] == 0xA1, 1);

    uint8_t zero[2] = { 0, r->c[0].regs[0x080E] };
    post_response(r, 0xA2);
    rig_rw(r, CMD_FPWR, 0x1001, REG_SM1_STATUS, zero, 2, t += MS(1));
    check("SM1 status is read-only for ECAT (full flag survives a write of 0)",
          r->c[0].regs[REG_SM1_STATUS] & SM_STATUS_MAILBOX_FULL, SM_STATUS_MAILBOX_FULL);
    /* Keep activate bit1 as it is: toggling it IS a repeat request. */
    uint8_t sm1[8] = { 0x80, 0x10, 0x80, 0, SII_SM1_CONTROL, 0, r->c[0].regs[0x080E], 0 };
    uint8_t ack_before = r->c[0].regs[0x080F] & 2;
    rig_rw(r, CMD_FPWR, 0x1001, REG_SM_BASE + 8, sm1, 8, t += MS(1));
    check("SM1 PDI control (repeat ack) survives a full 8-byte SM write",
          r->c[0].regs[0x080F] & 2, ack_before);
    rig_rw(r, CMD_FPRD, 0x1001, SII_SM1_OFFSET, mb, SII_SM1_SIZE, t += MS(1));

    esc_fault_command(r->f, r->c, r->n, "mbx_dup", t);
    post_response(r, 0xB7);
    rig_rw(r, CMD_FPRD, 0x1001, SII_SM1_OFFSET, mb, SII_SM1_SIZE, t += MS(1));
    check("mbx_dup: first read gets the response", mb[0], 0xB7);
    rig_rw(r, CMD_FPRD, 0x1001, REG_SM1_STATUS, st, 1, t += MS(1));
    check("mbx_dup: next frame sees mailbox full again", st[0] & SM_STATUS_MAILBOX_FULL, SM_STATUS_MAILBOX_FULL);
    rig_rw(r, CMD_FPRD, 0x1001, SII_SM1_OFFSET, mb, SII_SM1_SIZE, t += MS(1));
    check("mbx_dup: same bytes (same Cnt) delivered twice", mb[0], 0xB7);
    rig_rw(r, CMD_FPRD, 0x1001, REG_SM1_STATUS, st, 1, t += MS(1));
    check("mbx_dup: only once", st[0] & SM_STATUS_MAILBOX_FULL, 0);
    rig_free(r);
}

static void t_same_state(void)
{
    printf("\n[F10] AL Control request for the current state is not an error\n");
    rig_t *r = rig_new(1, 0, 1, -1);
    esc_fault_command(r->f, r->c, r->n, "drop_node 0", 0);
    esc_fault_command(r->f, r->c, r->n, "restore_node 0", 0);
    uint8_t d[2]; wr16(d, 0x1001);
    rig_rw(r, CMD_APWR, 0, REG_STATION_ADDR, d, 2, MS(1));
    set_al(r, 0, ESM_INIT, MS(1));
    check_hex("INIT -> request INIT: stays INIT, no error bit", al(r, 0), ESM_INIT);
    check_hex("... AL status code 0", alcode(r, 0), 0);
    esc_fault_command(r->f, r->c, r->n, "safeop 0 0x1A", MS(1));   /* INIT + ERR */
    set_al(r, 0, ESM_INIT, MS(1));
    check_hex("error bit kept until acknowledged", al(r, 0), ESM_INIT | 0x10);
    set_al(r, 0, ESM_INIT | 0x10, MS(1));
    check_hex("ack clears it", al(r, 0), ESM_INIT);
    rig_free(r);
}

static void t_parser(void)
{
    printf("\n[F9] command parser\n");
    rig_t *r = rig_new(2, 0, 1, -1);
    check("unknown command -> -1", esc_fault_command(r->f, r->c, r->n, "explode 3", 0), -1);
    check("missing argument -> -1", esc_fault_command(r->f, r->c, r->n, "mute", 0), -1);
    check("garbage number -> -1", esc_fault_command(r->f, r->c, r->n, "mute 1x", 0), -1);
    check("comment / blank line -> 0", esc_fault_command(r->f, r->c, r->n, "  # just a note", 0), 0);
    check("reject_al all -> 0", esc_fault_command(r->f, r->c, r->n, "reject_al all", 0), 0);
    check("reject_al sets force_reject_al on every node", r->c[0].force_reject_al + r->c[1].force_reject_al, 2);
    esc_fault_command(r->f, r->c, r->n, "mute 5", 0);
    esc_fault_command(r->f, r->c, r->n, "wkc_short 1 5", 0);
    check("clear -> 0", esc_fault_command(r->f, r->c, r->n, "clear", 0), 0);
    check("clear cancels mute, wkc_short, reject_al",
          (long)(r->f->mute_left + r->c[1].fault.wkc_short_left + r->c[0].force_reject_al), 0);
    rig_free(r);
}

int main(void)
{
    printf("=========================================================\n");
    printf(" test_fault — Phase 7 fault injection, NO network required\n");
    printf("=========================================================\n");
    t_watchdog();
    t_counters();
    t_mute_wkc_short();
    t_safeop();
    t_timing();
    t_stale();
    t_drop_restore();
    t_mailbox();
    t_same_state();
    t_parser();
    printf("\n=========================================================\n");
    printf(" RESULT: %d pass, %d fail\n", g_pass, g_fail);
    printf("=========================================================\n");
    return g_fail == 0 ? 0 : 1;
}
