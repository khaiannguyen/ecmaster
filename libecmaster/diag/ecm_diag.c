/* ==========================================================================
 * ecm_diag.c — bus diagnostics model (Phase 7.2). See ecm_diag.h.
 *
 * Localisation rules (datasheet Section I §14):
 *  - An ESC that detects a bad frame first counts it in invalid frame /
 *    RX error of the RECEIVING port and marks the frame; every ESC after it
 *    only counts "forwarded error". So the origin of a fault is the port
 *    with invalid/RX errors, not the ports with forwarded errors.
 *  - Port 0 receives the frame on its way out from the master (forward
 *    path): invalid/RX at slave k port 0 = fault on the link INTO slave k,
 *    i.e. between slave k-1 (or the master) and slave k.
 *  - Port 1 receives the frame on its way back (return path): invalid/RX at
 *    slave k port 1 = fault on the link between slave k and slave k+1, in
 *    the return direction.
 *  - Chain topology: BRD WKC = number of slaves the frame reached. If fewer
 *    than configured, the last one answering (slave m) closed its port-1
 *    loop: link down on port 1 = the chain ends after slave m. Link still
 *    up on port 1 = slave m+1 is physically there but not answering.
 *  - A slave counted by BRD that does not answer its configured station
 *    address lost its configuration (typically a power cycle: the ESC
 *    comes back with station address 0).
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ecm_diag.h"

#define DL_LINK_PORT(p)   (1u << (4 + (p)))     /* 0x0110 bits 4..7 */
#define AL_ERR            0x10u

/* ---------------------------------------------------------------- WKC --- */
ecm_wkc_class_t ecm_wkc_classify(int wkc, int expected)
{
    if (wkc == expected) return ECM_WKC_OK;
    if (wkc < 0)         return ECM_WKC_NOFRAME;   /* EC_NOFRAME / EC_TIMEOUT */
    if (wkc == 0)        return ECM_WKC_ZERO;
    if (wkc < expected)  return ECM_WKC_PARTIAL;
    return ECM_WKC_OVER;
}

void ecm_wkc_account(ecm_wkc_stats_t *st, int wkc, int expected)
{
    ecm_wkc_class_t c = ecm_wkc_classify(wkc, expected);
    st->count[c]++;
    if (c == ECM_WKC_PARTIAL) st->last_missing = expected - wkc;
    if (c == ECM_WKC_OK) {
        st->run_bad = 0;
    } else if (++st->run_bad > st->max_run_bad) {
        st->max_run_bad = st->run_bad;
    }
}

const char *ecm_wkc_class_name(ecm_wkc_class_t c)
{
    static const char *const n[ECM_WKC_NCLASS] = { "OK", "NOFRAME", "ZERO", "PARTIAL", "OVER" };
    return (c >= 0 && c < ECM_WKC_NCLASS) ? n[c] : "?";
}

/* ------------------------------------------------------------ handoff --- */
void ecm_diag_handoff_init(ecm_diag_handoff_t *h)
{
    memset(h, 0, sizeof(*h));
    atomic_init(&h->state[0], 0);
    atomic_init(&h->state[1], 0);
}

int ecm_diag_handoff_put(ecm_diag_handoff_t *h, const ecm_diag_raw_t *raw)
{
    int k = h->next_put;
    if (atomic_load_explicit(&h->state[k], memory_order_acquire) != 0) {
        k ^= 1;
        if (atomic_load_explicit(&h->state[k], memory_order_acquire) != 0) {
            h->drops++;
            return 0;
        }
    }
    h->slot[k] = *raw;
    h->put_seq[k] = ++h->puts;
    atomic_store_explicit(&h->state[k], 1, memory_order_release);
    h->next_put = k ^ 1;
    return 1;
}

int ecm_diag_handoff_get(ecm_diag_handoff_t *h, ecm_diag_raw_t *out)
{
    int f0 = atomic_load_explicit(&h->state[0], memory_order_acquire) == 1;
    int f1 = atomic_load_explicit(&h->state[1], memory_order_acquire) == 1;
    if (!f0 && !f1) return 0;
    /* Giai doan 7.6 (found by the stress test, not by TSan -- it is not a
     * data race): the two loads above are not one snapshot. If slot 0 was
     * seen empty, the producer may have filled it (epoch N) and then slot 1
     * (N+1) before slot 1 was loaded; taking slot 1 now hands out N+1
     * before N. So when only one slot looked full, look at the other one
     * again: if it has become full, take the older of the two. */
    if (f0 != f1) {
        int o = f0 ? 1 : 0;
        if (atomic_load_explicit(&h->state[o], memory_order_acquire) == 1) f0 = f1 = 1;
    }
    int k = (f0 && f1) ? (h->put_seq[0] < h->put_seq[1] ? 0 : 1) : (f0 ? 0 : 1);
    *out = h->slot[k];
    atomic_store_explicit(&h->state[k], 0, memory_order_release);
    return 1;
}

/* -------------------------------------------------------------- model --- */
void ecm_diag_init(ecm_diag_t *d, int n, uint16_t expected_state)
{
    memset(d, 0, sizeof(*d));
    d->n = n > ECM_DIAG_MAX_SLAVES ? ECM_DIAG_MAX_SLAVES : n;
    d->expected_state = expected_state;
    d->brd_count = -1;
}

static void split(const uint8_t *e, ecm_diag_counts_t *c)
{
    memset(c, 0, sizeof(*c));
    for (int p = 0; p < ECM_DIAG_PORTS; p++) {
        c->inv[p]  = e[2 * p];
        c->rx[p]   = e[2 * p + 1];
        c->fwd[p]  = e[8 + p];
        c->lost[p] = e[16 + p];
    }
    c->pu  = e[12];
    c->pdi = e[13];                /* 0x030E:0x030F = PDI error code, skipped */
}

static void add(ecm_diag_counts_t *t, const ecm_diag_counts_t *d)
{
    for (int p = 0; p < ECM_DIAG_PORTS; p++) {
        t->inv[p] += d->inv[p]; t->rx[p] += d->rx[p];
        t->fwd[p] += d->fwd[p]; t->lost[p] += d->lost[p];
    }
    t->pu += d->pu; t->pdi += d->pdi;
}

/* Byte offsets inside the 20-byte block that are counters (not the PDI
 * error code at 0x030E:0x030F, not reserved bytes). */
static int is_counter(int j) { return j != 14 && j != 15; }

void ecm_diag_ingest(ecm_diag_t *d, const ecm_diag_raw_t *raw)
{
    d->reads++;
    d->last_seq = raw->seq;
    d->last_t_ns = raw->t_ns;
    d->frames_lost = raw->frames_lost;
    d->handoff_drops = raw->handoff_drops;
    d->ngroups = raw->ngroups > ECM_DIAG_MAX_GROUPS ? ECM_DIAG_MAX_GROUPS : raw->ngroups;
    memcpy(d->wkc, raw->wkc, sizeof(d->wkc));
    if (!raw->frame_ok) return;

    d->brd_count = raw->brd_count;
    d->brd_al_or = raw->brd_al_or;

    int end = raw->first + raw->count;
    if (end > d->n) end = d->n;
    for (int i = raw->first; i < end; i++) {
        ecm_diag_slave_t *s = &d->s[i];
        const ecm_diag_raw_slave_t *r = &raw->s[i];
        s->seen_read = d->reads;
        s->station_addr = r->station_addr;
        memset(&s->last, 0, sizeof(s->last));
        s->answered = (r->wkc_err > 0 || r->wkc_al > 0);
        if (!s->answered) { s->no_answer++; continue; }
        s->ever_answered = 1;

        if (r->wkc_err > 0) {
            uint8_t delta[ECM_DIAG_ERR_LEN];
            int sat = 0;
            for (int j = 0; j < ECM_DIAG_ERR_LEN; j++) {
                uint8_t v = r->err[j];
                if (!is_counter(j)) { delta[j] = 0; continue; }
                if (v == 0xFF) sat = 1;
                if (raw->clear_on_read || !s->have_prev) delta[j] = v;
                else delta[j] = (v >= s->prev[j]) ? (uint8_t)(v - s->prev[j])
                                                  : v;   /* cleared / power cycle */
            }
            memcpy(s->prev, r->err, ECM_DIAG_ERR_LEN);
            s->have_prev = !raw->clear_on_read;
            if (raw->clear_on_read) memset(s->prev, 0, sizeof(s->prev));
            if (sat) s->saturated++;
            split(delta, &s->last);
            add(&s->tot, &s->last);
        }
        if (r->wkc_dl > 0) s->dl_status = r->dl_status;
        if (r->wkc_al > 0) { s->al_status = r->al_status; s->al_code = r->al_code; }
    }
}

/* ----------------------------------------------------------- analysis --- */
static int push(ecm_diag_finding_t *out, int *nf, int max, ecm_find_type_t t, ecm_sev_t sev,
                int a, int b, uint32_t c, uint64_t count, int active)
{
    if (*nf >= max) return 0;
    out[(*nf)++] = (ecm_diag_finding_t){ .type = t, .sev = sev, .a = a, .b = b, .c = c,
                                         .count = count, .active = active };
    return 1;
}

int ecm_diag_analyze(const ecm_diag_t *d, ecm_diag_finding_t *out, int max)
{
    int nf = 0, n = d->n;

    /* 1. Topology: how far does the frame get? */
    if (d->brd_count >= 0 && d->brd_count < n) {
        int m = d->brd_count;                       /* last answering slave, 1-based */
        if (m == 0) {
            push(out, &nf, max, ECM_FIND_CHAIN_BROKEN, ECM_SEV_ERROR, 0, n, 0, 0, 1);
        } else {
            const ecm_diag_slave_t *s = &d->s[m - 1];
            int known = s->answered && s->seen_read == d->reads;
            if (known && (s->dl_status & DL_LINK_PORT(1)))
                push(out, &nf, max, ECM_FIND_MISSING_LINK_UP, ECM_SEV_ERROR, m, n, 0, 0, 1);
            else
                push(out, &nf, max, ECM_FIND_CHAIN_BROKEN, ECM_SEV_ERROR, m, n, 0, 0, 1);
        }
    }
    /* 2. On the bus but not answering its address. */
    int on_bus = d->brd_count < 0 ? n : (d->brd_count < n ? d->brd_count : n);
    for (int i = 0; i < on_bus; i++) {
        const ecm_diag_slave_t *s = &d->s[i];
        if (s->seen_read == d->reads && !s->answered)
            push(out, &nf, max, ECM_FIND_NO_ADDRESS, ECM_SEV_ERROR, i + 1, 0, s->station_addr,
                 s->no_answer, 1);
    }

    /* 3. Frame errors: origins first. */
    int any_origin = 0;
    for (int i = 0; i < n; i++) {
        const ecm_diag_slave_t *s = &d->s[i];
        uint64_t f = s->tot.inv[0] > s->tot.rx[0] ? s->tot.inv[0] : s->tot.rx[0];
        if (f) {
            int act = (s->last.inv[0] + s->last.rx[0]) > 0;
            push(out, &nf, max, ECM_FIND_CABLE_FWD, act ? ECM_SEV_ERROR : ECM_SEV_WARN,
                 i + 1, 0, 0, f, act);
            any_origin = 1;
        }
        for (int p = 1; p < ECM_DIAG_PORTS; p++) {
            uint64_t g = s->tot.inv[p] > s->tot.rx[p] ? s->tot.inv[p] : s->tot.rx[p];
            if (!g) continue;
            int act = (s->last.inv[p] + s->last.rx[p]) > 0;
            push(out, &nf, max, ECM_FIND_CABLE_RET, act ? ECM_SEV_ERROR : ECM_SEV_WARN,
                 i + 1, p, 0, g, act);
            any_origin = 1;
        }
    }
    /* Forwarded errors on port 0 of slave j need an origin on port 0 of
     * some slave <= j; forwarded errors anywhere need at least one origin. */
    {
        int fwd_first = 0; uint64_t fwd_sum = 0;
        int origin_so_far = 0;
        for (int i = 0; i < n; i++) {
            const ecm_diag_slave_t *s = &d->s[i];
            if (s->tot.inv[0] || s->tot.rx[0]) origin_so_far = 1;
            uint64_t fw = 0;
            for (int p = 0; p < ECM_DIAG_PORTS; p++) fw += s->tot.fwd[p];
            int unexplained = (s->tot.fwd[0] && !origin_so_far) || (fw && !any_origin);
            if (unexplained) { if (!fwd_first) fwd_first = i + 1; fwd_sum += fw; }
        }
        if (fwd_first)
            push(out, &nf, max, ECM_FIND_FWD_NO_ORIGIN, ECM_SEV_WARN, fwd_first, 0, 0, fwd_sum, 0);
    }

    for (int i = 0; i < n; i++) {
        const ecm_diag_slave_t *s = &d->s[i];
        int port_err = 0;
        for (int p = 0; p < ECM_DIAG_PORTS; p++)
            port_err |= (s->tot.inv[p] || s->tot.rx[p] || s->tot.fwd[p]);
        if (s->tot.pu && !port_err)
            push(out, &nf, max, ECM_FIND_PU_ONLY, ECM_SEV_WARN, i + 1, 0, 0, s->tot.pu, s->last.pu > 0);
        if (s->tot.pdi)
            push(out, &nf, max, ECM_FIND_PDI, s->last.pdi ? ECM_SEV_ERROR : ECM_SEV_WARN,
                 i + 1, 0, 0, s->tot.pdi, s->last.pdi > 0);
        for (int p = 0; p < ECM_DIAG_PORTS; p++)
            if (s->tot.lost[p])
                push(out, &nf, max, ECM_FIND_LOST_LINK, ECM_SEV_WARN, i + 1, p, 0,
                     s->tot.lost[p], s->last.lost[p] > 0);
        if (s->saturated)
            push(out, &nf, max, ECM_FIND_SATURATED, ECM_SEV_WARN, i + 1, 0, 0, s->saturated, 0);
    }

    /* 4. AL state of every slave that answered this time. */
    for (int i = 0; i < n; i++) {
        const ecm_diag_slave_t *s = &d->s[i];
        if (!s->answered || s->seen_read != d->reads) continue;
        int bad = (s->al_status & AL_ERR) ||
                  (d->expected_state && (s->al_status & 0x0F) != d->expected_state);
        if (bad)
            push(out, &nf, max, ECM_FIND_STATE, ECM_SEV_ERROR, i + 1, s->al_status, s->al_code, 0, 1);
    }

    if (d->frames_lost)
        push(out, &nf, max, ECM_FIND_DIAG_LOST, ECM_SEV_INFO, 0, 0, 0, d->frames_lost, 0);

    /* Most severe first, stable within a severity. */
    for (int i = 1; i < nf; i++) {
        ecm_diag_finding_t x = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].sev < x.sev) { out[j + 1] = out[j]; j--; }
        out[j + 1] = x;
    }
    return nf;
}

/* ------------------------------------------------------------- output --- */
static const char *state_name(uint16_t al)
{
    switch (al & 0x0F) {
    case 1: return "INIT";
    case 2: return "PREOP";
    case 3: return "BOOT";
    case 4: return "SAFEOP";
    case 8: return "OP";
    default: return "?";
    }
}

static const char *sev_name(ecm_sev_t s)
{
    return s == ECM_SEV_ERROR ? "ERROR" : s == ECM_SEV_WARN ? "WARN " : "INFO ";
}

size_t ecm_diag_finding_str(const ecm_diag_finding_t *f, ecm_al_str_fn al_str, char *buf, size_t cap)
{
    unsigned long long c = (unsigned long long)f->count;
    const char *act = f->active ? " [active]" : " [not in last read]";
    int r = 0;
    switch (f->type) {
    case ECM_FIND_CHAIN_BROKEN:
        if (f->a == 0)
            r = snprintf(buf, cap, "no slave answers (BRD WKC 0 of %d): link master -> slave 1 down, "
                         "or slave 1 off", f->b);
        else
            r = snprintf(buf, cap, "chain broken after slave %d: %d of %d slaves answer, slave %d port 1 "
                         "has no link -> check slave %d / cable slave %d - slave %d",
                         f->a, f->a, f->b, f->a, f->a + 1, f->a, f->a + 1);
        break;
    case ECM_FIND_MISSING_LINK_UP:
        r = snprintf(buf, cap, "%d of %d slaves answer, but slave %d port 1 still has link: slave %d "
                     "is connected but its ESC does not process frames", f->a, f->b, f->a, f->a + 1);
        break;
    case ECM_FIND_NO_ADDRESS:
        r = snprintf(buf, cap, "slave %d is on the bus (BRD counts it) but ignores station address "
                     "0x%04X: power-cycled or lost its configuration (%llu reads)", f->a, f->c, c);
        break;
    case ECM_FIND_CABLE_FWD:
        if (f->a == 1)
            r = snprintf(buf, cap, "fault between master and slave 1: %llu frame errors first detected "
                         "at slave 1 port 0%s", c, act);
        else
            r = snprintf(buf, cap, "fault between slave %d and slave %d: %llu frame errors first "
                         "detected at slave %d port 0%s", f->a - 1, f->a, c, f->a, act);
        break;
    case ECM_FIND_CABLE_RET:
        r = snprintf(buf, cap, "fault on slave %d port %d (return path%s): %llu frame errors first "
                     "detected there%s", f->a, f->b, f->b == 1 ? ", link to the next slave" : "", c, act);
        break;
    case ECM_FIND_FWD_NO_ORIGIN:
        r = snprintf(buf, cap, "%llu forwarded errors from slave %d on with no origin in the counters "
                     "read (origin cleared, or at a slave that did not answer)", c, f->a);
        break;
    case ECM_FIND_PU_ONLY:
        r = snprintf(buf, cap, "slave %d: %llu EtherCAT processing unit errors without port errors "
                     "(malformed frames?)%s", f->a, c, act);
        break;
    case ECM_FIND_PDI:
        r = snprintf(buf, cap, "slave %d: %llu PDI errors (interface ESC <-> microcontroller)%s",
                     f->a, c, act);
        break;
    case ECM_FIND_LOST_LINK:
        r = snprintf(buf, cap, "slave %d port %d: link lost %llu time(s)%s", f->a, f->b, c, act);
        break;
    case ECM_FIND_STATE: {
        const char *txt = al_str ? al_str((uint16_t)f->c) : NULL;
        r = snprintf(buf, cap, "slave %d: AL state %s%s, AL status code 0x%04X%s%s%s", f->a,
                     state_name((uint16_t)f->b), (f->b & AL_ERR) ? "+ERR" : "", f->c,
                     txt ? " (" : "", txt ? txt : "", txt ? ")" : "");
        break;
    }
    case ECM_FIND_SATURATED:
        r = snprintf(buf, cap, "slave %d: a counter reached 0xFF within one read period (%llu times): "
                     "counts are lower bounds", f->a, c);
        break;
    case ECM_FIND_DIAG_LOST:
        r = snprintf(buf, cap, "%llu diagnostic frames lost", c);
        break;
    }
    if (r < 0) r = 0;
    return (size_t)r < cap ? (size_t)r : cap - 1;
}

#define APPEND(...) do {                                                   \
        if (len < cap) {                                                    \
            int _r = snprintf(buf + len, cap - len, __VA_ARGS__);           \
            if (_r > 0) { len += (size_t)_r; }                              \
            if (len >= cap) { len = cap - 1; }                              \
        }                                                                   \
    } while (0)

size_t ecm_diag_format(const ecm_diag_t *d, const ecm_diag_finding_t *f, int nf,
                       const char *const *group_names, const char *source,
                       ecm_al_str_fn al_str, char *buf, size_t cap)
{
    size_t len = 0;
    if (!cap) return 0;
    buf[0] = '\0';
    APPEND("# ecm_diag snapshot v1 source=%s seq=%llu t_mono=%.3f reads=%llu\n",
           source ? source : "?", (unsigned long long)d->last_seq, (double)d->last_t_ns * 1e-9,
           (unsigned long long)d->reads);
    APPEND("bus: configured=%d answering(BRD)=%d AL_or=0x%02X diag_frames_lost=%llu handoff_drops=%llu\n",
           d->n, d->brd_count, d->brd_al_or, (unsigned long long)d->frames_lost,
           (unsigned long long)d->handoff_drops);
    for (int g = 0; g < d->ngroups; g++) {
        const ecm_wkc_stats_t *w = &d->wkc[g];
        APPEND("wkc %-7s ok=%llu noframe=%llu zero=%llu partial=%llu(last missing %d) over=%llu "
               "max_run_bad=%u\n", group_names && group_names[g] ? group_names[g] : "group",
               (unsigned long long)w->count[ECM_WKC_OK], (unsigned long long)w->count[ECM_WKC_NOFRAME],
               (unsigned long long)w->count[ECM_WKC_ZERO], (unsigned long long)w->count[ECM_WKC_PARTIAL],
               w->last_missing, (unsigned long long)w->count[ECM_WKC_OVER], w->max_run_bad);
    }
    APPEND("SLAVE ADDR   STATE       CODE   LINK0/1  p0:inv/rx/fwd  p1:inv/rx/fwd  PU  PDI  LOST0/1  ANSWER\n");
    for (int i = 0; i < d->n; i++) {
        const ecm_diag_slave_t *s = &d->s[i];
        const ecm_diag_counts_t *t = &s->tot;
        char st[16];
        snprintf(st, sizeof(st), "%s%s", state_name(s->al_status), (s->al_status & AL_ERR) ? "+ERR" : "");
        APPEND("%5d 0x%04X %-11s 0x%04X %s/%s    %4llu/%llu/%llu      %4llu/%llu/%llu      %-3llu %-4llu %llu/%llu      %s\n",
               i + 1, s->station_addr, s->ever_answered ? st : "-", s->al_code,
               (s->dl_status & DL_LINK_PORT(0)) ? "up" : "--",
               (s->dl_status & DL_LINK_PORT(1)) ? "up" : "--",
               (unsigned long long)t->inv[0], (unsigned long long)t->rx[0], (unsigned long long)t->fwd[0],
               (unsigned long long)t->inv[1], (unsigned long long)t->rx[1], (unsigned long long)t->fwd[1],
               (unsigned long long)t->pu, (unsigned long long)t->pdi,
               (unsigned long long)t->lost[0], (unsigned long long)t->lost[1],
               s->seen_read != d->reads ? "not read" : s->answered ? "yes" : "NO");
    }
    if (nf == 0) {
        APPEND("findings: none\n");
    } else {
        APPEND("findings:\n");
        for (int k = 0; k < nf; k++) {
            char line[256];
            ecm_diag_finding_str(&f[k], al_str, line, sizeof(line));
            APPEND("  [%s] %s\n", sev_name(f[k].sev), line);
        }
    }
    return len;
}

int ecm_diag_write_file(const char *path, const char *text, size_t len)
{
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    size_t w = fwrite(text, 1, len, fp);
    if (fclose(fp) != 0 || w != len) { remove(tmp); return -1; }
    return rename(tmp, path) == 0 ? 0 : -1;
}

double ecm_diag_snapshot_time(const char *text)
{
    const char *p = strstr(text, "t_mono=");
    return p ? strtod(p + 7, NULL) : -1.0;
}
