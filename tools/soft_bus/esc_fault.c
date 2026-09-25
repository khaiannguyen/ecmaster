/* ==========================================================================
 * esc_fault.c — fault injection for soft_bus (Phase 7). See esc_fault.h for
 * the command list and the frame flow.
 *
 * Error counter semantics (Section I §14, Section II §2.9), used by
 * bad_cable. A cable fault between node k-1 and node k in the forward
 * direction:
 *   - node k, port 0: invalid frame 0x0300 +1, RX error 0x0301 +1, and the
 *     frame passes its processing unit -> 0x030C +1. The ESC marks the frame.
 *   - nodes k+1..last, port 0: forwarded error 0x0308 +1, 0x030C +1.
 *   - on the way back every node except the last one receives the marked
 *     frame on port 1 (loop open): forwarded error 0x0309 +1. The return
 *     path does not pass the processing unit, so no 0x030C there.
 *   - the last node's port 1 loop is closed: "errors are only counted if
 *     the loop of the port is open" -> nothing on its port 1.
 *   - writes are not committed by ESCs that see a bad FCS, so only nodes
 *     0..k-1 process the frame; the master gets a bad frame and the NIC
 *     drops it -> no reply.
 * Localisation rule for 7.2: the first node with 0x0300/0x0301 on port 0
 * is node k -> the cable between k-1 and k.
 * 0x0314 (extended RX error) is not modelled: LAN9252 does not have it.
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esc_types.h"
#include "esc_core.h"
#include "esc_dc.h"
#include "esc_fault.h"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

#define LOG(f, now, ...) do { if ((f)->log) {                                  \
        fprintf((f)->log, "fault: t_raw=%.6f ", (double)(now) * 1e-9);          \
        fprintf((f)->log, __VA_ARGS__); fputc('\n', (f)->log); fflush((f)->log); } } while (0)

void esc_fault_init(esc_fault_bus_t *f, FILE *log, int app_seq_offset)
{
    memset(f, 0, sizeof(*f));
    f->log = log;
    f->app_seq_offset = app_seq_offset;
    f->bad_cable_node = -1;
}

/* First powered-off node, or n: the frame turns around before it. */
static int reachable(const esc_t *chain, int n)
{
    for (int i = 0; i < n; i++)
        if (chain[i].fault.powered_off) return i;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Transmit queue                                                            */
/* ------------------------------------------------------------------------ */
static esc_fault_txq_slot_t *txq_put(esc_fault_bus_t *f, const uint8_t *buf, size_t len,
                                     uint64_t due_ns)
{
    if (len > FAULT_FRAME_MAX) len = FAULT_FRAME_MAX;
    for (int i = 0; i < FAULT_TXQ_LEN; i++) {
        esc_fault_txq_slot_t *s = &f->q[i];
        if (s->used) continue;
        s->used = 1; s->held = 0;
        s->len = (uint16_t)len;
        s->due_ns = due_ns;
        s->seq = f->seq++;
        memcpy(s->buf, buf, len);
        return s;
    }
    f->q_overflow++;
    return NULL;
}

static esc_fault_txq_slot_t *txq_held(esc_fault_bus_t *f)
{
    for (int i = 0; i < FAULT_TXQ_LEN; i++)
        if (f->q[i].used && f->q[i].held) return &f->q[i];
    return NULL;
}

int esc_fault_pop_due(esc_fault_bus_t *f, uint64_t now_ns, const uint8_t **buf, uint16_t *len)
{
    esc_fault_txq_slot_t *best = NULL;
    for (int i = 0; i < FAULT_TXQ_LEN; i++) {
        esc_fault_txq_slot_t *s = &f->q[i];
        if (!s->used || s->due_ns > now_ns) continue;
        if (!best || s->due_ns < best->due_ns ||
            (s->due_ns == best->due_ns && s->seq < best->seq))
            best = s;
    }
    if (!best) return 0;
    if (best->held) f->reorder_alone++;      /* nothing came to swap with */
    best->used = 0;
    best->held = 0;
    *buf = best->buf;
    *len = best->len;
    f->tx++;
    return 1;
}

uint64_t esc_fault_next_due(const esc_fault_bus_t *f)
{
    uint64_t m = 0;
    for (int i = 0; i < FAULT_TXQ_LEN; i++)
        if (f->q[i].used && (m == 0 || f->q[i].due_ns < m)) m = f->q[i].due_ns;
    return m;
}

/* ------------------------------------------------------------------------ */
/* Frame hooks                                                               */
/* ------------------------------------------------------------------------ */
static void apply_bad_cable_counters(esc_t *chain, int n_reach, int k)
{
    esc_cnt_inc(&chain[k], REG_ERR_INVALID_P(0));
    esc_cnt_inc(&chain[k], REG_ERR_RX_P(0));
    esc_cnt_inc(&chain[k], REG_ERR_ECAT_PU);
    for (int i = k + 1; i < n_reach; i++) {
        esc_cnt_inc(&chain[i], REG_ERR_FWD_P(0));
        esc_cnt_inc(&chain[i], REG_ERR_ECAT_PU);
    }
    for (int i = 0; i < n_reach - 1; i++)          /* return path, port 1 */
        esc_cnt_inc(&chain[i], REG_ERR_FWD_P(1));
}

int esc_fault_frame_begin(esc_fault_bus_t *f, esc_t *chain, int n,
                          const uint8_t *buf, size_t len, uint64_t now_ns)
{
    f->rx++;
    f->f_drop = 0;
    f->f_np = 0;
    f->f_has_pd = esc_dc_frame_has_pd(buf, len);

    /* Per-node housekeeping for every powered node, reachable or not: an
     * isolated node's watchdog keeps running on its own. */
    for (int i = 0; i < n; i++) {
        esc_t *e = &chain[i];
        e->fault.skip_logical = 0;
        e->fault.stale_frame  = 0;
        e->fault.sm1_consumed = 0;
        if (e->fault.powered_off) continue;
        if (e->fault.mbx_dup_pending) {
            e->fault.mbx_dup_pending = 0;
            e->regs[REG_SM1_STATUS] |= SM_STATUS_MAILBOX_FULL;   /* same bytes, same Cnt */
            f->mbx_dup++;
            LOG(f, now_ns, "mbx_dup: node %d re-posted its last mailbox response", i);
        }
        if (esc_wd_check(e, now_ns)) {
            f->wd_drops++;
            LOG(f, now_ns, "node %d: SM watchdog expired (%.3f ms since last trigger) "
                "-> SAFEOP+ERR 0x001B", i, (double)(now_ns - e->wd.last_trigger_ns) * 1e-6);
        }
    }

    if (f->mute_left) {
        f->muted++;
        if (--f->mute_left == 0) LOG(f, now_ns, "mute: done");
        f->f_drop = 1;
        return 0;
    }

    int n_reach = reachable(chain, n);
    if (n_reach == 0) {                  /* node 0 is off: nothing answers */
        f->no_link++;
        f->f_drop = 1;
        return 0;
    }

    int np = n_reach;
    if (f->bad_cable_left && f->bad_cable_node >= 0 && f->bad_cable_node < n_reach) {
        int k = f->bad_cable_node;
        apply_bad_cable_counters(chain, n_reach, k);
        f->bad_cable++;
        f->f_drop = 1;
        np = k;
        if (--f->bad_cable_left == 0) LOG(f, now_ns, "bad_cable: done (node %d)", k);
    }

    if (f->f_has_pd) {
        for (int i = 0; i < np; i++) {
            esc_node_fault_t *nf = &chain[i].fault;
            if (nf->wkc_short_left) {
                nf->skip_logical = 1;
                if (--nf->wkc_short_left == 0) LOG(f, now_ns, "wkc_short: done (node %d)", i);
            }
            if (nf->stale_left) {
                nf->stale_frame = 1;
                if (--nf->stale_left == 0) LOG(f, now_ns, "stale: done (node %d)", i);
            }
        }
    }
    f->f_np = np;
    return np;
}

/* The slave application's cyclic step: a 16-bit sequence counter in its
 * TxPDO, refreshed once per process data frame in SAFEOP/OP. This is part of
 * the slave's own PDO contract (plan §3.4), which is why it is opt-in and
 * placed by the slave, never by libecmaster. */
static void app_seq_step(esc_fault_bus_t *f, esc_t *e)
{
    if (f->app_seq_offset < 0 || e->fault.stale_frame) return;
    uint8_t state = e->regs[REG_AL_STATUS] & 0x0F;
    if (state != ESM_SAFEOP && state != ESM_OP) return;
    const uint8_t *sm3 = e->regs + REG_SM_BASE + 3 * REG_SM_ENTRY_SIZE;
    if (!(sm3[SM_OFF_ACTIVATE] & SM_ACT_ENABLE)) return;
    uint16_t start = rd16(sm3 + SM_OFF_PHYS_START), smlen = rd16(sm3 + SM_OFF_LENGTH);
    if ((uint32_t)f->app_seq_offset + 2 > smlen) return;
    if ((uint32_t)start + smlen > ESC_REG_SPACE_SIZE) return;
    e->fault.app_seq++;
    wr16(e->regs + start + f->app_seq_offset, e->fault.app_seq);
}

void esc_fault_frame_end(esc_fault_bus_t *f, esc_t *chain, int n,
                         const uint8_t *buf, size_t len, uint64_t now_ns)
{
    (void)n;
    for (int i = 0; i < f->f_np; i++) {
        esc_t *e = &chain[i];
        if (f->f_has_pd) app_seq_step(f, e);
        if (e->fault.sm1_consumed && e->fault.mbx_dup_armed) {
            e->fault.mbx_dup_armed = 0;
            e->fault.mbx_dup_pending = 1;
        }
        if (e->fault.sm1_consumed && e->fault.mbx_lose_armed && !f->f_drop) {
            e->fault.mbx_lose_armed = 0;
            f->mbx_lost++;
            f->f_drop = 1;
            LOG(f, now_ns, "mbx_repeat: reply carrying node %d's mailbox response lost", i);
        }
    }
    if (f->f_drop) return;

    uint64_t due = now_ns;
    if (f->late_left) {
        due += (uint64_t)f->late_us * 1000u;
        f->late++;
        if (--f->late_left == 0) LOG(f, now_ns, "late: done");
    }

    esc_fault_txq_slot_t *held = txq_held(f);
    if (held) {
        /* Reply of frame i+1 goes first, then the held reply of frame i. */
        txq_put(f, buf, len, due);
        held->held = 0;
        held->due_ns = due;
        held->seq = f->seq++;
        f->reordered++;
    } else if (f->reorder_left) {
        esc_fault_txq_slot_t *s = txq_put(f, buf, len, now_ns + FAULT_REORDER_MAX_NS);
        if (s) s->held = 1;
        if (--f->reorder_left == 0) LOG(f, now_ns, "reorder: last frame held");
    } else {
        txq_put(f, buf, len, due);
    }

    if (f->dup_left) {
        txq_put(f, buf, len, due);
        f->dup++;
        if (--f->dup_left == 0) LOG(f, now_ns, "dup: done");
    }
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                  */
/* ------------------------------------------------------------------------ */
static int parse_u(const char *s, unsigned long *v)
{
    if (!s) return -1;
    char *end;
    *v = strtoul(s, &end, 0);          /* base 0: accepts 0x001B */
    return (*end == '\0') ? 0 : -1;
}

static void node_power_on(esc_t *chain, int n, int k)
{
    esc_t *e = &chain[k];
    uint16_t pdo = e->pdo_size_bytes;
    uint8_t react = e->wd.react;
    memset(e, 0, sizeof(*e));          /* power-on: everything forgotten */
    esc_init(e, (uint8_t)k, pdo);
    e->wd.react = react;
    esc_dc_node_reset(e, k);
    esc_chain_wire(chain, n);
}

void esc_fault_status(const esc_fault_bus_t *f, const esc_t *chain, int n, FILE *out)
{
    if (!out) return;
    fprintf(out, "fault: status rx=%llu tx=%llu muted=%llu bad_cable=%llu late=%llu dup=%llu "
            "reordered=%llu reorder_alone=%llu mbx_lost=%llu mbx_dup=%llu no_link=%llu "
            "q_overflow=%llu wd_drops=%llu\n",
            (unsigned long long)f->rx, (unsigned long long)f->tx, (unsigned long long)f->muted,
            (unsigned long long)f->bad_cable, (unsigned long long)f->late,
            (unsigned long long)f->dup, (unsigned long long)f->reordered,
            (unsigned long long)f->reorder_alone, (unsigned long long)f->mbx_lost,
            (unsigned long long)f->mbx_dup, (unsigned long long)f->no_link,
            (unsigned long long)f->q_overflow, (unsigned long long)f->wd_drops);
    fprintf(out, "fault: pending mute=%u late=%u(%uus) dup=%u reorder=%u bad_cable=%u(node %d)\n",
            f->mute_left, f->late_left, f->late_us, f->dup_left, f->reorder_left,
            f->bad_cable_left, f->bad_cable_node);
    for (int i = 0; i < n; i++) {
        const esc_t *e = &chain[i];
        const uint8_t *r = e->regs;
        fprintf(out, "fault: node %d %s AL=0x%02X code=0x%04X DL=0x%04X "
                "err[inv0 rx0 inv1 rx1 fwd0 fwd1 pu lost0 lost1]=%u %u %u %u %u %u %u %u %u "
                "wd[st cnt]=%u %u sm2=0x%04X/%u wkc_short=%u stale=%u seq=%u mbx_rep=%llu\n",
                i, e->fault.powered_off ? "OFF" : "on ",
                r[REG_AL_STATUS], rd16(r + REG_AL_STATUS_CODE), rd16(r + REG_DL_STATUS),
                r[0x0300], r[0x0301], r[0x0302], r[0x0303], r[0x0308], r[0x0309],
                r[REG_ERR_ECAT_PU], r[0x0310], r[0x0311],
                r[REG_WD_STATUS_PD] & 1u, r[REG_WD_COUNTER_PD],
                rd16(r + REG_SM_BASE + 2 * REG_SM_ENTRY_SIZE),
                rd16(r + REG_SM_BASE + 2 * REG_SM_ENTRY_SIZE + SM_OFF_LENGTH),
                e->fault.wkc_short_left, e->fault.stale_left, e->fault.app_seq,
                (unsigned long long)e->fault.mbx_repeats_served);
    }
    fflush(out);
}

int esc_fault_command(esc_fault_bus_t *f, esc_t *chain, int n, const char *line, uint64_t now_ns)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char *hash = strchr(tmp, '#');
    if (hash) *hash = '\0';

    char *argv[4] = { 0 };
    int argc = 0;
    for (char *t = strtok(tmp, " \t\r\n"); t && argc < 4; t = strtok(NULL, " \t\r\n"))
        argv[argc++] = t;
    if (argc == 0) return 0;                       /* blank / comment */

    const char *cmd = argv[0];
    unsigned long a = 0, b = 0;
    int node = -1;

#define NEED_NODE(idx) do {                                                    \
        if (parse_u(argv[idx], &a) || a >= (unsigned long)n) goto bad_node;    \
        node = (int)a; } while (0)
#define NEED_U(idx, var) do { if (parse_u(argv[idx], &(var))) goto bad_args; } while (0)

    if (!strcmp(cmd, "mute") && argc == 2) {
        NEED_U(1, b);
        f->mute_left = (uint32_t)b;
        LOG(f, now_ns, "mute: next %lu frames lost", b);
    } else if (!strcmp(cmd, "wkc_short") && argc == 3) {
        NEED_NODE(1); NEED_U(2, b);
        chain[node].fault.wkc_short_left = (uint32_t)b;
        LOG(f, now_ns, "wkc_short: node %d (SOEM slave %d) ignores L* for %lu PD frames",
            node, node + 1, b);
    } else if (!strcmp(cmd, "stale") && argc == 3) {
        NEED_NODE(1); NEED_U(2, b);
        chain[node].fault.stale_left = (uint32_t)b;
        LOG(f, now_ns, "stale: node %d (SOEM slave %d) TxPDO frozen for %lu PD frames%s",
            node, node + 1, b, f->app_seq_offset < 0 ? " [WARNING: --app-seq is off, "
            "TxPDO never changes anyway]" : "");
    } else if (!strcmp(cmd, "safeop") && argc == 3) {
        NEED_NODE(1); NEED_U(2, b);
        esc_t *e = &chain[node];
        uint8_t cur = e->regs[REG_AL_STATUS] & 0x0F;
        uint8_t nxt = (cur == ESM_OP || cur == ESM_SAFEOP) ? ESM_SAFEOP : cur;
        wr16(e->regs + REG_AL_STATUS, (uint16_t)(nxt | 0x10));
        wr16(e->regs + REG_AL_STATUS_CODE, (uint16_t)b);
        if (nxt == ESM_SAFEOP) e->got_valid_outputs = 0;
        LOG(f, now_ns, "safeop: node %d (SOEM slave %d) 0x%02X -> 0x%02X, AL status code 0x%04lX",
            node, node + 1, cur, nxt | 0x10, b);
    } else if (!strcmp(cmd, "bad_cable") && argc == 3) {
        NEED_NODE(1); NEED_U(2, b);
        f->bad_cable_node = node;
        f->bad_cable_left = (uint32_t)b;
        LOG(f, now_ns, "bad_cable: cable into node %d (between node %d and node %d) bad for %lu frames",
            node, node - 1, node, b);
    } else if (!strcmp(cmd, "late") && argc == 3) {
        NEED_U(1, a); NEED_U(2, b);
        f->late_us = (uint32_t)a;
        f->late_left = (uint32_t)b;
        LOG(f, now_ns, "late: next %lu replies delayed by %lu us", b, a);
    } else if (!strcmp(cmd, "dup") && argc == 2) {
        NEED_U(1, b);
        f->dup_left = (uint32_t)b;
        LOG(f, now_ns, "dup: next %lu replies sent twice", b);
    } else if (!strcmp(cmd, "reorder") && argc == 2) {
        NEED_U(1, b);
        f->reorder_left = (uint32_t)b;
        LOG(f, now_ns, "reorder: %lu reply pairs swapped", b);
    } else if (!strcmp(cmd, "drop_node") && argc == 2) {
        NEED_NODE(1);
        if (chain[node].fault.powered_off) { LOG(f, now_ns, "drop_node: node %d already off", node); return 0; }
        chain[node].fault.powered_off = 1;
        if (node > 0 && !chain[node - 1].fault.powered_off)
            esc_cnt_inc(&chain[node - 1], REG_LOST_LINK_P(1));
        if (node < n - 1 && !chain[node + 1].fault.powered_off)
            esc_cnt_inc(&chain[node + 1], REG_LOST_LINK_P(0));
        esc_chain_wire(chain, n);
        LOG(f, now_ns, "drop_node: node %d (SOEM slave %d) powered off, bus now ends at node %d",
            node, node + 1, reachable(chain, n) - 1);
    } else if (!strcmp(cmd, "restore_node") && argc == 2) {
        NEED_NODE(1);
        if (!chain[node].fault.powered_off) { LOG(f, now_ns, "restore_node: node %d is not off", node); return 0; }
        node_power_on(chain, n, node);
        LOG(f, now_ns, "restore_node: node %d (SOEM slave %d) powered on (INIT, station address 0)",
            node, node + 1);
    } else if ((!strcmp(cmd, "mbx_repeat") || !strcmp(cmd, "mbx_dup")) && argc <= 2) {
        node = 0;
        if (argc == 2) NEED_NODE(1);
        if (cmd[4] == 'r') chain[node].fault.mbx_lose_armed = 1;
        else               chain[node].fault.mbx_dup_armed = 1;
        LOG(f, now_ns, "%s: armed on node %d (SOEM slave %d)", cmd, node, node + 1);
    } else if (!strcmp(cmd, "reject_al") && argc == 2) {
        if (!strcmp(argv[1], "all")) {
            for (int i = 0; i < n; i++) chain[i].force_reject_al = 1;
        } else {
            NEED_NODE(1);
            chain[node].force_reject_al = 1;
        }
        LOG(f, now_ns, "reject_al: next AL Control request of %s will be rejected", argv[1]);
    } else if (!strcmp(cmd, "clear") && argc == 1) {
        f->mute_left = f->late_left = f->dup_left = f->reorder_left = f->bad_cable_left = 0;
        for (int i = 0; i < n; i++) {
            esc_node_fault_t *nf = &chain[i].fault;
            nf->wkc_short_left = nf->stale_left = 0;
            nf->mbx_lose_armed = nf->mbx_dup_armed = nf->mbx_dup_pending = 0;
            chain[i].force_reject_al = 0;
        }
        LOG(f, now_ns, "clear: all pending injections cancelled");
    } else if (!strcmp(cmd, "status") && argc == 1) {
        esc_fault_status(f, chain, n, f->log);
    } else {
        goto bad_args;
    }
    return 0;

bad_node:
    LOG(f, now_ns, "ERROR: '%s': node must be 0..%d (0-based chain position)", line, n - 1);
    return -1;
bad_args:
    LOG(f, now_ns, "ERROR: unknown command or wrong arguments: '%s'", line);
    return -1;
#undef NEED_NODE
#undef NEED_U
}
