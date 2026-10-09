/* ecm_api.c -- Phase 11.1, see ecm_api.h and docs/api.md */
#define _GNU_SOURCE
#include "ecm_api.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/ecm_dc.h"
#include "../diag/ecm_diag.h"
#include "../policy/ecm_policy.h"

/* ------------------------------------------------------------ handoff --- */
void ecm_api_ho_init(ecm_api_ho_t *h, void *slot0, void *slot1, size_t size)
{
    memset(h, 0, sizeof(*h));
    atomic_init(&h->state[0], 0);
    atomic_init(&h->state[1], 0);
    h->slot[0] = slot0;
    h->slot[1] = slot1;
    h->size = size;
    h->open = -1;
}

void *ecm_api_ho_begin(ecm_api_ho_t *h)
{
    int k = h->next_put;
    if (atomic_load_explicit(&h->state[k], memory_order_acquire) != 0) {
        k ^= 1;
        if (atomic_load_explicit(&h->state[k], memory_order_acquire) != 0) {
            h->drops++;
            h->open = -1;
            return NULL;
        }
    }
    h->open = k;
    return h->slot[k];
}

void ecm_api_ho_commit(ecm_api_ho_t *h)
{
    int k = h->open;
    if (k < 0) return;
    h->put_seq[k] = ++h->puts;
#ifdef ECM_API_HO_BROKEN
    /* negative control (test_api_tsan): publish without release */
    atomic_store_explicit(&h->state[k], 1, memory_order_relaxed);
#else
    atomic_store_explicit(&h->state[k], 1, memory_order_release);
#endif
    h->next_put = k ^ 1;
    h->open = -1;
}

int ecm_api_ho_put(ecm_api_ho_t *h, const void *src)
{
    void *p = ecm_api_ho_begin(h);
    if (!p) return 0;
    memcpy(p, src, h->size);
    ecm_api_ho_commit(h);
    return 1;
}

int ecm_api_ho_get(ecm_api_ho_t *h, void *dst)
{
    int f0 = atomic_load_explicit(&h->state[0], memory_order_acquire) == 1;
    int f1 = atomic_load_explicit(&h->state[1], memory_order_acquire) == 1;
    if (!f0 && !f1) return 0;
    /* the two loads are not one snapshot (Phase 7.6, ecm_diag_handoff_get):
     * when only one slot looked full, look at the other again and take the
     * older of the two if it has become full meanwhile */
    if (f0 != f1) {
        int o = f0 ? 1 : 0;
        if (atomic_load_explicit(&h->state[o], memory_order_acquire) == 1) f0 = f1 = 1;
    }
    int k = (f0 && f1) ? (h->put_seq[0] < h->put_seq[1] ? 0 : 1) : (f0 ? 0 : 1);
    memcpy(dst, h->slot[k], h->size);
    atomic_store_explicit(&h->state[k], 0, memory_order_release);
    return 1;
}

/* ---------------------------------------------------------- histograms --- */
void ecm_api_hist_from(ecm_api_hist_t *o, const ecm_hist_t *h)
{
    o->have = 1;
    o->count = h->count;
    o->sum_ns = h->sum_ns;
    o->min_ns = h->count ? h->min_ns : 0;
    o->max_ns = h->max_ns;
    memcpy(o->b, h->buckets, sizeof(o->b));
}

/* ---------------------------------------------------------------- keys --- */
const char *ecm_api_bus_key(int s)
{
    static const char *const k[] = { "RUN", "DEGRADED", "LOST", "RECOVER" };
    _Static_assert(sizeof(k) / sizeof(k[0]) == ECM_BUS_NSTATE, "bus state keys");
    return s >= 0 && s < ECM_BUS_NSTATE ? k[s] : "UNKNOWN";
}

const char *ecm_api_dc_key(int s)
{
    switch (s) {
    case ECM_DC_UNANCHORED: return "UNANCHORED";
    case ECM_DC_ACQUIRE:    return "ACQUIRE";
    case ECM_DC_LOCKED:     return "LOCKED";
    }
    return "UNKNOWN";
}

const char *ecm_api_sact_key(int a)
{
    switch (a) {
    case ECM_SACT_NONE:     return "none";
    case ECM_SACT_ACK:      return "ack";
    case ECM_SACT_OP:       return "op";
    case ECM_SACT_RECONFIG: return "reconfig";
    }
    return "unknown";
}

#define FIND_KEYS_N (ECM_FIND_EMCY_LOST + 1)
static const char *const FIND_KEYS[] = {
    "chain_broken", "missing_link_up", "no_address", "cable_fwd", "cable_ret", "fwd_no_origin",
    "pu_only", "pdi", "lost_link", "state", "saturated", "diag_lost", "emcy", "emcy_lost",
};
_Static_assert(sizeof(FIND_KEYS) / sizeof(FIND_KEYS[0]) == FIND_KEYS_N,
               "ecm_find_type_t grew: add its key here and in docs/api.md");

const char *ecm_api_finding_key(int t)
{
    return t >= 0 && t < FIND_KEYS_N ? FIND_KEYS[t] : "unknown";
}

const char *ecm_api_sev_key(int s)
{
    return s == ECM_SEV_INFO ? "info" : s == ECM_SEV_WARN ? "warn" : s == ECM_SEV_ERROR ? "error" : "unknown";
}

const char *ecm_api_al_key(uint16_t al)
{
    switch (al & 0x0F) {
    case 1: return "INIT";
    case 2: return "PREOP";
    case 3: return "BOOT";
    case 4: return "SAFEOP";
    case 8: return "OP";
    }
    return "UNKNOWN";
}

const char *ecm_api_recover_key(const ecm_api_mon_slave_t *s)
{
    if (s->failed_config) return "failed_config";
    if (s->failed) return "failed";
    if (s->unhealthy) return "retrying";
    return "ok";
}

/* ---------------------------------------------------------------- JSON --- */
typedef struct {
    char  *b;
    size_t len, cap;
    int    ovf;
    int    depth;
    uint8_t comma[24];          /* a value was written at this depth             */
} jw_t;

static void jraw(jw_t *j, const char *s, size_t n)
{
    if (j->ovf) return;
    if (j->len + n + 1 > j->cap) { j->ovf = 1; return; }
    memcpy(j->b + j->len, s, n);
    j->len += n;
    j->b[j->len] = '\0';
}

static void jputs(jw_t *j, const char *s) { jraw(j, s, strlen(s)); }

static void jprintf(jw_t *j, const char *fmt, ...)
{
    if (j->ovf) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(j->b + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r + 1 > j->cap - j->len) { j->ovf = 1; return; }
    j->len += (size_t)r;
}

static void jstr(jw_t *j, const char *s)
{
    jraw(j, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; jraw(j, e, 2); }
        else if (*p < 0x20) jprintf(j, "\\u%04x", *p);
        else jraw(j, (const char *)p, 1);
    }
    jraw(j, "\"", 1);
}

/* a new value at the current depth: comma before every one but the first */
static void jsep(jw_t *j)
{
    if (j->comma[j->depth]) jraw(j, j->depth == 1 ? ",\n" : ",", j->depth == 1 ? 2 : 1);   /* one top-level key per line */
    j->comma[j->depth] = 1;
}

static void jkey(jw_t *j, const char *k) { jsep(j); jstr(j, k); jraw(j, ":", 1); }

static void jopen(jw_t *j, char c)
{
    char s[2] = { c, 0 };
    jputs(j, s);
    if (j->depth + 1 < (int)sizeof(j->comma)) j->depth++;
    j->comma[j->depth] = 0;
}

static void jclose(jw_t *j, char c)
{
    char s[2] = { c, 0 };
    if (j->depth > 0) j->depth--;
    jputs(j, s);
}

static void kobj(jw_t *j, const char *k) { jkey(j, k); jopen(j, '{'); }
static void karr(jw_t *j, const char *k) { jkey(j, k); jopen(j, '['); }
static void aobj(jw_t *j) { jsep(j); jopen(j, '{'); }
static void ku64(jw_t *j, const char *k, uint64_t v) { jkey(j, k); jprintf(j, "%" PRIu64, v); }
static void ki64(jw_t *j, const char *k, int64_t v) { jkey(j, k); jprintf(j, "%" PRId64, v); }
static void kstr(jw_t *j, const char *k, const char *v) { jkey(j, k); jstr(j, v); }
static void kbool(jw_t *j, const char *k, int v) { jkey(j, k); jputs(j, v ? "true" : "false"); }
static void knull(jw_t *j, const char *k) { jkey(j, k); jputs(j, "null"); }
static void khex(jw_t *j, const char *k, unsigned v, int digits) { jkey(j, k); jprintf(j, "\"0x%0*X\"", digits, v); }
static void kopt(jw_t *j, const char *k, int64_t v) { if (v < 0) knull(j, k); else ki64(j, k, v); }

static void kcounts(jw_t *j, const char *k, const uint64_t *b, int n)
{
    int last = n - 1;
    while (last >= 0 && b[last] == 0) last--;            /* trailing zeros left out */
    karr(j, k);
    for (int i = 0; i <= last; i++) { jsep(j); jprintf(j, "%" PRIu64, b[i]); }
    jclose(j, ']');
}

static void ku64x4(jw_t *j, const char *k, const uint64_t *v)
{
    karr(j, k);
    for (int i = 0; i < 4; i++) { jsep(j); jprintf(j, "%" PRIu64, v[i]); }
    jclose(j, ']');
}

static void khist(jw_t *j, const char *k, const ecm_api_hist_t *h)
{
    if (!h->have) { knull(j, k); return; }
    kobj(j, k);
    kstr(j, "unit", "ns");
    kstr(j, "buckets", "log20_100ns_v1");
    ku64(j, "count", h->count);
    ku64(j, "sum", h->sum_ns);
    ki64(j, "min", h->min_ns);
    ki64(j, "max", h->max_ns);
    kcounts(j, "counts", h->b, ECM_API_HIST_BINS);
    jclose(j, '}');
}

static void bucket_table(jw_t *j)
{
    static ecm_hist_t ref;                 /* same bounds as every ecm_hist_t */
    static int init;
    if (!init) { hist_init(&ref); init = 1; }
    kobj(j, "buckets");
    kobj(j, "log20_100ns_v1");
    kstr(j, "unit", "ns");
    karr(j, "upper");
    for (int i = 0; i < HIST_NUM_BUCKETS; i++) { jsep(j); jprintf(j, "%" PRIu64, ref.bucket_upper_ns[i]); }
    jclose(j, ']');
    ku64(j, "n", ECM_API_HIST_BINS);                   /* index n-1: above the last upper */
    jclose(j, '}');
    kobj(j, "lin1us_v1");
    kstr(j, "unit", "us");
    ku64(j, "width", 1);
    ku64(j, "n", ECM_API_DC_BINS);                     /* index n-1: n-1 us and more      */
    jclose(j, '}');
    jclose(j, '}');
}

static void emcy_obj(jw_t *j, const ecm_api_emcy_t *e)
{
    aobj(j);
    ku64(j, "mono_ns", e->mono_ns);
    ku64(j, "tick", e->tick);
    khex(j, "code", e->code, 4);
    khex(j, "reg", e->reg, 2);
    jkey(j, "data");
    jprintf(j, "\"%02X%02X%02X%02X%02X\"", e->data[0], e->data[1], e->data[2], e->data[3], e->data[4]);
    kstr(j, "class", ecm_emcy_class_name(e->code));
    jclose(j, '}');
}

size_t ecm_api_stats_json(const ecm_api_stats_t *st, char *buf, size_t cap)
{
    jw_t jj = { .b = buf, .cap = cap }, *j = &jj;
    if (!cap) return 0;
    buf[0] = '\0';
    const ecm_api_rt_t *rt = st->have_rt ? &st->rt : NULL;

    jopen(j, '{');
    ku64(j, "schema", ECM_API_SCHEMA);
    ku64(j, "pub_seq", st->pub_seq);
    ku64(j, "mono_ns", st->mono_ns);
    ki64(j, "pid", st->pid);
    kbool(j, "final", st->final);
    bucket_table(j);

    /* ---- bus ---- */
    kobj(j, "bus");
    kstr(j, "fsm", rt ? st->fsm : "STARTING");          /* no RT data yet: not RUN */
    ki64(j, "cycle_ns", st->cycle_ns);
    kstr(j, "link", st->link);
    ki64(j, "n_slaves", st->n_slaves);
    karr(j, "ports");
    for (int p = 0; p < st->nports && p < ECM_API_MAX_PORTS; p++) {
        const ecm_api_port_t *po = &st->ports[p];
        uint64_t tx = po->tx, nf = po->noframe;
        if (st->nports == 1 && rt) {               /* one port: every cyclic frame went through it */
            tx = nf = 0;
            for (int g = 0; g < rt->ngroups && g < ECM_API_MAX_GROUPS; g++) {
                tx += rt->g[g].cycles;
                nf += rt->g[g].wkc[ECM_WKC_NOFRAME];
            }
        }
        aobj(j);
        kstr(j, "name", po->name);
        kstr(j, "iface", po->iface);
        kstr(j, "link", po->link > 0 ? "up" : po->link == 0 ? "down" : "unknown");
        if (rt) { ku64(j, "tx", tx); ku64(j, "noframe", nf); }
        kopt(j, "crc_err", po->crc_err);
        kopt(j, "rx_missed", po->rx_missed);
        kopt(j, "carrier_changes", po->carrier_changes);
        jclose(j, '}');
    }
    jclose(j, ']');
    kstr(j, "path", st->path);
    jclose(j, '}');

    /* ---- groups ---- */
    karr(j, "groups");
    for (int g = 0; g < st->ngroups && g < ECM_API_MAX_GROUPS; g++) {
        const ecm_api_group_cfg_t *gc = &st->gcfg[g];
        aobj(j);
        kstr(j, "name", gc->name);
        karr(j, "slaves");
        for (int i = 0; i < gc->nslaves; i++) { jsep(j); jprintf(j, "%u", gc->slaves[i]); }
        jclose(j, ']');
        ki64(j, "cycle_ns", gc->cycle_ns);
        if (rt && g < rt->ngroups) {
            const ecm_api_rt_group_t *r = &rt->g[g];
            ku64(j, "cycles", r->cycles);
            ku64(j, "ok", r->wkc[ECM_WKC_OK]);
            ku64(j, "noframe", r->wkc[ECM_WKC_NOFRAME]);
            ku64(j, "zero", r->wkc[ECM_WKC_ZERO]);
            ku64(j, "partial", r->wkc[ECM_WKC_PARTIAL]);
            ku64(j, "over", r->wkc[ECM_WKC_OVER]);
            ku64(j, "max_run_bad", r->max_run_bad);
            ku64(j, "overrun", r->overrun);
        }
        jclose(j, '}');
    }
    jclose(j, ']');

    /* ---- hist ---- */
    kobj(j, "hist");
    static const ecm_api_hist_t none;
    const ecm_api_tel_t *t = st->have_tel ? &st->tel : NULL;
    khist(j, "wake_jitter", t ? &t->wake_jitter : &none);
    khist(j, "prep_send",   t ? &t->prep_send   : &none);
    khist(j, "turnaround",  t ? &t->turnaround  : &none);
    khist(j, "occupancy",   t ? &t->occupancy   : &none);
    if (st->have_dc && st->dc.h.have) {
        kobj(j, "dc_err");
        kstr(j, "unit", "us");
        kstr(j, "buckets", "lin1us_v1");
        ku64(j, "count", st->dc.h.n);
        kcounts(j, "counts", st->dc.h.b, ECM_API_DC_BINS);
        jclose(j, '}');
    } else {
        knull(j, "dc_err");
    }
    khist(j, "hook", st->have_hook ? &st->hook_h.h : &none);
    jclose(j, '}');

    /* ---- dc ---- */
    if (rt && rt->dc_on) {
        kobj(j, "dc");
        kstr(j, "state", ecm_api_dc_key(rt->dc_state));
        ki64(j, "ref_pos", st->dc_ref_pos);
        ku64(j, "locks", rt->dc_locks);
        ku64(j, "unlocks", rt->dc_unlocks);
        ku64(j, "clamps", rt->dc_clamps);
        ku64(j, "wraps", rt->dc_wraps);
        ku64(j, "stale", rt->dc_stale);
        ki64(j, "err_ns", rt->dc_err_ns);
        jclose(j, '}');
    } else {
        knull(j, "dc");
    }

    /* ---- link ---- */
    kobj(j, "link");
    if (rt && rt->txtime_have) {
        ku64(j, "txtime_late", rt->txtime_late);
        ku64(j, "txtime_bypass", rt->txtime_bypass);
        ku64(j, "send_err", rt->send_err);
        ku64(j, "tai_steps", rt->tai_steps);
    }
    ku64(j, "etf_missed", st->etf_missed);
    ku64(j, "etf_invalid", st->etf_invalid);
    jclose(j, '}');

    /* ---- slaves ---- */
    karr(j, "slaves");
    const ecm_api_mon_t *m = st->have_mon ? &st->mon : NULL;
    for (int i = 0; i < st->n_slaves && i < ECM_API_MAX_SLAVES; i++) {
        const ecm_api_slave_cfg_t *sc = &st->scfg[i];
        aobj(j);
        ki64(j, "pos", sc->pos);
        khex(j, "vendor", sc->vendor, 8);
        khex(j, "product", sc->product, 8);
        khex(j, "revision", sc->revision, 8);
        kstr(j, "name", sc->name);
        if (m && i < m->n) {
            const ecm_api_mon_slave_t *s = &m->s[i];
            if (m->diag_on && s->ever_answered) {
                khex(j, "station", s->station, 4);
                kstr(j, "al", ecm_api_al_key(s->al_status));
                kbool(j, "al_err", (s->al_status & 0x10) != 0);
                khex(j, "al_code", s->al_code, 4);
                khex(j, "dl_status", s->dl_status, 4);
            }
            if (m->diag_on) {
                kbool(j, "answered", s->answered);
                ku64x4(j, "inv", s->inv);
                ku64x4(j, "rx_err", s->rx);
                ku64x4(j, "fwd_err", s->fwd);
                ku64x4(j, "lost_link", s->lost);
                ku64(j, "pu_err", s->pu);
                ku64(j, "pdi_err", s->pdi);
            }
            kstr(j, "policy", ecm_api_sact_key(s->sact));
            kstr(j, "recover", ecm_api_recover_key(s));
            ku64(j, "attempts", s->attempts);
            ku64(j, "recoveries", s->recoveries);
            ku64(j, "initcmd_failed", s->initcmd_failed);
            ku64(j, "emcy_count", s->emcy_count);
            karr(j, "emcy");
            for (int k = 0; k < s->nemcy && k < ECM_API_EMCY_KEEP; k++) emcy_obj(j, &s->emcy[k]);
            jclose(j, ']');
        }
        jclose(j, '}');
    }
    jclose(j, ']');

    /* ---- axes ---- */
    karr(j, "axes");
    for (int k = 0; k < st->naxes && k < ECM_API_MAX_AXES; k++) {
        const ecm_api_axis_t *a = &st->ax[k];
        aobj(j);
        kstr(j, "name", a->name);
        ki64(j, "slave", a->slave);
        ki64(j, "n", a->n);
        if (a->valid) {
            kstr(j, "state", a->state);
            khex(j, "sw", a->sw, 4);
            khex(j, "cw", a->cw, 4);
            ki64(j, "mode", a->mode);
            ki64(j, "mode_display", a->mode_display);
            kstr(j, "err", a->err);
            khex(j, "error_code", a->error_code, 4);
            kstr(j, "error_src", a->error_src);
            kstr(j, "error_text", a->error_text);
            ki64(j, "pos", a->pos);
            ki64(j, "vel", a->vel);
            ku64(j, "tick", a->tick);
            ku64(j, "used", a->used);
            ku64(j, "late", a->late);
            ku64(j, "underruns", a->underruns);
            ku64(j, "dropped", a->dropped);
            ku64(j, "step_refused", a->step_refused);
            ku64(j, "pp_acked", a->pp_acked);
            kbool(j, "homed", a->homed);
        }
        jclose(j, '}');
    }
    jclose(j, ']');

    /* ---- rt ---- */
    kobj(j, "rt");
    kbool(j, "dma_latency_held", st->dma_latency_held);
    if (st->hook[0]) kstr(j, "hook", st->hook); else knull(j, "hook");
    if (rt) ku64(j, "hook_calls", rt->hook_calls);
    kstr(j, "api_cpus", st->api_cpus);
    jclose(j, '}');

    /* ---- findings ---- */
    karr(j, "findings");
    for (int k = 0; m && k < m->nf && k < ECM_API_MAX_FINDINGS; k++) {
        const ecm_api_finding_t *f = &m->f[k];
        aobj(j);
        kstr(j, "type", f->type);
        kstr(j, "sev", f->sev);
        kstr(j, "text", f->text);
        ki64(j, "a", f->a);
        ki64(j, "b", f->b);
        ku64(j, "c", f->c);
        ku64(j, "count", f->count);
        kbool(j, "active", f->active);
        jclose(j, '}');
    }
    jclose(j, ']');

    /* ---- sources: where each part came from, for staleness per part ---- */
    kobj(j, "sources");
    if (rt) { kobj(j, "rt"); ku64(j, "seq", rt->seq); ku64(j, "mono_ns", rt->mono_ns); ku64(j, "tick", rt->tick); jclose(j, '}'); }
    else knull(j, "rt");
    if (t) { kobj(j, "telemetry"); ku64(j, "seq", t->seq); ku64(j, "mono_ns", t->mono_ns); ku64(j, "ring_drops", t->ring_drops); jclose(j, '}'); }
    else knull(j, "telemetry");
    if (m) {
        kobj(j, "monitor");
        ku64(j, "seq", m->seq); ku64(j, "mono_ns", m->mono_ns);
        kbool(j, "diag", m->diag_on);
        ku64(j, "diag_reads", m->diag_reads); ku64(j, "diag_frames_lost", m->diag_frames_lost);
        ku64(j, "emcy_total", m->emcy_total); ku64(j, "emcy_lost", m->emcy_lost);
        jclose(j, '}');
    } else knull(j, "monitor");
    ku64(j, "handoff_drops", (rt ? rt->ho_drops : 0) + (t ? t->ho_drops : 0) + (m ? m->ho_drops : 0));
    jclose(j, '}');

    jclose(j, '}');
    jputs(j, "\n");
    if (j->ovf) { buf[0] = '\0'; return 0; }
    return j->len;
}

/* ------------------------------------------------------------- publish --- */
int ecm_api_publish(const char *dir, const char *name, const char *buf, size_t len)
{
    char tmp[512], dst[512];
    if (snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, name) >= (int)sizeof(tmp)) return -ENAMETOOLONG;
    if (snprintf(dst, sizeof(dst), "%s/%s", dir, name) >= (int)sizeof(dst)) return -ENAMETOOLONG;
#ifdef ECM_API_PUBLISH_INPLACE
    /* negative control (test_api_offline): write the file in place, no rename */
    (void)tmp;
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#else
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
    if (fd < 0) return -errno;
    size_t off = 0;
    while (off < len) {
#ifdef ECM_API_PUBLISH_INPLACE
        size_t chunk = len - off > 512 ? 512 : len - off;   /* make the half file visible */
#else
        size_t chunk = len - off;
#endif
        ssize_t w = write(fd, buf + off, chunk);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            unlink(tmp);
            return -e;
        }
        off += (size_t)w;
#ifdef ECM_API_PUBLISH_INPLACE
        usleep(50);
#endif
    }
    if (close(fd) != 0) { int e = errno; unlink(tmp); return -e; }
#ifndef ECM_API_PUBLISH_INPLACE
    if (rename(tmp, dst) != 0) { int e = errno; unlink(tmp); return -e; }
#endif
    return 0;
}

int ecm_api_mkdir(const char *dir, unsigned mode)
{
    if (mkdir(dir, (mode_t)mode) == 0) return 0;
    if (errno != EEXIST) return -errno;
    struct stat sb;
    if (stat(dir, &sb) != 0) return -errno;
    return S_ISDIR(sb.st_mode) ? 0 : -ENOTDIR;
}

/* ---------------------------------------------------------------- sysfs --- */
static int64_t read_i64(const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    long long v;
    int ok = fscanf(f, "%lld", &v) == 1;
    fclose(f);
    return ok && v >= 0 ? (int64_t)v : -1;
}

void ecm_api_nic_read(const char *sysroot, const char *ifname, ecm_api_port_t *p)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/class/net/%s/statistics/rx_crc_errors", sysroot, ifname);
    p->crc_err = read_i64(path);
    snprintf(path, sizeof(path), "%s/class/net/%s/statistics/rx_missed_errors", sysroot, ifname);
    p->rx_missed = read_i64(path);
    snprintf(path, sizeof(path), "%s/class/net/%s/carrier_changes", sysroot, ifname);
    p->carrier_changes = read_i64(path);
    snprintf(path, sizeof(path), "%s/class/net/%s/operstate", sysroot, ifname);
    p->link = -1;
    FILE *f = fopen(path, "re");
    if (f) {
        char s[32] = "";
        if (fgets(s, sizeof(s), f)) {
            if (!strncmp(s, "up", 2)) p->link = 1;
            else if (!strncmp(s, "down", 4) || !strncmp(s, "lowerlayerdown", 14)) p->link = 0;
        }
        fclose(f);
    }
}

/* ----------------------------------------------------------------- cpus --- */
int ecm_api_parse_cpus(const char *s, uint64_t *mask)
{
    uint64_t m = 0;
    const char *p = s;
    if (!s || !*s) return -1;
    while (*p) {
        char *e;
        long a = strtol(p, &e, 10);
        if (e == p || a < 0 || a > 63) return -1;
        long b = a;
        p = e;
        if (*p == '-') {
            p++;
            b = strtol(p, &e, 10);
            if (e == p || b < a || b > 63) return -1;
            p = e;
        }
        for (long c = a; c <= b; c++) m |= 1ull << c;
        if (*p == ',') { p++; if (!*p) return -1; }
        else if (*p) return -1;
    }
    if (!m) return -1;
    *mask = m;
    return 0;
}
