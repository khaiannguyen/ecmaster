/* ==========================================================================
 * ecm_policy.c — see ecm_policy.h and docs/fault_policy.md.
 * ========================================================================== */
#include <string.h>

#include "ecm_policy.h"

/* AL status bits (ETG1000.6 / ESC datasheet 0x0130) */
#define AL_INIT    0x01
#define AL_PREOP   0x02
#define AL_SAFEOP  0x04
#define AL_OP      0x08
#define AL_ERR     0x10

int ecm_rx_timeout_us(uint64_t now_ns, uint64_t deadline_ns, int min_us, int max_us)
{
    int64_t left_us = deadline_ns > now_ns ? (int64_t)((deadline_ns - now_ns) / 1000u) : 0;
    if (left_us < min_us) left_us = min_us;
    if (left_us > max_us) left_us = max_us;
    return (int)left_us;
}

/* ---- bus state machine ---------------------------------------------------- */
void ecm_bus_default_cfg(ecm_bus_cfg_t *cfg)
{
    cfg->n_lost = 100;
    cfg->probe_ticks = 10;
    cfg->recover_backoff_ticks = 1000;
}

void ecm_bus_init(ecm_bus_fsm_t *f, const ecm_bus_cfg_t *cfg)
{
    memset(f, 0, sizeof(*f));
    f->cfg = *cfg;
    if (f->cfg.n_lost == 0) f->cfg.n_lost = 1;
    if (f->cfg.probe_ticks == 0) f->cfg.probe_ticks = 1;
    f->state = ECM_BUS_RUN;
    f->io_ok = 1;
    f->entered[ECM_BUS_RUN] = 1;
}

const char *ecm_bus_state_name(ecm_bus_state_t s)
{
    static const char *const names[ECM_BUS_NSTATE] = { "RUN", "DEGRADED", "LOST", "RECOVER" };
    return (unsigned)s < ECM_BUS_NSTATE ? names[s] : "?";
}

static int go(ecm_bus_fsm_t *f, uint64_t tick, ecm_bus_state_t to, int reason, int group, ecm_event_t *ev)
{
    ecm_event_t e = { .tick = tick, .type = ECM_EV_BUS, .from = (uint8_t)f->state, .to = (uint8_t)to,
                      .a = reason, .b = group };
    f->state = to;
    f->since_tick = tick;
    f->entered[to]++;
    if (ev) *ev = e;
    return 1;
}

int ecm_bus_on_cycle(ecm_bus_fsm_t *f, uint64_t tick, int motion_class, int io_class, ecm_event_t *ev)
{
    if (f->state != ECM_BUS_RUN && f->state != ECM_BUS_DEGRADED) return 0;

    if (motion_class == ECM_POL_WKC_NOFRAME) f->noframe_run++;
    else f->noframe_run = 0;
    if (io_class >= 0) { f->io_ok = (io_class == ECM_POL_WKC_OK); f->io_last_class = io_class; }

    if (f->noframe_run >= f->cfg.n_lost) {
        f->next_probe_tick = tick + 1;
        return go(f, tick, ECM_BUS_LOST, ECM_POL_WKC_NOFRAME, 0, ev);
    }
    int bad = (motion_class != ECM_POL_WKC_OK) || !f->io_ok;
    if (f->state == ECM_BUS_RUN && bad) {
        int motion_bad = motion_class != ECM_POL_WKC_OK;
        return go(f, tick, ECM_BUS_DEGRADED, motion_bad ? motion_class : f->io_last_class,
                  motion_bad ? 0 : 1, ev);
    }
    if (f->state == ECM_BUS_DEGRADED && !bad)
        return go(f, tick, ECM_BUS_RUN, ECM_POL_WKC_OK, 0, ev);
    return 0;
}

int ecm_bus_probe_due(const ecm_bus_fsm_t *f, uint64_t tick)
{
    return f->state == ECM_BUS_LOST && tick >= f->next_probe_tick;
}

int ecm_bus_on_probe(ecm_bus_fsm_t *f, uint64_t tick, int brd_wkc, ecm_event_t *ev)
{
    if (f->state != ECM_BUS_LOST) return 0;
    if (brd_wkc > 0) return go(f, tick, ECM_BUS_RECOVER, brd_wkc, 0, ev);
    f->next_probe_tick = tick + f->cfg.probe_ticks;
    return 0;
}

int ecm_bus_on_recover_done(ecm_bus_fsm_t *f, uint64_t tick, int ok, ecm_event_t *ev)
{
    if (f->state != ECM_BUS_RECOVER) return 0;
    f->noframe_run = 0;
    f->io_ok = 1;
    if (ok) return go(f, tick, ECM_BUS_RUN, 0, 0, ev);
    f->next_probe_tick = tick + f->cfg.recover_backoff_ticks;
    return go(f, tick, ECM_BUS_LOST, -1, 0, ev);
}

void ecm_bus_tick(ecm_bus_fsm_t *f)
{
    f->ticks_in[f->state]++;
}

/* ---- SPSC rings ------------------------------------------------------------ */
void ecm_evring_init(ecm_evring_t *r)
{
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
}

int ecm_evring_push(ecm_evring_t *r, const ecm_event_t *ev)
{
    unsigned h = atomic_load_explicit(&r->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (h - t >= ECM_EVRING_LEN) { r->drops++; return 0; }
    r->e[h & (ECM_EVRING_LEN - 1)] = *ev;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 1;
}

int ecm_evring_pop(ecm_evring_t *r, ecm_event_t *out)
{
    unsigned t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t == h) return 0;
    *out = r->e[t & (ECM_EVRING_LEN - 1)];
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

void ecm_cmdq_init(ecm_cmdq_t *q)
{
    memset(q, 0, sizeof(*q));
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
}

int ecm_cmdq_push(ecm_cmdq_t *q, const ecm_cmd_t *c)
{
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (h - t >= ECM_CMDQ_LEN) { q->drops++; return 0; }
    q->c[h & (ECM_CMDQ_LEN - 1)] = *c;
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
    return 1;
}

int ecm_cmdq_peek(ecm_cmdq_t *q, ecm_cmd_t *out)
{
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t == h) return 0;
    *out = q->c[t & (ECM_CMDQ_LEN - 1)];
    return 1;
}

void ecm_cmdq_drop_head(ecm_cmdq_t *q)
{
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t != h) atomic_store_explicit(&q->tail, t + 1, memory_order_release);
}

/* ---- per-slave recovery planner -------------------------------------------- */
void ecm_srec_init(ecm_srec_plan_t *p, int n, uint32_t max_attempts, uint64_t backoff0_ns)
{
    memset(p, 0, sizeof(*p));
    p->n = n > ECM_SREC_MAX_SLAVES ? ECM_SREC_MAX_SLAVES : n;
    p->max_attempts = max_attempts;
    p->backoff0_ns = backoff0_ns;
}

void ecm_srec_reset(ecm_srec_plan_t *p, int i)
{
    if (i < 0 || i >= p->n) return;
    ecm_srec_t *s = &p->s[i];
    s->failed = 0;
    s->attempts = 0;
    s->next_try_ns = 0;
}

const char *ecm_sact_name(ecm_sact_t a)
{
    switch (a) {
    case ECM_SACT_NONE:     return "none";
    case ECM_SACT_ACK:      return "ack";
    case ECM_SACT_OP:       return "request OP";
    case ECM_SACT_RECONFIG: return "recover+reconfigure";
    }
    return "?";
}

ecm_sact_t ecm_srec_decide(ecm_srec_plan_t *p, int i, const ecm_srec_input_t *in,
                           uint64_t now_ns, int *recovered, int *gave_up)
{
    if (recovered) *recovered = 0;
    if (gave_up) *gave_up = 0;
    if (i < 0 || i >= p->n) return ECM_SACT_NONE;
    ecm_srec_t *s = &p->s[i];

    /* Behind a broken chain: nothing can be done from this side, and it is
     * not the slave's fault -- no attempt counted. */
    if (!in->in_reach) return ECM_SACT_NONE;

    uint8_t st = (uint8_t)(in->al_status & 0x0F);
    int err = (in->al_status & AL_ERR) != 0;
    if (in->answered && st == AL_OP && !err) {
        if (s->unhealthy) { s->recoveries++; if (recovered) *recovered = 1; }
        s->unhealthy = 0;
        s->attempts = 0;
        s->failed = 0;
        s->next_try_ns = 0;
        s->last = ECM_SACT_NONE;
        return ECM_SACT_NONE;
    }
    s->unhealthy = 1;
    if (s->failed) return ECM_SACT_NONE;

    ecm_sact_t act;
    if (!in->answered || st == AL_INIT || st == AL_PREOP) act = ECM_SACT_RECONFIG;
    else if (err)                                         act = ECM_SACT_ACK;
    else if (st == AL_SAFEOP)                             act = ECM_SACT_OP;
    else                                                  act = ECM_SACT_NONE;   /* BOOT etc. */

    /* ACK and RECONFIG start a new attempt: bounded and backed off. OP is
     * the second half of an attempt already started, so it is not gated. */
    if (act == ECM_SACT_ACK || act == ECM_SACT_RECONFIG) {
        if (now_ns < s->next_try_ns) return ECM_SACT_NONE;
        if (s->attempts >= p->max_attempts) {
            s->failed = 1;
            if (gave_up) *gave_up = 1;
            return ECM_SACT_NONE;
        }
        s->attempts++;
        uint32_t sh = s->attempts - 1 > 20 ? 20 : s->attempts - 1;
        s->next_try_ns = now_ns + (p->backoff0_ns << sh);
    }
    if (act != ECM_SACT_NONE) { s->last = act; s->actions[act]++; }
    return act;
}

/* ---- input freshness ---------------------------------------------------- */
void ecm_fresh_init(ecm_fresh_t *f, uint32_t stale_after, int bits)
{
    memset(f, 0, sizeof(*f));
    f->stale_after = stale_after ? stale_after : 1;
    f->bits = (bits == 8 || bits == 32) ? bits : 16;
}

ecm_fresh_ev_t ecm_fresh_update(ecm_fresh_t *f, uint32_t value)
{
    uint32_t mask = f->bits == 32 ? 0xFFFFFFFFu : ((1u << f->bits) - 1u);
    value &= mask;
    f->samples++;
    if (!f->have) { f->have = 1; f->last = value; return ECM_FRESH_FIRST; }

    uint32_t d = (value - f->last) & mask;
    if (d == 0) {
        f->run++;
        if (f->run > f->max_run) f->max_run = f->run;
        if (f->run < f->stale_after) return ECM_FRESH_UNCHANGED;
        f->stale_cycles++;
        if (!f->stale) { f->stale = 1; f->stale_episodes++; return ECM_FRESH_STALE_START; }
        return ECM_FRESH_STALE;
    }
    if (d > (mask >> 1)) {          /* "negative" difference: older than accepted */
        f->regressions++;
        return ECM_FRESH_REGRESSION; /* last stays: the next genuine value is newer */
    }
    f->last = value;
    f->run = 0;
    if (f->stale) { f->stale = 0; return ECM_FRESH_RESUMED; }
    return ECM_FRESH_OK;
}
