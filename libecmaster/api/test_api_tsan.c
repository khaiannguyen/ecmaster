/* test_api_tsan.c -- Phase 11.1: the two-slot handoff under ThreadSanitizer.
 *
 *   ./test_api_tsan [puts]     one producer, one consumer (the api thread's shape)
 *
 * The producer writes every word of a 8 KB payload with its sequence number
 * (the size of the DC histogram the RT thread hands over) and NEVER waits;
 * the consumer drains like the api thread and checks each payload is whole
 * and newer than the last one. Exit 0 = no torn payload, no reordering.
 * Built with -DECM_API_HO_BROKEN (state published relaxed), TSan must report
 * a data race (run_api_tsan.sh, negative control). */
#define _GNU_SOURCE
#include "ecm_api.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS 1024
typedef struct { uint64_t w[WORDS]; } payload_t;

static payload_t s0, s1;
static ecm_api_ho_t h;
static atomic_int done, started;
static atomic_ullong concurrent_gets;     /* gets made while the producer still ran */
static uint64_t n_puts, n_attempts;
#define MIN_CONCURRENT 1000

/* The producer waits for the consumer and keeps going until enough gets
 * overlapped with it: on a busy 2-CPU machine the consumer could otherwise
 * start after the producer finished, and then the `done` release/acquire
 * orders everything -- the negative control would pass by luck, not detect. */
static void *producer(void *arg)
{
    uint64_t n = *(uint64_t *)arg;
    while (!atomic_load_explicit(&started, memory_order_acquire)) { }
    for (uint64_t i = 1; i <= n || (atomic_load_explicit(&concurrent_gets, memory_order_relaxed) < MIN_CONCURRENT
                                     && i <= 100 * n); i++) {
        n_attempts = i;
        payload_t *p = ecm_api_ho_begin(&h);
        if (p) {
            for (int k = 0; k < WORDS; k++) p->w[k] = i;
            ecm_api_ho_commit(&h);
        }
    }
    n_puts = h.puts;
    atomic_store_explicit(&done, 1, memory_order_release);
    return NULL;
}

int main(int argc, char **argv)
{
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 10) : 200000;
    static payload_t got;
    ecm_api_ho_init(&h, &s0, &s1, sizeof(payload_t));
    pthread_t t;
    pthread_create(&t, NULL, producer, &n);
    uint64_t last = 0, gets = 0, torn = 0, back = 0;
    atomic_store_explicit(&started, 1, memory_order_release);
    for (;;) {
        int fin = atomic_load_explicit(&done, memory_order_acquire);
        while (ecm_api_ho_get(&h, &got)) {
            gets++;
            if (!fin) atomic_fetch_add_explicit(&concurrent_gets, 1, memory_order_relaxed);
            for (int k = 1; k < WORDS; k++) if (got.w[k] != got.w[0]) { torn++; break; }
            if (got.w[0] <= last) back++;
            last = got.w[0];
        }
        if (fin) break;
    }
    pthread_join(t, NULL);
    uint64_t cg = atomic_load(&concurrent_gets);
    printf("handoff: %" PRIu64 " attempts, %" PRIu64 " put, %" PRIu64 " dropped, %" PRIu64 " got "
           "(%" PRIu64 " while the producer ran), %" PRIu64 " torn, %" PRIu64 " out of order, last %" PRIu64 "\n",
           n_attempts, n_puts, h.drops, gets, cg, torn, back, last);
    if (cg < MIN_CONCURRENT) { printf("INCONCLUSIVE: fewer than %d gets overlapped the producer\n", MIN_CONCURRENT); return 1; }
    return (torn || back || gets != n_puts || n_puts + h.drops != n_attempts) ? 1 : 0;
}
