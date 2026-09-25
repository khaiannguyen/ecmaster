/* ==========================================================================
 * test_spsc_stress.c — Giai doan 7.6: every lock-free RT <-> non-RT queue of
 * the project under ThreadSanitizer, one producer thread + one consumer
 * thread each, with a check of what comes out:
 *
 *   ring_spsc_t       (telemetry, GD4)  RT -> telemetry, rt_sample_t
 *   tx_order_ring_t   (telemetry, GD4)  RT -> telemetry, send order
 *   ecm_evring_t      (policy, 7.3)     RT -> monitor, events
 *   ecm_cmdq_t        (policy, 7.3)     monitor -> RT, recovery commands
 *   ecm_diag_handoff_t(diag, 7.2)       RT -> monitor, 2-slot "latest wins"
 *
 * Queues: in order, nothing lost (the producer retries when full), and no
 * torn element (every field is derived from the sequence number).
 * Handoff: allowed to drop, but what comes out is increasing and whole.
 *
 * TSan reports any data race; this program additionally exits 1 on a
 * content error, so it also catches a broken ordering that TSan's model
 * happened not to flag.
 *
 *   ./test_spsc_stress [items_per_queue]   (default 2 000 000)
 * ========================================================================== */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "libecmaster/telemetry/ring_spsc.h"
#include "libecmaster/telemetry/turnaround.h"
#include "libecmaster/policy/ecm_policy.h"
#include "libecmaster/diag/ecm_diag.h"

static uint64_t g_n = 2000000;
static atomic_int g_errors;

#define FAILF(...) do { if (atomic_fetch_add(&g_errors, 1) < 10) { fprintf(stderr, __VA_ARGS__); } } while (0)

/* ---------------- ring_spsc_t ---------------- */
static ring_spsc_t g_ring;
static void *ring_prod(void *a)
{
    (void)a;
    for (uint64_t i = 0; i < g_n; i++) {
        rt_sample_t s = { .wake_jitter_ns = (int64_t)i, .prep_send_ns = (int64_t)(i * 3),
                          .cycle_occupancy_ns = (int64_t)(i ^ 0x5555), .rx_ts_ns = ~i, .tick = i,
                          .sent_io_group = (uint8_t)(i & 1), .any_wkc_mismatch = (uint8_t)(i >> 1 & 1) };
        while (ring_push(&g_ring, &s) != 0) { /* full: retry, nothing may be lost */ }
    }
    return NULL;
}
static void *ring_cons(void *a)
{
    (void)a;
    rt_sample_t s;
    for (uint64_t i = 0; i < g_n; ) {
        if (ring_pop(&g_ring, &s) != 0) continue;
        if (s.tick != i || s.wake_jitter_ns != (int64_t)i || s.prep_send_ns != (int64_t)(i * 3)
            || s.cycle_occupancy_ns != (int64_t)(i ^ 0x5555) || s.rx_ts_ns != ~i
            || s.sent_io_group != (uint8_t)(i & 1) || s.any_wkc_mismatch != (uint8_t)(i >> 1 & 1))
            FAILF("ring_spsc: element %llu wrong (tick %llu)\n", (unsigned long long)i, (unsigned long long)s.tick);
        i++;
    }
    return NULL;
}

/* ---------------- tx_order_ring_t ---------------- */
static tx_order_ring_t g_txo;
static void *txo_prod(void *a)
{
    (void)a;
    for (uint64_t i = 0; i < g_n; i++)
        while (tx_order_ring_push_idx(&g_txo, i, (ecm_group_id_t)(1 + (i & 1)), (uint8_t)(i % 16)) != 0) { }
    return NULL;
}
static void *txo_cons(void *a)
{
    (void)a;
    tx_order_sample_t s;
    for (uint64_t i = 0; i < g_n; ) {
        if (tx_order_ring_pop(&g_txo, &s) != 0) continue;
        if (s.tick != i || s.group_id != 1 + (i & 1) || s.ec_idx != i % 16)
            FAILF("tx_order_ring: element %llu wrong\n", (unsigned long long)i);
        i++;
    }
    return NULL;
}

/* ---------------- ecm_evring_t ---------------- */
static ecm_evring_t g_ev;
static void *ev_prod(void *a)
{
    (void)a;
    for (uint64_t i = 0; i < g_n; i++) {
        ecm_event_t e = { .tick = i, .type = (uint16_t)(i & 0xFFFF), .from = (uint8_t)i,
                          .to = (uint8_t)(i >> 8), .a = (int32_t)i, .b = -(int32_t)i };
        while (!ecm_evring_push(&g_ev, &e)) { }
    }
    return NULL;
}
static void *ev_cons(void *a)
{
    (void)a;
    ecm_event_t e;
    for (uint64_t i = 0; i < g_n; ) {
        if (!ecm_evring_pop(&g_ev, &e)) continue;
        if (e.tick != i || e.type != (uint16_t)(i & 0xFFFF) || e.from != (uint8_t)i
            || e.to != (uint8_t)(i >> 8) || e.a != (int32_t)i || e.b != -(int32_t)i)
            FAILF("ecm_evring: element %llu wrong\n", (unsigned long long)i);
        i++;
    }
    return NULL;
}

/* ---------------- ecm_cmdq_t (peek + drop_head, as the RT thread uses it) ---------------- */
static ecm_cmdq_t g_cq;
static void *cq_prod(void *a)
{
    (void)a;
    for (uint64_t i = 0; i < g_n; i++) {
        ecm_cmd_t c = { .slave = (uint16_t)i, .configadr = (uint16_t)(i * 7),
                        .reg = (uint16_t)(i ^ 0x0120), .value = (uint16_t)(i >> 16) };
        while (!ecm_cmdq_push(&g_cq, &c)) { }
    }
    return NULL;
}
static void *cq_cons(void *a)
{
    (void)a;
    ecm_cmd_t c;
    for (uint64_t i = 0; i < g_n; ) {
        if (!ecm_cmdq_peek(&g_cq, &c)) continue;
        if (c.slave != (uint16_t)i || c.configadr != (uint16_t)(i * 7)
            || c.reg != (uint16_t)(i ^ 0x0120) || c.value != (uint16_t)(i >> 16))
            FAILF("ecm_cmdq: element %llu wrong\n", (unsigned long long)i);
        ecm_cmdq_drop_head(&g_cq);
        i++;
    }
    return NULL;
}

/* ---------------- ecm_diag_handoff_t (2 slots, latest wins) ---------------- */
static ecm_diag_handoff_t g_ho;
static atomic_int g_ho_done;
static uint64_t g_ho_got;
static uint64_t g_ho_last_ok;           /* last epoch the handoff accepted (written before g_ho_done) */
static void fill_raw(ecm_diag_raw_t *r, uint64_t seq)
{
    r->seq = seq;
    r->t_ns = seq * 1000;
    r->n = (int)(seq % 64) + 1;
    r->brd_count = (int)(seq % 97);
    for (int i = 0; i < ECM_DIAG_MAX_SLAVES; i++) {
        r->s[i].station_addr = (uint16_t)(seq + (uint64_t)i);
        r->s[i].al_code = (uint16_t)(seq >> 3);
    }
}
static int raw_whole(const ecm_diag_raw_t *r)
{
    if (r->t_ns != r->seq * 1000 || r->n != (int)(r->seq % 64) + 1 || r->brd_count != (int)(r->seq % 97)) return 0;
    for (int i = 0; i < ECM_DIAG_MAX_SLAVES; i++)
        if (r->s[i].station_addr != (uint16_t)(r->seq + (uint64_t)i) || r->s[i].al_code != (uint16_t)(r->seq >> 3)) return 0;
    return 1;
}
static void *ho_prod(void *a)
{
    (void)a;
    static ecm_diag_raw_t r;
    uint64_t n = g_n / 20 + 1;           /* 4.5 KB per put: fewer rounds */
    for (uint64_t i = 1; i <= n; i++) {
        fill_raw(&r, i);
        if (ecm_diag_handoff_put(&g_ho, &r)) g_ho_last_ok = i;   /* both slots full: this one dropped (by design) */
    }
    atomic_store(&g_ho_done, 1);
    return NULL;
}
static void *ho_cons(void *a)
{
    (void)a;
    static ecm_diag_raw_t r;
    uint64_t last = 0;
    for (;;) {
        int done = atomic_load(&g_ho_done);
        while (ecm_diag_handoff_get(&g_ho, &r)) {
            if (!raw_whole(&r)) FAILF("diag handoff: torn snapshot seq %llu\n", (unsigned long long)r.seq);
            if (r.seq <= last) FAILF("diag handoff: seq %llu after %llu\n", (unsigned long long)r.seq, (unsigned long long)last);
            last = r.seq;
            g_ho_got++;
        }
        if (done) break;
    }
    /* the newest epoch the handoff ACCEPTED must arrive (a dropped one cannot) */
    if (last != g_ho_last_ok) FAILF("diag handoff: last seq %llu, last accepted put %llu\n",
                                    (unsigned long long)last, (unsigned long long)g_ho_last_ok);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) g_n = strtoull(argv[1], NULL, 0);
    ring_init(&g_ring);
    tx_order_ring_init(&g_txo);
    ecm_evring_init(&g_ev);
    ecm_cmdq_init(&g_cq);
    ecm_diag_handoff_init(&g_ho);

    struct { const char *name; void *(*p)(void *); void *(*c)(void *); } q[] = {
        { "ring_spsc_t (rt_sample_t)",  ring_prod, ring_cons },
        { "tx_order_ring_t",            txo_prod,  txo_cons  },
        { "ecm_evring_t",               ev_prod,   ev_cons   },
        { "ecm_cmdq_t (peek/drop)",     cq_prod,   cq_cons   },
        { "ecm_diag_handoff_t",         ho_prod,   ho_cons   },
    };
    int nq = (int)(sizeof(q) / sizeof(q[0]));
    pthread_t t[10];
    for (int i = 0; i < nq; i++) {       /* all queues at once: more interleavings */
        pthread_create(&t[2 * i], NULL, q[i].c, NULL);
        pthread_create(&t[2 * i + 1], NULL, q[i].p, NULL);
    }
    for (int i = 0; i < 2 * nq; i++) pthread_join(t[i], NULL);

    for (int i = 0; i < nq; i++) printf("  %-28s done\n", q[i].name);
    printf("  items per queue: %llu (diag handoff: %llu puts, %llu taken, drops %llu)\n",
           (unsigned long long)g_n, (unsigned long long)(g_n / 20 + 1),
           (unsigned long long)g_ho_got, (unsigned long long)g_ho.drops);
    int e = atomic_load(&g_errors);
    printf("RESULT: %s (%d content error(s))\n", e ? "FAIL" : "PASS", e);
    return e ? 1 : 0;
}
