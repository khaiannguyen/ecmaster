/*
 * test_xchg_tsan.c -- Phase 10.4 Q-05: the exchange under ThreadSanitizer.
 *
 * Three threads, like ecm_run with an exchange:
 *   rt        calls ecm_xchg_rt() for ticks 1..N on a private IOmap (the
 *             only thread touching it), publishes the tick in a flag
 *   producer  feeds setpoints ahead of the published tick and SET commands
 *   reader    reads the clock and slot records; every record must be
 *             internally consistent: the slot's VALUE is what the producer
 *             computes for that tick (or held while it starves), and the
 *             counters never go backwards
 * plus a raw seqlock stress (writer vs reader, payload = f(tick)) that
 * counts torn copies: 0 required here, > 0 required for the ECM_XST_BROKEN
 * build (negative control, run_xchg_tsan.sh).
 *
 *   ./test_xchg_tsan [ticks]
 * Exit 0 = consistent; 1 = a content error; TSan exits 66 on a race.
 */
#include "ecm_xchg.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ecm_xchg_t X;
static uint8_t io[64];
static _Atomic uint64_t g_tick, g_done;
static uint64_t N = 200000;
static long g_err;

static ecm_pdo_handle_t h0 = { .slave = 1, .group = 1, .dir = ECM_PDO_OUT, .bit = 0, .bits = 32 };

static int64_t f(uint64_t t) { return (int64_t)((t * 2654435761u) & 0x7FFFFFFF); }

static void *rt(void *a)
{
    for (uint64_t t = 1; t <= N; t++) {
        ecm_xchg_rt(&X, io, t, t * 1000, 1, 0);
        atomic_store_explicit(&g_tick, t, memory_order_release);
        if ((t & 63) == 0) sched_yield();
    }
    atomic_store(&g_done, 1);
    return a;
}

static void *producer(void *a)
{
    uint64_t next = 2;
    while (!atomic_load(&g_done)) {
        uint64_t now = atomic_load_explicit(&g_tick, memory_order_acquire);
        while (next <= now + 32 && next <= N + 1) {
            if (ecm_xchg_setpoint(&X, 0, next, f(next))) break;
            next++;
        }
        if ((next & 1023) == 0) {
            ecm_xcmd_t c = { .op = ECM_XOP_NOP, .seq = (uint32_t)next };
            ecm_xchg_cmd(&X, &c);
        }
        sched_yield();
    }
    return a;
}

static void *reader(void *a)
{
    uint64_t w[ECM_XST_WORDS], t, last_used = 0, last_applied = 0, n = 0;
    while (!atomic_load(&g_done)) {
        if (ecm_xchg_read_slot(&X, 0, &t, w) == 0 && w[ECM_XS_VALID]) {
            /* the hook of tick t wrote the value for t+1 -- unless it held */
            int64_t v = (int64_t)(uint32_t)w[ECM_XS_VALUE];
            if (w[ECM_XS_USED] < last_used) g_err++;
            if (v != f(t + 1) && w[ECM_XS_UNDERRUN] == 0 && w[ECM_XS_LATE] == 0) g_err++;
            last_used = w[ECM_XS_USED];
            n++;
        }
        if (ecm_xchg_read_clock(&X, &t, w) == 0) {
            if (w[ECM_XC_CMD_APPLIED] < last_applied || w[ECM_XC_T_SEND_NS] != t * 1000) g_err++;
            last_applied = w[ECM_XC_CMD_APPLIED];
        }
    }
    printf("    reader: %llu slot records checked\n", (unsigned long long)n);
    return a;
}

/* ---- raw seqlock stress ------------------------------------------------- */
static ecm_xst_t S;
static _Atomic int s_stop;
static void *s_writer(void *a)
{
    uint64_t w[ECM_XST_WORDS];
    for (uint64_t t = 1; t <= N * 4; t++) {
        for (int i = 0; i < ECM_XST_WORDS; i++) w[i] = t * 31 + (uint64_t)i;
        ecm_xst_write(&S, t, w, ECM_XST_WORDS);
    }
    atomic_store(&s_stop, 1);
    return a;
}
static long s_torn, s_reads;
static void *s_reader(void *a)
{
    uint64_t w[ECM_XST_WORDS], t;
    while (!atomic_load(&s_stop)) {
        if (ecm_xst_read(&S, &t, w, ECM_XST_WORDS, ECM_XST_TRIES)) continue;
        s_reads++;
        for (int i = 0; i < ECM_XST_WORDS; i++)
            if (w[i] != t * 31 + (uint64_t)i) { s_torn++; break; }
    }
    return a;
}

int main(int argc, char **argv)
{
    if (argc > 1) N = strtoull(argv[1], NULL, 0);
    ecm_xchg_init(&X, 1000000);
    ecm_xchg_add_slot(&X, &h0);
    pthread_t a, b, c;
    pthread_create(&a, NULL, rt, NULL);
    pthread_create(&b, NULL, producer, NULL);
    pthread_create(&c, NULL, reader, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL); pthread_join(c, NULL);
    printf("    exchange: %llu ticks, used %llu late %llu underrun %llu, content errors %ld\n",
           (unsigned long long)N, (unsigned long long)X.slot[0].used, (unsigned long long)X.slot[0].late,
           (unsigned long long)X.slot[0].underrun, g_err);

    pthread_create(&a, NULL, s_writer, NULL);
    pthread_create(&b, NULL, s_reader, NULL);
    pthread_join(a, NULL); pthread_join(b, NULL);
    printf("    seqlock: %ld consistent reads, %ld torn\n", s_reads, s_torn);
    printf("RESULT: %s (content errors %ld, torn %ld)\n", g_err || s_torn ? "FAIL" : "PASS", g_err, s_torn);
    return g_err || s_torn ? 1 : 0;
}
