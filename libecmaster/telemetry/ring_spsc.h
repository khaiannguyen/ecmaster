#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

/* Pushed by the RT thread — does NOT contain turnaround_ns, because the TX
   timestamp only comes back asynchronously via MSG_ERRQUEUE, read by the
   telemetry thread */
typedef struct {
    int64_t  wake_jitter_ns;
    int64_t  prep_send_ns;
    int64_t  cycle_occupancy_ns;
    uint64_t rx_ts_ns;   /* RX timestamp of the GROUP_MOTION frame for this tick only —
                         never GROUP_IO, even on ticks where sent_io_group=1.
                         Keeps the meaning of "turnaround" identical across
                         platforms regardless of motor count or group count. */
    uint64_t tick;
    uint8_t  sent_io_group;        /* 1 if this tick also sent GROUP_IO */
    uint8_t  any_wkc_mismatch;     /* OR of wkc mismatch across both groups */
    uint8_t  _pad[6];
} rt_sample_t;

/* Compile-time check — catches padding drift between x86 (CI) and arm64
   (Jetson) before it becomes a runtime bug.
   5 x 8-byte fields (40) + 2 x 1-byte flags + 6-byte pad (8) = 48 bytes. */
_Static_assert(sizeof(rt_sample_t) == 48, "rt_sample_t must be exactly 48 bytes");

#define RING_CAPACITY 4096   /* power of 2, so we can use & instead of % */

typedef struct {
    rt_sample_t    buf[RING_CAPACITY];
    _Atomic size_t head;      /* written only by the RT thread (producer) */
    _Atomic size_t tail;      /* written only by the telemetry thread (consumer) */
    uint64_t       drop_count; /* written only by the producer — samples dropped because the ring was full */
} ring_spsc_t;

void ring_init(ring_spsc_t *r);
/* Called from the RT thread. NEVER blocks. Returns 0=ok, -1=ring full (drop_count already incremented) */
int  ring_push(ring_spsc_t *r, const rt_sample_t *s);
/* Called from the telemetry thread. Returns 0=ok, -1=ring empty */
int  ring_pop(ring_spsc_t *r, rt_sample_t *out);