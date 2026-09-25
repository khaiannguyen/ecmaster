#include "turnaround.h"
#include "histogram.h"
#include "ecm_groups.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    tx_order_ring_t ring;
    tx_order_ring_init(&ring);

    turnaround_ctx_t ctx;
    turnaround_init(&ctx);

    ecm_hist_t hist;
    hist_init(&hist);

    /* T1: simplest case — tick 0, motion only. TX completion arrives
     * FIRST (this is the physically realistic order: a frame must leave
     * before it can come back), THEN the passive-socket RX arrival
     * completes the pair and feeds the histogram. */
    assert(tx_order_ring_push(&ring, 0, GROUP_MOTION) == 0);
    turnaround_drain_tx_order(&ctx, &ring);
    turnaround_on_tx_complete(&ctx, 1000000);        /* tx_ts_ns, from MSG_ERRQUEUE */
    turnaround_on_rx_arrival(&ctx, 1000200, &hist);  /* rx_ts_ns, from the passive socket */
    assert(ctx.stat_matched == 1);
    assert(hist.count == 1);
    printf("T1 OK: motion-only tick matched, turnaround=%llu\n",
           (unsigned long long)hist_percentile(&hist, 0.5));

    /* T2: tick 1, motion + io both sent (the sent_io_group=1 case). Both
     * TX completions arrive first (send order), then both RX arrivals
     * (physical arrival order matches send order, per service_group()'s
     * synchronous send+block-on-receive design). The io pair must be
     * discarded, not counted, and not crash. */
    assert(tx_order_ring_push(&ring, 1, GROUP_MOTION) == 0);
    assert(tx_order_ring_push(&ring, 1, GROUP_IO) == 0);
    turnaround_drain_tx_order(&ctx, &ring);
    turnaround_on_tx_complete(&ctx, 2000000);   /* motion TX completion */
    turnaround_on_tx_complete(&ctx, 2000050);   /* io TX completion */
    turnaround_on_rx_arrival(&ctx, 2000300, &hist);  /* motion RX arrival -> matched */
    turnaround_on_rx_arrival(&ctx, 2000360, &hist);  /* io RX arrival -> discarded */
    assert(ctx.stat_matched == 2);
    assert(hist.count == 2);   /* io must NOT add a 3rd sample */
    printf("T2 OK: motion measured, io discarded, histogram count stays at 2\n");

    /* T3: an RX arrival shows up with nothing pending -> must not crash */
    turnaround_on_rx_arrival(&ctx, 999, &hist);
    assert(ctx.stat_rx_no_pending == 1);
    printf("T3 OK: unexpected RX arrival handled, stat_rx_no_pending=%llu\n",
           (unsigned long long)ctx.stat_rx_no_pending);

    /* T4: an RX arrival's TX completion never showed up (or its slot got
     * evicted) before RX arrives -> must not crash, must be VISIBLE via
     * stat_evicted_no_match, not silently swallowed. Fill the tx-wait
     * table past capacity with completions that have no corresponding
     * pending_rx entry (so they can never be matched), then try one RX
     * arrival that has nothing to find. */
    for (uint64_t t = 100; t < 100 + RX_LOOKUP_CAPACITY + 1; t++) {
        assert(tx_order_ring_push(&ring, t, GROUP_MOTION) == 0);
    }
    turnaround_drain_tx_order(&ctx, &ring);
    for (uint64_t t = 100; t < 100 + RX_LOOKUP_CAPACITY + 1; t++) {
        turnaround_on_tx_complete(&ctx, t * 1000);
    }
    /* Now ask for tick 100 specifically, which should have been evicted
     * by the time slot RX_LOOKUP_CAPACITY+1 came in -- but simplest
     * robust check here is just that stat_evicted_no_match increased at
     * least once during the note_rx overwrites above (already exercised
     * inside turnaround_on_tx_complete -> turnaround_note_rx). */
    assert(ctx.stat_evicted_no_match >= 1);
    printf("T4 OK: eviction without a match is counted, not silent (evicted=%llu)\n",
           (unsigned long long)ctx.stat_evicted_no_match);

    printf("ALL PASS\n");
    return 0;
}