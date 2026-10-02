/*
 * test_xchg_offline.c -- Phase 10.4 app <-> RT exchange, offline, one thread.
 *
 *   X-01o  commands: FIFO, at most ECM_XCHG_CMD_PER_TICK per tick (the rest
 *          deferred, counted), a command for a future tick waits at the head
 *          and blocks the ones behind it, SET only on outputs, owner ops
 *          counted as unknown here, last seq echoed in the clock record
 *   X-02o  setpoints: the hook of tick k writes the entry for k+1; late ones
 *          dropped and counted, at most ECM_XCHG_SP_SKIP_MAX per tick; none
 *          -> underrun once armed, the last value held (Q-04 offline); not
 *          armed -> no underrun, outputs untouched
 *   X-03o  bus lost: setpoints for its ticks dropped (dropped_lost), not
 *          replayed afterwards; input slot keeps its last good value and
 *          VALID = 0 while the frame is bad
 *   X-04o  ring: capacity, full drops, order across wrap-around; seqlock
 *          round trip
 */
#include "ecm_xchg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
static void check(const char *name, long got, long want)
{
    int ok = got == want;
    printf("  [%s] %-70s = %ld%s\n", ok ? "PASS" : "FAIL", name, got, ok ? "" : " (unexpected)");
    if (ok) g_pass++; else g_fail++;
}

static ecm_xchg_t X;
static uint8_t io[64];
static ecm_pdo_handle_t h_out32 = { .slave = 1, .group = 1, .dir = ECM_PDO_OUT, .bit = 16, .bits = 32 };
static ecm_pdo_handle_t h_out16 = { .slave = 1, .group = 1, .dir = ECM_PDO_OUT, .bit = 48, .bits = 16 };
static ecm_pdo_handle_t h_in32  = { .slave = 1, .group = 1, .dir = ECM_PDO_IN,  .bit = 256, .bits = 32 };

static uint64_t w[ECM_XST_WORDS], tk;
static uint64_t sw(int slot, int word) { ecm_xchg_read_slot(&X, slot, &tk, w); return w[word]; }
static uint64_t cw(int word) { ecm_xchg_read_clock(&X, &tk, w); return w[word]; }

static void fresh(void)
{
    ecm_xchg_init(&X, 1000000);
    memset(io, 0, sizeof(io));
    check("slot 0 = 32-bit output", ecm_xchg_add_slot(&X, &h_out32), 0);
    check("slot 1 = 16-bit output", ecm_xchg_add_slot(&X, &h_out16), 1);
    check("slot 2 = 32-bit input", ecm_xchg_add_slot(&X, &h_in32), 2);
}

static void x01(void)
{
    printf("[X-01o commands]\n");
    fresh();
    for (int i = 0; i < 12; i++) {
        ecm_xcmd_t c = { .tick = 0, .target = 1, .op = ECM_XOP_SET, .seq = (uint32_t)(100 + i), .arg0 = 1000 + i };
        ecm_xchg_cmd(&X, &c);
    }
    ecm_xchg_rt(&X, io, 10, 0, 1, 0);
    check("12 queued, 8 applied in one tick", (long)cw(ECM_XC_CMD_APPLIED), 8);
    check("  the rest deferred (counted once)", (long)cw(ECM_XC_CMD_DEFERRED), 1);
    check("  FIFO: last applied seq 107", (long)cw(ECM_XC_LAST_SEQ), 107);
    check("  slot 1 output = 1007 (the 8th SET)", (long)ecm_pdo_get(&h_out16, io), 1007);
    ecm_xchg_rt(&X, io, 11, 0, 1, 0);
    check("next tick: the remaining 4 (seq 111, value 1011)", (long)cw(ECM_XC_LAST_SEQ) * 10000 + (long)ecm_pdo_get(&h_out16, io), 1111011);

    ecm_xcmd_t fut = { .tick = 20, .target = 0, .op = ECM_XOP_SET, .seq = 200, .arg0 = 77 };
    ecm_xcmd_t now = { .tick = 0, .target = 0, .op = ECM_XOP_SET, .seq = 201, .arg0 = 88 };
    ecm_xchg_cmd(&X, &fut);
    ecm_xchg_cmd(&X, &now);
    ecm_xchg_rt(&X, io, 12, 0, 1, 0);
    check("future command (tick 20) waits at the head ...", (long)cw(ECM_XC_LAST_SEQ), 111);
    check("  ... and the one behind it waits too (FIFO, not reordered)", (long)ecm_pdo_get(&h_out32, io), 0);
    ecm_xchg_rt(&X, io, 18, 0, 1, 0);
    check("tick 18: still waiting (applies in the hook whose k+1 = 20)", (long)ecm_pdo_get(&h_out32, io), 0);
    ecm_xchg_rt(&X, io, 19, 0, 1, 0);
    check("tick 19 (k+1 = 20): both applied in order, value 88", (long)cw(ECM_XC_LAST_SEQ) * 1000 + (long)ecm_pdo_get(&h_out32, io), 201088);

    ecm_xcmd_t bad = { .target = 2, .op = ECM_XOP_SET, .seq = 300, .arg0 = 5 };
    ecm_xcmd_t own = { .target = 0, .op = ECM_XOP_USER + 3, .seq = 301 };
    ecm_xchg_cmd(&X, &bad);
    ecm_xchg_cmd(&X, &own);
    ecm_xchg_rt(&X, io, 20, 0, 1, 0);
    check("SET on an input slot ignored (input bits untouched)", (long)ecm_pdo_get(&h_in32, io), 0);
    check("owner op counted as unknown by the generic hook", (long)cw(ECM_XC_CMD_UNKNOWN), 1);
}

static void x02(void)
{
    printf("[X-02o setpoints]\n");
    fresh();
    ecm_xchg_rt(&X, io, 1, 0, 1, 0);
    check("not armed: no underrun", (long)sw(0, ECM_XS_UNDERRUN), 0);
    check("not armed: output untouched, VALID 0", (long)ecm_pdo_get(&h_out32, io) * 10 + (long)sw(0, ECM_XS_VALID), 0);
    for (uint64_t t = 3; t <= 12; t++) ecm_xchg_setpoint(&X, 0, t, (int64_t)(t * 100));
    ecm_xchg_rt(&X, io, 2, 0, 1, 0);
    check("hook of tick 2 writes the setpoint of tick 3 (300)", (long)ecm_pdo_get(&h_out32, io), 300);
    check("  used 1, VALID 1", (long)sw(0, ECM_XS_USED) * 10 + (long)sw(0, ECM_XS_VALID), 11);
    ecm_xchg_rt(&X, io, 7, 0, 1, 0);                /* ticks 3..6 skipped: entries 4..7 late */
    check("jump to tick 7: setpoint of 8 used (800)", (long)ecm_pdo_get(&h_out32, io), 800);
    check("  4 late ones dropped (4, 5, 6, 7)", (long)sw(0, ECM_XS_LATE), 4);
    ecm_xchg_rt(&X, io, 8, 0, 1, 0);
    ecm_xchg_rt(&X, io, 9, 0, 1, 0);
    ecm_xchg_rt(&X, io, 10, 0, 1, 0);
    ecm_xchg_rt(&X, io, 11, 0, 1, 0);               /* uses 12, the last one */
    check("through tick 12: 1200", (long)ecm_pdo_get(&h_out32, io), 1200);
    ecm_pdo_set(&h_out32, io, 0xDEAD);              /* something else wrote it */
    for (uint64_t t = 12; t < 112; t++) ecm_xchg_rt(&X, io, t, 0, 1, 0);   /* 100 ms without setpoints */
    check("Q-04o 100 ticks without setpoints: 100 underruns", (long)sw(0, ECM_XS_UNDERRUN), 100);
    check("  the last value (1200) held, rewritten every tick", (long)ecm_pdo_get(&h_out32, io), 1200);
    for (uint64_t t = 50; t < 90; t++) ecm_xchg_setpoint(&X, 0, t, 1);     /* all late */
    ecm_xchg_setpoint(&X, 0, 113, 4242);
    ecm_xchg_rt(&X, io, 112, 0, 1, 0);
    check("40 late in the ring: only 16 dropped this tick (RT budget)", (long)sw(0, ECM_XS_LATE), 4 + 16);
    ecm_xchg_rt(&X, io, 112, 0, 1, 0);
    ecm_xchg_rt(&X, io, 112, 0, 1, 0);
    check("  ... the rest in the next ticks, then 4242 for tick 113", (long)sw(0, ECM_XS_LATE) * 10000 + (long)ecm_pdo_get(&h_out32, io), 440000 + 4242);
    check("slot 1 never armed: no underrun all along", (long)sw(1, ECM_XS_UNDERRUN), 0);
}

static void x03(void)
{
    printf("[X-03o bus lost]\n");
    fresh();
    ecm_pdo_set(&h_in32, io, 0x11223344);
    ecm_xchg_rt(&X, io, 1, 0, 1, 0);
    check("input slot: value read, VALID 1", (long)sw(2, ECM_XS_VALUE) * 10 + (long)sw(2, ECM_XS_VALID), 0x11223344L * 10 + 1);
    ecm_pdo_set(&h_in32, io, 0x55555555);           /* stale bytes of a bad frame */
    ecm_xchg_rt(&X, io, 2, 0, 0, 0);
    check("bad frame: last good value kept, VALID 0", (long)sw(2, ECM_XS_VALUE) * 10 + (long)sw(2, ECM_XS_VALID), 0x11223344L * 10);
    for (uint64_t t = 3; t <= 20; t++) ecm_xchg_setpoint(&X, 0, t, (int64_t)t);
    ecm_xchg_rt(&X, io, 2, 0, 1, 0);               /* uses 3 */
    for (uint64_t t = 3; t < 10; t++) ecm_xchg_rt(&X, io, t, 0, 0, 1);    /* lost: k+1 = 4..10 */
    check("lost for 7 ticks: 7 setpoints dropped_lost", (long)sw(0, ECM_XS_DROPPED_LOST), 7);
    check("  no underrun counted while lost, output not written", (long)sw(0, ECM_XS_UNDERRUN) * 1000 + (long)ecm_pdo_get(&h_out32, io), 3);
    check("  clock record says bus lost", (long)cw(ECM_XC_BUS_LOST), 1);
    ecm_xchg_rt(&X, io, 10, 0, 1, 0);
    check("bus back at tick 10: setpoint of 11, nothing replayed", (long)ecm_pdo_get(&h_out32, io), 11);
}

static void x04(void)
{
    printf("[X-04o ring and seqlock]\n");
    static ecm_xring_t r;
    ecm_xring_init(&r, 8);
    uint64_t e[4] = { 0 }, o[4];
    int pushed = 0;
    for (int i = 0; i < 10; i++) { e[0] = (uint64_t)i; pushed += ecm_xring_push(&r, e) == 0; }
    check("capacity 8: 8 pushed, 2 full drops", pushed * 10 + (long)r.full_drops, 82);
    long order = 1;
    for (int k = 0; k < 1000; k++) {               /* many wrap-arounds */
        if (ecm_xring_peek(&r, o) || o[0] != (uint64_t)(k)) { order = 0; break; }
        ecm_xring_pop(&r);
        e[0] = (uint64_t)(k + 8);
        ecm_xring_push(&r, e);
    }
    check("FIFO order kept across 1000 wrap-arounds", order, 1);
    static ecm_xst_t s;
    uint64_t in[ECM_XST_WORDS] = { 1, 2, 3, 4, 5, 6, 7, 8 }, out[ECM_XST_WORDS], t;
    ecm_xst_write(&s, 42, in, ECM_XST_WORDS);
    check("seqlock round trip", ecm_xst_read(&s, &t, out, ECM_XST_WORDS, 1) == 0 && t == 42 && !memcmp(in, out, sizeof(in)), 1);
    atomic_store(&s.seq, 7);                       /* writer "in progress" forever */
    check("odd seq (writer inside): read gives up after its tries", ecm_xst_read(&s, &t, out, ECM_XST_WORDS, 5), -1);
}

int main(void)
{
    x01();
    x02();
    x03();
    x04();
    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
