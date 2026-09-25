#include "ring_spsc.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    ring_spsc_t r;
    ring_init(&r);

    rt_sample_t s = {0};
    rt_sample_t out;

    /* T1: pop on an empty ring must fail */
    assert(ring_pop(&r, &out) == -1);
    printf("T1 OK: pop on empty ring returns -1\n");

    /* T2: fill exactly RING_CAPACITY elements, none lost */
    for (uint64_t i = 0; i < RING_CAPACITY; i++) {
        s.tick = i;
        assert(ring_push(&r, &s) == 0);
    }
    printf("T2 OK: pushed %d elements successfully\n", RING_CAPACITY);

    /* T3: once full, push must fail and increment drop_count */
    s.tick = 999999;
    assert(ring_push(&r, &s) == -1);
    assert(r.drop_count == 1);
    printf("T3 OK: push on full ring returns -1, drop_count=1\n");

    /* T4: pop back in correct FIFO order, correct tick values */
    for (uint64_t i = 0; i < RING_CAPACITY; i++) {
        assert(ring_pop(&r, &out) == 0);
        assert(out.tick == i);
    }
    printf("T4 OK: popped in correct FIFO order, %d elements\n", RING_CAPACITY);

    /* T5: after draining everything, the ring is empty again */
    assert(ring_pop(&r, &out) == -1);
    printf("T5 OK: back to empty after draining\n");

    printf("ALL PASS\n");
    return 0;
}