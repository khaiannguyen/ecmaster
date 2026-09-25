#include "ring_spsc.h"
#include <string.h>

void ring_init(ring_spsc_t *r) {
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
}

int ring_push(ring_spsc_t *r, const rt_sample_t *s) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (head - tail >= RING_CAPACITY) {
        r->drop_count++;   /* only the producer touches this, no atomic needed */
        return -1;
    }

    r->buf[head & (RING_CAPACITY - 1)] = *s;
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return 0;
}

int ring_pop(ring_spsc_t *r, rt_sample_t *out) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (tail == head) return -1;

    *out = r->buf[tail & (RING_CAPACITY - 1)];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return 0;
}