#include "histogram.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

void hist_init(ecm_hist_t *h) {
    memset(h, 0, sizeof(*h));
    h->min_ns = INT64_MAX;
    h->max_ns = 0;

    /* Log-linear, base 10, 20 buckets/decade => each bucket is ~10^(1/20)
       (~1.122x) wider than the previous one. Computed once here at init;
       hist_add() never calls pow(). */
    for (int i = 0; i < HIST_NUM_BUCKETS; i++) {
        double exponent = (double)(i + 1) / HIST_BUCKETS_PER_DECADE;
        h->bucket_upper_ns[i] = (uint64_t)(HIST_MIN_NS * pow(10.0, exponent));
    }
}

static int hist_bucket_index(const ecm_hist_t *h, int64_t ns) {
    if (ns < HIST_MIN_NS) return 0;   /* below range: fold into bucket 0 instead of silently dropping */
    if ((uint64_t)ns > h->bucket_upper_ns[HIST_NUM_BUCKETS - 1]) return HIST_OVERFLOW_IDX;

    int lo = 0, hi = HIST_NUM_BUCKETS - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if ((uint64_t)ns <= h->bucket_upper_ns[mid]) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

void hist_add(ecm_hist_t *h, int64_t ns) {
    int idx = hist_bucket_index(h, ns);
    h->buckets[idx]++;
    h->count++;
    h->sum_ns += ns;
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
}

uint64_t hist_percentile(const ecm_hist_t *h, double p) {
    if (h->count == 0) return 0;
    uint64_t target = (uint64_t)(p * (double)h->count);
    if (target >= h->count) target = h->count - 1;

    uint64_t running = 0;
    for (int i = 0; i <= HIST_NUM_BUCKETS; i++) {
        running += h->buckets[i];
        if (running > target) {
            if (i == HIST_OVERFLOW_IDX) return h->max_ns;  /* genuinely overflowed the 100ms range */
            return h->bucket_upper_ns[i];
        }
    }
    return h->max_ns;
}

void hist_print(const ecm_hist_t *h, const char *label) {
    printf("%-20s n=%-8llu p50=%8lluns p99=%8lluns p99.9=%8lluns p99.99=%8lluns max=%8lluns overflow=%llu\n",
           label, (unsigned long long)h->count,
           (unsigned long long)hist_percentile(h, 0.50),
           (unsigned long long)hist_percentile(h, 0.99),
           (unsigned long long)hist_percentile(h, 0.999),
           (unsigned long long)hist_percentile(h, 0.9999),
           (unsigned long long)h->max_ns,
           (unsigned long long)h->buckets[HIST_OVERFLOW_IDX]);
}