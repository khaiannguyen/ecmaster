#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include "histogram.h"
#include "ecm_groups.h"   /* GROUP_MOTION / GROUP_IO — single source of truth,
                              shared with apps/ecm_run/ecm_run.c */

typedef uint8_t ecm_group_id_t;   /* holds GROUP_MOTION (1) or GROUP_IO (2) */

/* --- TX order ring: pushed by the RT thread at the exact moment of each
   sendto(), one entry per send call (up to 2 per tick: motion always,
   io on 1/8 ticks). Kept separate from the rt_sample_t ring (which is
   pushed once per full tick) because TX order must be captured
   send-by-send, in the exact order sendto() was called — that order is
   what lets us match MSG_ERRQUEUE completions correctly without ever
   reading back PDO payload (see giai_doan_4_ke_hoach.md §5.2). */

typedef struct {
    uint64_t tick;
    uint8_t  group_id;   /* ecm_group_id_t */
    uint8_t  _pad[7];
} tx_order_sample_t;

_Static_assert(sizeof(tx_order_sample_t) == 16, "tx_order_sample_t must be exactly 16 bytes");

#define TX_ORDER_RING_CAPACITY 256  /* power of 2; up to 2 pushes/tick, generous headroom */

typedef struct {
    tx_order_sample_t buf[TX_ORDER_RING_CAPACITY];
    _Atomic size_t     head;   /* written only by the RT thread (producer) */
    _Atomic size_t     tail;   /* written only by the telemetry thread (consumer) */
    uint64_t           drop_count;
} tx_order_ring_t;

void tx_order_ring_init(tx_order_ring_t *r);
/* Called from the RT thread, right after each sendto(). NEVER blocks. */
int  tx_order_ring_push(tx_order_ring_t *r, uint64_t tick, ecm_group_id_t g);
/* Called from the telemetry thread. */
int  tx_order_ring_pop(tx_order_ring_t *r, tx_order_sample_t *out);


/* --- Turnaround correlator: runs entirely in the telemetry thread, single
   threaded internally — no atomics needed below this point. --- */

#define RX_LOOKUP_CAPACITY 64   /* tick -> tx_ts_ns, waiting for the matching RX
                                    arrival (TX happens first physically, so
                                    this table is filled by turnaround_on_tx_
                                    complete() and drained by turnaround_on_rx_
                                    arrival()). Sized for a few tens of ms of
                                    worst-case MSG_ERRQUEUE delay at 1ms cycle
                                    time. Field kept named rx_ts_ns/note_rx for
                                    historical reasons -- it now holds whichever
                                    timestamp arrives FIRST for a tick, which in
                                    practice is always tx_ts_ns. */

typedef struct {
    uint64_t tick;       /* UINT64_MAX = empty/consumed slot */
    uint64_t rx_ts_ns;
} rx_lookup_slot_t;

typedef struct {
    rx_lookup_slot_t entries[RX_LOOKUP_CAPACITY];
    size_t next_slot;

    /* Two INDEPENDENT FIFOs draining the SAME tx_order_ring_t sequence,
       each advanced by its own consumer:
       - pending_tx is advanced by turnaround_on_tx_complete() (from
         MSG_ERRQUEUE, on ctx.port.sockhandle -- the socket SOEM itself
         uses to send).
       - pending_rx is advanced by turnaround_on_rx_arrival() (from a
         SEPARATE, passive AF_PACKET socket bound to the same interface,
         used only to observe genuine incoming frames -- see
         giai_doan_4_ke_hoach.md §5.2, option (b): this keeps nicdrv.c
         completely untouched).
       Both must exist because send order == both TX-completion order AND
       physical RX-arrival order (service_group() sends then blocks on
       its own reply before the next group is ever sent -- see
       ecm_run.c), so both consumers can independently walk the same
       ordered sequence without interfering with each other. */
    tx_order_sample_t pending_tx[TX_ORDER_RING_CAPACITY];
    size_t pending_tx_head, pending_tx_tail;

    tx_order_sample_t pending_rx[TX_ORDER_RING_CAPACITY];
    size_t pending_rx_head, pending_rx_tail;

    /* Observability counters — never silently swallow a mismatch, per the
       lesson from the SOEM buffer-overflow bug in Giai đoạn 3
       (giai_doan_3_tong_hop.md §3.5): a clean-looking run is not proof of
       correctness unless the failure paths are actually counted. */
    uint64_t stat_matched;               /* motion completions successfully matched */
    uint64_t stat_evicted_no_match;      /* rx entry evicted before its TX ts arrived */
    uint64_t stat_completion_no_pending; /* TX completion arrived with an empty pending_tx FIFO */
    uint64_t stat_rx_no_pending;         /* passive-socket RX arrival with an empty pending_rx FIFO */
    uint64_t stat_implausible;           /* matched pair whose turnaround was negative or absurdly
                                             large (see TURNAROUND_SANITY_MAX_NS) -- almost always
                                             means pending_tx/pending_rx desynced (e.g. the passive
                                             RX socket missed a frame and everything after it is now
                                             off by a constant number of ticks). NOT fed into the
                                             histogram -- counted here instead so a silent pairing
                                             bug shows up in the stats instead of polluting p50/p99. */
    uint64_t stat_tx_io_discarded;       /* TX completion for a GROUP_IO tick, correctly discarded
                                             (not a bug) -- counted so the numbers can be reconciled:
                                             stat_matched + stat_implausible + stat_tx_io_discarded +
                                             stat_completion_no_pending should equal the number of
                                             tx_order_ring pushes actually drained on the TX side. */
    uint64_t stat_rx_io_discarded;       /* same, for the RX/passive-socket side. */
} turnaround_ctx_t;

/* Above this, a "turnaround" is not physically plausible for this rig
   (real values are microseconds; this is generous on purpose, just a
   trip-wire for gross mismatches, not a real physical bound). */
#define TURNAROUND_SANITY_MAX_NS 1000000   /* 1ms */

void turnaround_init(turnaround_ctx_t *c);

/* Call once per tick as a DIAGNOSTIC cross-check only (compares the RT
   thread's own clock_gettime() estimate against the real passive-socket
   timestamp below) -- NOT the source of truth for turnaround anymore.
   Kept for compatibility with existing offline tests; real rx timestamps
   now come from turnaround_on_rx_arrival(). */
void turnaround_note_rx(turnaround_ctx_t *c, uint64_t tick, uint64_t rx_ts_ns);

/* Drain tx_order_ring_t into BOTH pending_tx and pending_rx. Call this
   once per telemetry poll cycle, before turnaround_on_tx_complete() and
   turnaround_on_rx_arrival(). */
void turnaround_drain_tx_order(turnaround_ctx_t *c, tx_order_ring_t *ring);

/* Call once per TX completion read from MSG_ERRQUEUE, in send order.
   Pops the oldest pending_tx (tick, group_id); if GROUP_MOTION, STORES
   tx_ts_ns keyed by tick, waiting for the matching RX arrival below.
   GROUP_IO completions are popped and discarded — see
   giai_doan_4_ke_hoach.md §5.2. Does NOT touch the histogram itself: TX
   physically happens before RX (a frame must leave before it can come
   back), so by the time this runs the RX side has normally not arrived
   yet — turnaround_on_rx_arrival() is where the pair completes and the
   histogram gets fed. */
void turnaround_on_tx_complete(turnaround_ctx_t *c, uint64_t tx_ts_ns);

/* Call once per genuine incoming frame observed by the passive RX socket
   (sll_pkttype == PACKET_HOST only -- the caller must filter out
   PACKET_OUTGOING before calling this), in physical arrival order. Pops
   the oldest pending_rx (tick, group_id); if GROUP_MOTION, looks up the
   tx_ts_ns stored by turnaround_on_tx_complete() for that tick and feeds
   (rx_ts_ns - tx_ts_ns) into 'hist'. GROUP_IO arrivals are popped and
   discarded. */
void turnaround_on_rx_arrival(turnaround_ctx_t *c, uint64_t rx_ts_ns, ecm_hist_t *hist);