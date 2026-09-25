/* ==========================================================================
 * test_policy_offline.c — unit tests for ecm_policy.c (Phase 7.3), no network.
 * Each group maps to a line of docs/fault_policy.md.
 *
 * Build & run:  make test
 * ========================================================================== */
#include <stdio.h>
#include <string.h>

#include "ecm_policy.h"

static int g_pass = 0, g_fail = 0;
static void check(const char *name, long got, long want)
{
    if (got == want) { printf("  [PASS] %-64s = %ld\n", name, got); g_pass++; }
    else { printf("  [FAIL] %-64s = %ld (expected %ld)\n", name, got, want); g_fail++; }
}

#define OK      ECM_POL_WKC_OK
#define NOFRAME ECM_POL_WKC_NOFRAME
#define PARTIAL ECM_POL_WKC_PARTIAL
#define NOIO    (-1)
#define MS(x)   ((uint64_t)(x) * 1000000ull)
#define S(x)    ((uint64_t)(x) * 1000000000ull)

static ecm_bus_fsm_t F;
static ecm_event_t   E;

static void fsm_new(void)
{
    ecm_bus_cfg_t c;
    ecm_bus_default_cfg(&c);
    ecm_bus_init(&F, &c);
}

/* P1: rx timeout from the deadline (§2) */
static void p1_timeout(void)
{
    printf("P1 receive timeout from tick deadline\n");
    uint64_t wake = S(10), deadline = wake + 1000000 - 150000;   /* 1 ms cycle, 150 us guard */
    check("P1a just woke: 850 us left", ecm_rx_timeout_us(wake, deadline, 50, 1000), 850);
    check("P1b 700 us into the tick: 150 us", ecm_rx_timeout_us(wake + 700000, deadline, 50, 1000), 150);
    check("P1c past the deadline: RX_MIN", ecm_rx_timeout_us(wake + 900000, deadline, 50, 1000), 50);
    check("P1d far past (late wake): RX_MIN, no underflow", ecm_rx_timeout_us(deadline + S(1), deadline, 50, 1000), 50);
    check("P1e clamp to RX_MAX", ecm_rx_timeout_us(0, S(1), 50, 1000), 1000);
}

/* P2: RUN <-> DEGRADED (§3) */
static void p2_degraded(void)
{
    printf("P2 RUN <-> DEGRADED\n");
    fsm_new();
    check("P2a starts in RUN", F.state, ECM_BUS_RUN);
    check("P2b OK cycle: no transition", ecm_bus_on_cycle(&F, 1, OK, OK, &E), 0);
    check("P2c motion PARTIAL -> transition", ecm_bus_on_cycle(&F, 2, PARTIAL, NOIO, &E), 1);
    check("P2d   now DEGRADED", F.state, ECM_BUS_DEGRADED);
    check("P2e   event from RUN", E.from, ECM_BUS_RUN);
    check("P2f   event reason PARTIAL", E.a, PARTIAL);
    check("P2g   event group motion", E.b, 0);
    check("P2h one good cycle -> RUN", ecm_bus_on_cycle(&F, 3, OK, NOIO, &E), 1);
    check("P2i   now RUN", F.state, ECM_BUS_RUN);

    /* IO failure holds DEGRADED until the next IO cycle is good, even
     * though motion cycles in between are fine */
    ecm_bus_on_cycle(&F, 8, OK, PARTIAL, &E);
    check("P2j IO PARTIAL -> DEGRADED", F.state, ECM_BUS_DEGRADED);
    check("P2k   reason from IO", E.a, PARTIAL);
    check("P2l   group io", E.b, 1);
    check("P2m motion OK, no IO this tick: still DEGRADED", ecm_bus_on_cycle(&F, 9, OK, NOIO, &E), 0);
    check("P2n next IO OK -> RUN", ecm_bus_on_cycle(&F, 16, OK, OK, &E), 1);
    check("P2o   entered DEGRADED twice", (long)F.entered[ECM_BUS_DEGRADED], 2);
}

/* P3: DEGRADED -> LOST exactly at n_lost consecutive NOFRAME (L5-02) */
static void p3_lost(void)
{
    printf("P3 LOST threshold (L5-02)\n");
    fsm_new();
    uint64_t t = 1;
    for (int i = 0; i < 99; i++) ecm_bus_on_cycle(&F, t++, NOFRAME, NOIO, &E);
    check("P3a 99 NOFRAME: DEGRADED, not LOST", F.state, ECM_BUS_DEGRADED);
    ecm_bus_on_cycle(&F, t, OK, NOIO, &E);
    check("P3b one good frame resets the run -> RUN", F.state, ECM_BUS_RUN);
    for (int i = 0; i < 99; i++) ecm_bus_on_cycle(&F, ++t, NOFRAME, NOIO, &E);
    check("P3c 99 again: still DEGRADED", F.state, ECM_BUS_DEGRADED);
    check("P3d 100th NOFRAME -> transition", ecm_bus_on_cycle(&F, ++t, NOFRAME, NOIO, &E), 1);
    check("P3e   now LOST", F.state, ECM_BUS_LOST);
    check("P3f   from DEGRADED", E.from, ECM_BUS_DEGRADED);
    check("P3g   cycles no longer accounted in LOST", ecm_bus_on_cycle(&F, ++t, OK, OK, &E), 0);
    check("P3h   still LOST", F.state, ECM_BUS_LOST);
    /* PARTIAL forever never makes LOST (bus still answers) */
    fsm_new();
    for (int i = 0; i < 1000; i++) ecm_bus_on_cycle(&F, (uint64_t)i + 1, PARTIAL, NOIO, &E);
    check("P3i 1000 PARTIAL: DEGRADED, not LOST", F.state, ECM_BUS_DEGRADED);
}

/* P4: LOST probing, RECOVER, done/failed (L5-03/04) */
static void p4_recover(void)
{
    printf("P4 LOST -> RECOVER -> RUN / LOST\n");
    fsm_new();
    uint64_t t = 1;
    for (int i = 0; i < 100; i++) ecm_bus_on_cycle(&F, t++, NOFRAME, NOIO, &E);
    uint64_t lost_at = t - 1;
    check("P4a LOST", F.state, ECM_BUS_LOST);
    check("P4b probe due on the next tick", ecm_bus_probe_due(&F, lost_at + 1), 1);
    check("P4c nobody answers: no transition", ecm_bus_on_probe(&F, lost_at + 1, 0, &E), 0);
    check("P4d next probe not before +10", ecm_bus_probe_due(&F, lost_at + 10), 0);
    check("P4e due at +11", ecm_bus_probe_due(&F, lost_at + 11), 1);
    check("P4f BRD answers 8 -> RECOVER", ecm_bus_on_probe(&F, lost_at + 11, 8, &E), 1);
    check("P4g   state", F.state, ECM_BUS_RECOVER);
    check("P4h   event carries BRD WKC", E.a, 8);
    check("P4i no probes while RECOVER", ecm_bus_probe_due(&F, lost_at + 100), 0);
    check("P4j recover failed -> LOST", ecm_bus_on_recover_done(&F, 2000, 0, &E), 1);
    check("P4k   state", F.state, ECM_BUS_LOST);
    check("P4l backoff: no probe at +999", ecm_bus_probe_due(&F, 2999), 0);
    check("P4m probe at +1000", ecm_bus_probe_due(&F, 3000), 1);
    ecm_bus_on_probe(&F, 3000, 8, &E);
    check("P4n recover ok -> RUN", ecm_bus_on_recover_done(&F, 3100, 1, &E), 1);
    check("P4o   state", F.state, ECM_BUS_RUN);
    check("P4p   LOST entered twice", (long)F.entered[ECM_BUS_LOST], 2);
    check("P4q after RUN a single NOFRAME is DEGRADED, not LOST", (ecm_bus_on_cycle(&F, 3101, NOFRAME, NOIO, &E), F.state), ECM_BUS_DEGRADED);
    check("P4r recover_done outside RECOVER ignored", ecm_bus_on_recover_done(&F, 3102, 1, &E), 0);
}

/* P5: event ring and command queue */
static void p5_rings(void)
{
    printf("P5 SPSC rings\n");
    static ecm_evring_t r;
    ecm_evring_init(&r);
    ecm_event_t e = { .type = ECM_EV_BUS }, o;
    int pushed = 0;
    for (int i = 0; i < ECM_EVRING_LEN + 10; i++) { e.tick = (uint64_t)i; pushed += ecm_evring_push(&r, &e); }
    check("P5a ring holds LEN events", pushed, ECM_EVRING_LEN);
    check("P5b the rest are counted as drops", (long)r.drops, 10);
    check("P5c pop is FIFO", (ecm_evring_pop(&r, &o), (long)o.tick), 0);
    int n = 1;
    while (ecm_evring_pop(&r, &o)) n++;
    check("P5d pop all", n, ECM_EVRING_LEN);
    check("P5e last is LEN-1", (long)o.tick, ECM_EVRING_LEN - 1);
    check("P5f empty", ecm_evring_pop(&r, &o), 0);

    static ecm_cmdq_t q;
    ecm_cmdq_init(&q);
    ecm_cmd_t c = { .slave = 3, .configadr = 0x1003, .reg = 0x0120, .value = 0x14 }, g;
    check("P5g cmd push", ecm_cmdq_push(&q, &c), 1);
    check("P5h peek sees it", ecm_cmdq_peek(&q, &g), 1);
    check("P5i peek does not remove", (ecm_cmdq_peek(&q, &g), g.value), 0x14);
    ecm_cmdq_drop_head(&q);
    check("P5j drop_head removes", ecm_cmdq_peek(&q, &g), 0);
    ecm_cmdq_drop_head(&q);
    check("P5k drop_head on empty is harmless", ecm_cmdq_peek(&q, &g), 0);
    for (int i = 0; i < ECM_CMDQ_LEN + 1; i++) ecm_cmdq_push(&q, &c);
    check("P5l full queue drops", (long)q.drops, 1);
}

/* P6: per-slave planner (§4) */
static ecm_srec_plan_t P;
static ecm_sact_t dec(int i, int reach, int ans, uint16_t al, uint64_t now, int *rec, int *gu)
{
    ecm_srec_input_t in = { .in_reach = reach, .answered = ans, .al_status = al };
    return ecm_srec_decide(&P, i, &in, now, rec, gu);
}

static void p6_planner(void)
{
    printf("P6 per-slave recovery planner\n");
    int rec, gu;
    ecm_srec_init(&P, 8, 5, S(1));
    check("P6a healthy OP: nothing", dec(0, 1, 1, 0x08, S(1), &rec, &gu), ECM_SACT_NONE);
    check("P6b   not a recovery", rec, 0);

    /* path A: SAFEOP+ERR 0x001B / 0x001A (L5-05, L5-13) */
    check("P6c SAFEOP+ERR -> ACK", dec(2, 1, 1, 0x14, S(10), &rec, &gu), ECM_SACT_ACK);
    check("P6d   attempt 1", P.s[2].attempts, 1);
    check("P6e SAFEOP after ack -> OP (not gated by backoff)", dec(2, 1, 1, 0x04, S(10) + MS(100), &rec, &gu), ECM_SACT_OP);
    check("P6f OP again -> healthy, recovered flag", (dec(2, 1, 1, 0x08, S(11), &rec, &gu), rec), 1);
    check("P6g   attempts reset", P.s[2].attempts, 0);
    check("P6h   recoveries", (long)P.s[2].recoveries, 1);

    /* path B: no answer on configured address (power cycled, address 0) */
    check("P6i in reach but no answer -> RECONFIG", dec(4, 1, 0, 0x00, S(20), &rec, &gu), ECM_SACT_RECONFIG);
    check("P6j INIT -> RECONFIG, but backed off 1 s", dec(4, 1, 1, 0x01, S(20) + MS(500), &rec, &gu), ECM_SACT_NONE);
    check("P6k after 1 s -> RECONFIG (attempt 2)", dec(4, 1, 1, 0x01, S(21), &rec, &gu), ECM_SACT_RECONFIG);
    check("P6l   next wait is 2 s: none at +1.5 s", dec(4, 1, 1, 0x01, S(22) + MS(500), &rec, &gu), ECM_SACT_NONE);
    check("P6m   ok at +2 s", dec(4, 1, 1, 0x01, S(23), &rec, &gu), ECM_SACT_RECONFIG);
    check("P6n PREOP -> RECONFIG path too", (P.s[4].next_try_ns = 0, dec(4, 1, 1, 0x02, S(40), &rec, &gu)), ECM_SACT_RECONFIG);

    /* behind a broken chain: not its fault, nothing counted */
    uint32_t a0 = P.s[6].attempts;
    check("P6o out of reach -> NONE", dec(6, 0, 0, 0x00, S(30), &rec, &gu), ECM_SACT_NONE);
    check("P6p   no attempt counted", P.s[6].attempts, a0);

    /* bounded: 5 attempts, then FAILED (flag once), then silence */
    ecm_srec_init(&P, 8, 5, S(1));
    uint64_t t = S(100);
    int acks = 0, gave = 0;
    for (int k = 0; k < 40; k++, t += S(20)) {
        ecm_sact_t a = dec(1, 1, 1, 0x14, t, &rec, &gu);
        if (a == ECM_SACT_ACK) acks++;
        gave += gu;
    }
    check("P6q stuck slave: exactly 5 ACK attempts", acks, 5);
    check("P6r   gave_up reported once", gave, 1);
    check("P6s   failed", P.s[1].failed, 1);
    check("P6t   failed slave: no more actions", dec(1, 1, 1, 0x14, t + S(100), &rec, &gu), ECM_SACT_NONE);
    check("P6u recovers by itself -> failed cleared", (dec(1, 1, 1, 0x08, t + S(200), &rec, &gu), P.s[1].failed), 0);
    ecm_srec_reset(&P, 1);
    check("P6v reset is safe on a healthy slave", P.s[1].attempts, 0);
    check("P6w index out of range -> NONE", dec(99, 1, 0, 0, S(1), &rec, &gu), ECM_SACT_NONE);
}

/* P7: input freshness (L5-09 and the L5-07/08 oracle) */
static void p7_fresh(void)
{
    printf("P7 input freshness\n");
    ecm_fresh_t f;
    ecm_fresh_init(&f, 10, 16);
    check("P7a first sample", ecm_fresh_update(&f, 100), ECM_FRESH_FIRST);
    check("P7b +1 ok", ecm_fresh_update(&f, 101), ECM_FRESH_OK);
    check("P7c +2 (an IO frame in between) ok", ecm_fresh_update(&f, 103), ECM_FRESH_OK);
    int ev = 0, starts = 0;
    for (int i = 0; i < 9; i++) ev = ecm_fresh_update(&f, 103);
    check("P7d 9 unchanged: not yet stale", ev, ECM_FRESH_UNCHANGED);
    ev = ecm_fresh_update(&f, 103);
    check("P7e 10th unchanged: STALE_START", ev, ECM_FRESH_STALE_START);
    for (int i = 0; i < 20; i++) if (ecm_fresh_update(&f, 103) == ECM_FRESH_STALE_START) starts++;
    check("P7f reported once per episode", starts, 0);
    check("P7g stale cycles counted", (long)f.stale_cycles, 21);
    check("P7h changes again: RESUMED", ecm_fresh_update(&f, 104), ECM_FRESH_RESUMED);
    check("P7i one episode", (long)f.stale_episodes, 1);
    check("P7j 15 older (reused index, old reply): REGRESSION", ecm_fresh_update(&f, 89), ECM_FRESH_REGRESSION);
    check("P7k next genuine value is ok (last kept)", ecm_fresh_update(&f, 105), ECM_FRESH_OK);
    check("P7m regressions counted", (long)f.regressions, 1);
    ecm_fresh_t w;
    ecm_fresh_init(&w, 10, 16);
    ecm_fresh_update(&w, 0xFFFE);
    ecm_fresh_update(&w, 0xFFFF);
    check("P7l wrap 0xFFFF -> 0x0000 is +1, ok", ecm_fresh_update(&w, 0x0000), ECM_FRESH_OK);
    ecm_fresh_t g;
    ecm_fresh_init(&g, 3, 8);
    ecm_fresh_update(&g, 250);
    check("P7n 8-bit wrap 250 -> 4 is forward", ecm_fresh_update(&g, 4), ECM_FRESH_OK);
    check("P7o 8-bit 4 -> 200 is backward", ecm_fresh_update(&g, 200), ECM_FRESH_REGRESSION);
}

int main(void)
{
    p1_timeout();
    p2_degraded();
    p3_lost();
    p4_recover();
    p5_rings();
    p6_planner();
    p7_fresh();
    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
