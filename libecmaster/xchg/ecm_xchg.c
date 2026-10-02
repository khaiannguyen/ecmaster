/* ecm_xchg.c — Phase 10.4, see ecm_xchg.h and docs/app_rt_exchange.md */
#include "ecm_xchg.h"

#include <string.h>

/* Negative controls (tests only, never in a real build):
 *   ECM_XRING_BROKEN  the producer publishes head with memory_order_relaxed
 *                     -> TSan must report the element copy as a race
 *   ECM_XST_BROKEN    the reader takes one copy without checking seq
 *                     -> torn records must be seen */
#ifdef ECM_XRING_BROKEN
#define XRING_PUBLISH memory_order_relaxed
#else
#define XRING_PUBLISH memory_order_release
#endif

/* ---- ring --------------------------------------------------------------- */

/* Elements are copied word by word, NOT with memcpy: gcc inlines a fixed-size
 * memcpy without ThreadSanitizer instrumentation, which hid a broken ring
 * from TSan (Q-05 negative control passed silently with memcpy). */
static inline void copy32(uint64_t *d, const uint64_t *s)
{
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
}

void ecm_xring_init(ecm_xring_t *r, size_t cap)
{
    memset(r->buf, 0, sizeof(r->buf));
    if (cap == 0 || cap > ECM_XRING_CAP || (cap & (cap - 1))) cap = ECM_XRING_CAP;
    r->mask = cap - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    r->full_drops = 0;
}

int ecm_xring_push(ecm_xring_t *r, const void *e32)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head - tail > r->mask) { r->full_drops++; return -1; }
    copy32(r->buf[head & r->mask], (const uint64_t *)e32);
    atomic_store_explicit(&r->head, head + 1, XRING_PUBLISH);
    return 0;
}

int ecm_xring_peek(ecm_xring_t *r, void *e32)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail == head) return -1;
    copy32((uint64_t *)e32, r->buf[tail & r->mask]);
    return 0;
}

void ecm_xring_pop(ecm_xring_t *r)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}

/* ---- seqlock ------------------------------------------------------------ */

void ecm_xst_write(ecm_xst_t *s, uint64_t tick, const uint64_t *w, int n)
{
    uint32_t q = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, q + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->tick, tick, memory_order_relaxed);
    for (int i = 0; i < n && i < ECM_XST_WORDS; i++)
        atomic_store_explicit(&s->w[i], w[i], memory_order_relaxed);
    atomic_store_explicit(&s->seq, q + 2, memory_order_release);
}

int ecm_xst_read(ecm_xst_t *s, uint64_t *tick, uint64_t *w, int n, int tries)
{
    for (int t = 0; t < tries; t++) {
        uint32_t s1 = atomic_load_explicit(&s->seq, memory_order_acquire);
#ifndef ECM_XST_BROKEN
        if (s1 & 1) continue;
#endif
        *tick = atomic_load_explicit(&s->tick, memory_order_relaxed);
        for (int i = 0; i < n && i < ECM_XST_WORDS; i++)
            w[i] = atomic_load_explicit(&s->w[i], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
#ifdef ECM_XST_BROKEN
        (void)s1;
        return 0;
#else
        if (atomic_load_explicit(&s->seq, memory_order_relaxed) == s1) return 0;
#endif
    }
    return -1;
}

/* ---- exchange ------------------------------------------------------------ */

void ecm_xchg_init(ecm_xchg_t *x, uint64_t cycle_ns)
{
    memset(x, 0, sizeof(*x));
    x->cycle_ns = cycle_ns;
    ecm_xring_init(&x->cmd, ECM_XCHG_CMD_CAP);
    for (int i = 0; i < ECM_XCHG_MAX_SLOTS; i++) ecm_xring_init(&x->sp[i], ECM_XRING_CAP);
}

int ecm_xchg_add_slot(ecm_xchg_t *x, const ecm_pdo_handle_t *h)
{
    if (x->nslots >= ECM_XCHG_MAX_SLOTS || !h || !h->bits) return -1;
    ecm_xslot_t *s = &x->slot[x->nslots];
    memset(s, 0, sizeof(*s));
    s->h = *h;
    s->is_out = h->dir == ECM_PDO_OUT;
    return x->nslots++;
}

static void apply(ecm_xslot_t *s, uint8_t *iomap, int64_t v)
{
    ecm_pdo_set(&s->h, iomap, (uint64_t)v);
    s->last = v;
    s->armed = 1;
}

void ecm_xchg_rt(ecm_xchg_t *x, uint8_t *iomap, uint64_t tick, uint64_t t_send_ns,
                 int in_valid, int bus_lost)
{
    const uint64_t k1 = tick + 1;            /* the frame the outputs go out in */

    /* 1. commands, FIFO, a bounded number per tick */
    int n = 0;
    ecm_xcmd_t c;
    while (ecm_xring_peek(&x->cmd, &c) == 0) {
        if (c.tick > k1) break;              /* not yet: keeps its place */
        if (n == ECM_XCHG_CMD_PER_TICK) { x->cmd_deferred++; break; }
        if (c.op == ECM_XOP_SET && c.target < x->nslots && x->slot[c.target].is_out && !bus_lost)
            apply(&x->slot[c.target], iomap, c.arg0);
        else if (c.op != ECM_XOP_NOP && c.op != ECM_XOP_SET)
            x->cmd_unknown++;                /* owner ops: handled by the owner's hook */
        x->last_seq = c.seq;
        x->cmd_applied++;
        ecm_xring_pop(&x->cmd);
        n++;
    }

    /* 2. setpoints for k+1, per output slot */
    for (int i = 0; i < x->nslots; i++) {
        ecm_xslot_t *s = &x->slot[i];
        if (!s->is_out) continue;
        ecm_xring_t *r = &x->sp[i];
        ecm_xsp_t e;
        int got = 0, skipped = 0;
        while (ecm_xring_peek(r, &e) == 0) {
            if (e.tick < k1 || (bus_lost && e.tick == k1)) {
                /* late, or its frame cannot be sent (no bus): dropped, never
                 * replayed when the bus comes back */
                if (skipped == ECM_XCHG_SP_SKIP_MAX) break;
                if (bus_lost) s->dropped_lost++; else s->late++;
                ecm_xring_pop(r);
                skipped++;
                continue;
            }
            if (bus_lost) break;              /* a later tick: decided then */
            if (e.tick == k1) {
                apply(s, iomap, e.v[0]);
                s->used++;
                ecm_xring_pop(r);
                got = 1;
            }
            break;                            /* e.tick > k1: for a later tick */
        }
        if (!got && s->armed && !bus_lost) {
            s->underrun++;
            ecm_pdo_set(&s->h, iomap, (uint64_t)s->last);   /* hold the last value */
        }
    }

    /* 3. state */
    for (int i = 0; i < x->nslots; i++) {
        ecm_xslot_t *s = &x->slot[i];
        uint64_t w[ECM_XS_NWORDS];
        int valid = s->is_out ? s->armed : in_valid;
        w[ECM_XS_VALUE] = (s->is_out || in_valid) ? ecm_pdo_get(&s->h, iomap) : 0;
        w[ECM_XS_USED] = s->used;
        w[ECM_XS_LATE] = s->late;
        w[ECM_XS_UNDERRUN] = s->underrun;
        w[ECM_XS_DROPPED_LOST] = s->dropped_lost;
        w[ECM_XS_VALID] = (uint64_t)valid;
        if (!s->is_out && !in_valid) {       /* keep the last good input value */
            uint64_t t0, old[ECM_XST_WORDS];
            if (ecm_xst_read(&x->st[i], &t0, old, ECM_XS_NWORDS, 1) == 0) w[ECM_XS_VALUE] = old[ECM_XS_VALUE];
        }
        ecm_xst_write(&x->st[i], tick, w, ECM_XS_NWORDS);
    }
    uint64_t cw[ECM_XC_NWORDS] = {
        [ECM_XC_T_SEND_NS] = t_send_ns, [ECM_XC_CYCLE_NS] = x->cycle_ns,
        [ECM_XC_IN_VALID] = (uint64_t)in_valid, [ECM_XC_BUS_LOST] = (uint64_t)bus_lost,
        [ECM_XC_CMD_APPLIED] = x->cmd_applied, [ECM_XC_CMD_DEFERRED] = x->cmd_deferred,
        [ECM_XC_LAST_SEQ] = x->last_seq, [ECM_XC_CMD_UNKNOWN] = x->cmd_unknown,
    };
    ecm_xst_write(&x->clock, tick, cw, ECM_XC_NWORDS);
}

int ecm_xchg_cmd(ecm_xchg_t *x, const ecm_xcmd_t *c)
{
    return ecm_xring_push(&x->cmd, c);
}

int ecm_xchg_setpoint(ecm_xchg_t *x, int slot, uint64_t tick, int64_t v0)
{
    if (slot < 0 || slot >= x->nslots || !x->slot[slot].is_out) return -1;
    ecm_xsp_t e = { .tick = tick, .v = { v0, 0, 0 } };
    return ecm_xring_push(&x->sp[slot], &e);
}

int ecm_xchg_read_clock(ecm_xchg_t *x, uint64_t *tick, uint64_t w[ECM_XST_WORDS])
{
    return ecm_xst_read(&x->clock, tick, w, ECM_XC_NWORDS, ECM_XST_TRIES);
}

int ecm_xchg_read_slot(ecm_xchg_t *x, int slot, uint64_t *tick, uint64_t w[ECM_XST_WORDS])
{
    if (slot < 0 || slot >= x->nslots) return -1;
    return ecm_xst_read(&x->st[slot], tick, w, ECM_XS_NWORDS, ECM_XST_TRIES);
}
