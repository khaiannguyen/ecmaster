#include "turnaround.h"
#include <string.h>

/* ---- tx_order_ring_t: identical SPSC pattern to ring_spsc.c ---- */

void tx_order_ring_init(tx_order_ring_t *r) {
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
}

int tx_order_ring_push(tx_order_ring_t *r, uint64_t tick, ecm_group_id_t g) {
    return tx_order_ring_push_idx(r, tick, g, TX_ORDER_IDX_UNKNOWN);
}

int tx_order_ring_push_idx(tx_order_ring_t *r, uint64_t tick, ecm_group_id_t g, uint8_t ec_idx) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (head - tail >= TX_ORDER_RING_CAPACITY) {
        r->drop_count++;
        return -1;
    }

    tx_order_sample_t *slot = &r->buf[head & (TX_ORDER_RING_CAPACITY - 1)];
    slot->tick = tick;
    slot->group_id = (uint8_t)g;
    slot->ec_idx = ec_idx;

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

void turnaround_resync(turnaround_ctx_t *c) {
    for (size_t i = 0; i < RX_LOOKUP_CAPACITY; i++) {
        c->entries[i].tick = UINT64_MAX;
    }
    c->next_slot = 0;
    c->pending_tx_head = c->pending_tx_tail = 0;
    c->pending_rx_head = c->pending_rx_tail = 0;
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

int turnaround_frame_idx(const uint8_t *eth, size_t len) {
    if (len < 18) return -1;
    if (eth[12] != 0x88 || eth[13] != 0xA4) return -1;
    return eth[17];
}

/* Take the pending send that this event (TX completion or RX arrival)
   belongs to. ec_idx < 0: the oldest, as before. Otherwise: first drop
   sends that are too old to still be answered, then the first pending send
   with that index; the sends before it never got theirs. Returns 0 (and
   touches nothing) when no pending send carries the index. */
static int fifo_take(tx_order_sample_t *q, size_t *tail, size_t head, int ec_idx,
                     uint64_t *skipped, uint64_t *unmatched, tx_order_sample_t *out) {
    if (*tail == head) return -1;
    if (ec_idx < 0) {
        *out = q[*tail % TX_ORDER_RING_CAPACITY];
        (*tail)++;
        return 1;
    }
    uint64_t newest = q[(head - 1) % TX_ORDER_RING_CAPACITY].tick;
    while (*tail != head) {
        const tx_order_sample_t *e = &q[*tail % TX_ORDER_RING_CAPACITY];
        if (newest - e->tick <= TURNAROUND_MAX_AGE_TICKS) break;
        (*tail)++;
        (*skipped)++;
    }
    for (size_t j = 0; j < TURNAROUND_LOOKAHEAD && *tail + j < head; j++) {
        const tx_order_sample_t *e = &q[(*tail + j) % TX_ORDER_RING_CAPACITY];
        if (e->ec_idx == (uint8_t)ec_idx) {
            *skipped += j;
            *tail += j + 1;
            *out = *e;
            return 1;
        }
    }
    /* No send carries this index. If the oldest pending send's index is
     * unknown (e.g. a mailbox frame SOEM sent internally), this event is
     * most likely its own: pair positionally. */
    if (*tail != head && q[*tail % TX_ORDER_RING_CAPACITY].ec_idx == TX_ORDER_IDX_UNKNOWN) {
        *out = q[*tail % TX_ORDER_RING_CAPACITY];
        (*tail)++;
        return 1;
    }
    (*unmatched)++;
    return 0;
}

void turnaround_on_tx_complete(turnaround_ctx_t *c, uint64_t tx_ts_ns) {
    turnaround_on_tx_complete_idx(c, tx_ts_ns, -1);
}

void turnaround_on_tx_complete_idx(turnaround_ctx_t *c, uint64_t tx_ts_ns, int ec_idx) {
    tx_order_sample_t s;
    int r = fifo_take(c->pending_tx, &c->pending_tx_tail, c->pending_tx_head, ec_idx,
                      &c->stat_tx_skipped, &c->stat_tx_unmatched, &s);
    if (r < 0) { c->stat_completion_no_pending++; return; }
    if (r == 0) return;

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
    turnaround_on_rx_arrival_idx(c, rx_ts_ns, -1, hist);
}

void turnaround_on_rx_arrival_idx(turnaround_ctx_t *c, uint64_t rx_ts_ns, int ec_idx, ecm_hist_t *hist) {
    tx_order_sample_t s;
    int r = fifo_take(c->pending_rx, &c->pending_rx_tail, c->pending_rx_head, ec_idx,
                      &c->stat_rx_skipped, &c->stat_rx_unmatched, &s);
    if (r < 0) { c->stat_rx_no_pending++; return; }
    if (r == 0) return;

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