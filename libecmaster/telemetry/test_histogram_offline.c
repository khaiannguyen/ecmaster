#include "histogram.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    ecm_hist_t h;
    hist_init(&h);

    /* 1000 samples at exactly 500ns -> p50 must land in the bucket containing 500ns */
    for (int i = 0; i < 1000; i++) hist_add(&h, 500);
    uint64_t p50 = hist_percentile(&h, 0.50);
    assert(p50 >= 450 && p50 <= 600);   /* ~1.122x bucket width, this margin is expected */
    printf("T1 OK: p50=%llu (expected ~500)\n", (unsigned long long)p50);

    /* 1 sample beyond the 100ms range -> must go to overflow, no crash */
    hist_add(&h, 200000000);
    assert(h.buckets[HIST_OVERFLOW_IDX] == 1);
    printf("T2 OK: overflow bucket = 1\n");

    /* negative sample (simulating an abnormal negative wake jitter) -> no crash, falls into bucket 0 */
    hist_add(&h, -50);
    assert(h.buckets[0] >= 1);
    printf("T3 OK: negative value does not crash\n");

    hist_print(&h, "test");
    printf("ALL PASS\n");
    return 0;
}