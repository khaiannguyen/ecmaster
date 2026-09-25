#include "turnaround.h"
#include <string.h>

/* ---- tx_order_ring_t: identical SPSC pattern to ring_spsc.c ---- */

void tx_order_ring_init(tx_order_ring_t *r) {
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
}

int tx_order_ring_push(tx_order_ring_t *r, uint64_t tick, ecm_group_id_t g) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (head - tail >= TX_ORDER_RING_CAPACITY) {
        r->drop_count++;
        return -1;
    }

    tx_order_sample_t *slot = &r->buf[head & (TX_ORDER_RING_CAPACITY - 1)];
    slot->tick = tick;
    slot->group_id = (uint8_t)g;

    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return 0;
}

int tx_order_ring_pop(tx_order_ring_t *r, tx_order_sample_t *out) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (tail == head) return -1;

    *out = r->buf[tail & (TX_ORDER_RING_CAPACITY - 1)];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return 0;
}

/* ---- turnaround correlator ---- */

void turnaround_init(turnaround_ctx_t *c) {
    memset(c, 0, sizeof(*c));
    for (size_t i = 0; i < RX_LOOKUP_CAPACITY; i++) {
        c->entries[i].tick = UINT64_MAX;  /* mark slot empty */
    }
}

void turnaround_note_rx(turnaround_ctx_t *c, uint64_t tick, uint64_t rx_ts_ns) {
    size_t idx = c->next_slot % RX_LOOKUP_CAPACITY;
    if (c->entries[idx].tick != UINT64_MAX) {
        /* overwriting a slot whose TX ts never arrived in time — count it,
           don't swallow it silently */
        c->stat_evicted_no_match++;
    }
    c->entries[idx].tick = tick;
    c->entries[idx].rx_ts_ns = rx_ts_ns;
    c->next_slot++;
}

static int rx_lookup_take(turnaround_ctx_t *c, uint64_t tick, uint64_t *out_rx_ts) {
    for (size_t i = 0; i < RX_LOOKUP_CAPACITY; i++) {
        if (c->entries[i].tick == tick) {
            *out_rx_ts = c->entries[i].rx_ts_ns;
            c->entries[i].tick = UINT64_MAX;  /* consumed */
            return 1;
        }
    }
    return 0;
}

void turnaround_drain_tx_order(turnaround_ctx_t *c, tx_order_ring_t *ring) {
    tx_order_sample_t s;
    while (tx_order_ring_pop(ring, &s) == 0) {
        if (c->pending_tx_head - c->pending_tx_tail >= TX_ORDER_RING_CAPACITY) {
            c->pending_tx_tail++;  /* defensive: drop oldest rather than overflow */
        }
        c->pending_tx[c->pending_tx_head % TX_ORDER_RING_CAPACITY] = s;
        c->pending_tx_head++;

        if (c->pending_rx_head - c->pending_rx_tail >= TX_ORDER_RING_CAPACITY) {
            c->pending_rx_tail++;
        }
        c->pending_rx[c->pending_rx_head % TX_ORDER_RING_CAPACITY] = s;
        c->pending_rx_head++;
    }
}

void turnaround_on_tx_complete(turnaround_ctx_t *c, uint64_t tx_ts_ns) {
    if (c->pending_tx_head == c->pending_tx_tail) {
        c->stat_completion_no_pending++;
        return;
    }
    tx_order_sample_t s = c->pending_tx[c->pending_tx_tail % TX_ORDER_RING_CAPACITY];
    c->pending_tx_tail++;

    if (s.group_id == GROUP_IO) {
        c->stat_tx_io_discarded++;
        return;   /* not part of the 4 core measured quantities in Giai đoạn 4, see §5.2 */
    }

    /* TX physically happens before RX (the frame must leave before it can
       come back), so this normally runs BEFORE the matching
       turnaround_on_rx_arrival() -- store tx_ts_ns here, waiting for RX
       to complete the pair and feed the histogram. */
    turnaround_note_rx(c, s.tick, tx_ts_ns);
}

void turnaround_on_rx_arrival(turnaround_ctx_t *c, uint64_t rx_ts_ns, ecm_hist_t *hist) {
    if (c->pending_rx_head == c->pending_rx_tail) {
        c->stat_rx_no_pending++;
        return;
    }
    tx_order_sample_t s = c->pending_rx[c->pending_rx_tail % TX_ORDER_RING_CAPACITY];
    c->pending_rx_tail++;

    if (s.group_id == GROUP_IO) {
        c->stat_rx_io_discarded++;
        return;   /* not measured, see §5.2 */
    }

    uint64_t tx_ts_ns;
    if (rx_lookup_take(c, s.tick, &tx_ts_ns)) {
        int64_t turnaround_ns = (int64_t)(rx_ts_ns - tx_ts_ns);
        if (turnaround_ns < 0 || turnaround_ns > TURNAROUND_SANITY_MAX_NS) {
            /* Almost certainly a pairing desync (pending_tx/pending_rx
               drifted apart -- e.g. the passive socket missed a frame at
               some point and everything since is off by a constant
               number of ticks), not a real physical delay. Don't let it
               into the histogram; count it so the desync is visible. */
            c->stat_implausible++;
        } else {
            hist_add(hist, turnaround_ns);
            c->stat_matched++;
        }
    } else {
        /* TX completion for this tick hasn't been read from the error
           queue yet (poll timing), or its slot was evicted -- already
           counted at eviction time in turnaround_note_rx(). Either way,
           don't fabricate a turnaround value; just skip this one. */
        c->stat_evicted_no_match++;
    }
}