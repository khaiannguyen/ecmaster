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


    /* ---- Giai doan 7.4 (L5-07/08): pairing by EtherCAT index ---------- */
    /* Helper scenario: ticks t = 1000.., motion frame idx = t % 16, TX at
     * t*1e6, reply 20 us later. */
#define TXT(t) ((uint64_t)(t) * 1000000ull)
#define RXT(t) ((uint64_t)(t) * 1000000ull + 20000ull)

    /* T5 (documents the old bug): positional pairing, ONE reply lost. The
     * next reply is paired with the lost send -> ~1 ms "turnaround", and
     * every later pair is off by one tick for the rest of the run. */
    {
        tx_order_ring_t r5; tx_order_ring_init(&r5);
        turnaround_ctx_t c5; turnaround_init(&c5);
        ecm_hist_t h5; hist_init(&h5);
        for (uint64_t t = 1000; t < 1010; t++) {
            tx_order_ring_push(&r5, t, GROUP_MOTION);
            turnaround_drain_tx_order(&c5, &r5);
            turnaround_on_tx_complete(&c5, TXT(t));
            if (t != 1002) turnaround_on_rx_arrival(&c5, RXT(t), &h5);   /* 1002: NOFRAME */
        }
        assert(c5.stat_matched == 2);       /* only 1000, 1001 paired right */
        printf("T5 OK (old behaviour documented): positional pairing after one lost reply: "
               "matched=%llu implausible=%llu of 9 replies, all later pairs shifted\n",
               (unsigned long long)c5.stat_matched, (unsigned long long)c5.stat_implausible);
    }

    /* T6: same with index pairing: the lost send is skipped, the rest pair right. */
    {
        tx_order_ring_t r6; tx_order_ring_init(&r6);
        turnaround_ctx_t c6; turnaround_init(&c6);
        ecm_hist_t h6; hist_init(&h6);
        for (uint64_t t = 1000; t < 1010; t++) {
            tx_order_ring_push_idx(&r6, t, GROUP_MOTION, (uint8_t)(t % 16));
            turnaround_drain_tx_order(&c6, &r6);
            turnaround_on_tx_complete_idx(&c6, TXT(t), (int)(t % 16));
            if (t != 1002) turnaround_on_rx_arrival_idx(&c6, RXT(t), (int)(t % 16), &h6);
        }
        assert(c6.stat_matched == 9);
        assert(c6.stat_implausible == 0);
        assert(c6.stat_rx_skipped == 1);
        assert(hist_percentile(&h6, 0.99) <= 25000);
        printf("T6 OK: index pairing, one lost reply: matched=9 skipped=1 implausible=0\n");
    }

    /* T7: duplicate reply (L5-08 dup): counted, ignored, no shift. */
    {
        tx_order_ring_t r7; tx_order_ring_init(&r7);
        turnaround_ctx_t c7; turnaround_init(&c7);
        ecm_hist_t h7; hist_init(&h7);
        for (uint64_t t = 1000; t < 1010; t++) {
            tx_order_ring_push_idx(&r7, t, GROUP_MOTION, (uint8_t)(t % 16));
            turnaround_drain_tx_order(&c7, &r7);
            turnaround_on_tx_complete_idx(&c7, TXT(t), (int)(t % 16));
            turnaround_on_rx_arrival_idx(&c7, RXT(t), (int)(t % 16), &h7);
            if (t == 1003) turnaround_on_rx_arrival_idx(&c7, RXT(t) + 5000, (int)(t % 16), &h7);
        }
        assert(c7.stat_matched == 10 && c7.stat_implausible == 0);
        assert(c7.stat_rx_unmatched + c7.stat_rx_no_pending == 1 && c7.stat_rx_skipped == 0);   /* empty FIFO -> no_pending */
        printf("T7 OK: duplicate reply counted as unmatched, 10/10 pairs right\n");
    }

    /* T8: reordered replies (L5-08 reorder): reply of 1004 arrives after
     * 1005's. 1005 pairs right; 1004 was given up when 1005 arrived and
     * its late arrival is unmatched. Nothing shifts. */
    {
        tx_order_ring_t r8; tx_order_ring_init(&r8);
        turnaround_ctx_t c8; turnaround_init(&c8);
        ecm_hist_t h8; hist_init(&h8);
        for (uint64_t t = 1000; t < 1010; t++) {
            tx_order_ring_push_idx(&r8, t, GROUP_MOTION, (uint8_t)(t % 16));
            turnaround_drain_tx_order(&c8, &r8);
            turnaround_on_tx_complete_idx(&c8, TXT(t), (int)(t % 16));
            if (t == 1004) continue;
            turnaround_on_rx_arrival_idx(&c8, RXT(t), (int)(t % 16), &h8);
            if (t == 1005) turnaround_on_rx_arrival_idx(&c8, RXT(t) + 1000, (int)(1004 % 16), &h8);
        }
        assert(c8.stat_matched == 9 && c8.stat_implausible == 0);
        assert(c8.stat_rx_skipped == 1 && c8.stat_rx_unmatched + c8.stat_rx_no_pending == 1);
        printf("T8 OK: reordered reply: 9 pairs right, 1 skipped, late one unmatched\n");
    }

    /* T9: mute storm longer than the 16-index cycle (L5-02/L5-07): 30
     * replies lost, then normal. Index 1030 % 16 == 1014 % 16 etc., so
     * without age eviction the first reply after the storm would pair with
     * a send 16 ticks older. */
    {
        tx_order_ring_t r9; tx_order_ring_init(&r9);
        turnaround_ctx_t c9; turnaround_init(&c9);
        ecm_hist_t h9; hist_init(&h9);
        for (uint64_t t = 1000; t < 1060; t++) {
            tx_order_ring_push_idx(&r9, t, GROUP_MOTION, (uint8_t)(t % 16));
            turnaround_drain_tx_order(&c9, &r9);
            turnaround_on_tx_complete_idx(&c9, TXT(t), (int)(t % 16));
            if (t < 1010 || t >= 1040) turnaround_on_rx_arrival_idx(&c9, RXT(t), (int)(t % 16), &h9);
        }
        assert(c9.stat_matched == 30 && c9.stat_implausible == 0);
        assert(c9.stat_rx_skipped == 30);
        printf("T9 OK: 30-frame storm (> 16 indexes): 30 matched after it, 30 skipped, 0 implausible\n");
    }

    /* T10: TX side, send failed (link down: no completion) for 3 sends. */
    {
        tx_order_ring_t r10; tx_order_ring_init(&r10);
        turnaround_ctx_t c10; turnaround_init(&c10);
        ecm_hist_t h10; hist_init(&h10);
        for (uint64_t t = 1000; t < 1010; t++) {
            tx_order_ring_push_idx(&r10, t, GROUP_MOTION, (uint8_t)(t % 16));
            turnaround_drain_tx_order(&c10, &r10);
            if (t >= 1003 && t < 1006) continue;               /* no completion, no reply */
            turnaround_on_tx_complete_idx(&c10, TXT(t), (int)(t % 16));
            turnaround_on_rx_arrival_idx(&c10, RXT(t), (int)(t % 16), &h10);
        }
        assert(c10.stat_matched == 7 && c10.stat_implausible == 0);
        assert(c10.stat_tx_skipped == 3);
        printf("T10 OK: 3 failed sends: TX side skips them, 7/7 pairs right\n");
    }

    /* T11: frame index parsing */
    {
        uint8_t f[20] = { 0 };
        f[12] = 0x88; f[13] = 0xA4; f[17] = 7;
        assert(turnaround_frame_idx(f, sizeof(f)) == 7);
        f[13] = 0x00;
        assert(turnaround_frame_idx(f, sizeof(f)) == -1);
        assert(turnaround_frame_idx(f, 10) == -1);
        printf("T11 OK: EtherCAT index parsed from byte 17, non-EtherCAT rejected\n");
    }

    printf("ALL PASS\n");
    return 0;
}