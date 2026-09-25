#pragma once
#include <stdint.h>

#define HIST_MIN_NS              100      /* 100ns */
#define HIST_DECADES             6        /* 100ns..100ms = 6 decades */
#define HIST_BUCKETS_PER_DECADE  20
#define HIST_NUM_BUCKETS         (HIST_DECADES * HIST_BUCKETS_PER_DECADE)  /* 120 */
#define HIST_OVERFLOW_IDX        HIST_NUM_BUCKETS   /* bucket 120: any value > 100ms */

typedef struct {
    uint64_t bucket_upper_ns[HIST_NUM_BUCKETS];  /* upper bound of each bucket, computed once at init */
    uint64_t buckets[HIST_NUM_BUCKETS + 1];      /* +1 for overflow */
    uint64_t count;
    uint64_t sum_ns;
    int64_t  min_ns;
    int64_t  max_ns;
} ecm_hist_t;

void     hist_init(ecm_hist_t *h);
void     hist_add(ecm_hist_t *h, int64_t ns);
uint64_t hist_percentile(const ecm_hist_t *h, double p);   /* p = 0.50 / 0.99 / 0.999 / 0.9999 */
void     hist_print(const ecm_hist_t *h, const char *label);