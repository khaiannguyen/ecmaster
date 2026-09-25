#ifndef ESC_FAULT_H
#define ESC_FAULT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "esc_types.h"

/* ==========================================================================
 * esc_fault.h — fault injection for soft_bus (Phase 7, L5-01..L5-13).
 *
 * Same split as the rest of soft_bus: no sockets here. soft_bus_main.c feeds
 * command lines (from the control FIFO) and frames in, and sends whatever
 * esc_fault_pop_due() hands back. Everything is therefore testable offline
 * (test_fault.c) with explicit timestamps.
 *
 * Frame flow per received frame:
 *     np = esc_fault_frame_begin(f, chain, n, buf, len, now);
 *     if (np > 0) process_frame(chain, np, buf, len);
 *     esc_fault_frame_end(f, chain, n, buf, len, now);
 *     while (esc_fault_pop_due(f, now, &p, &l)) sendto(p, l);
 *
 * Node numbering: node k is the k-th ESC in the chain, 0-based, the same
 * numbering as the dcsync report ("ref=node0"). SOEM's slave number is k+1.
 *
 * Durations are counted in frames, never in time, so an injection means the
 * same thing at any cycle time:
 *   - mute / bad_cable / late / dup / reorder: every frame received
 *   - wkc_short / stale: frames that carry process data (LRD/LWR/LRW)
 *
 * Commands (one per line; '#' starts a comment):
 *   mute <n>                  next n frames are lost before node 0: no ESC
 *                             sees them, no reply (L5-02)
 *   wkc_short <node> <n>      node ignores LRD/LWR/LRW in the next n PD frames:
 *                             no data exchanged, no WKC contribution (L5-01)
 *   safeop <node> <code>      node falls to SAFEOP+ERR with AL status code
 *                             <code> on its own, as a local error would (L5-05)
 *   bad_cable <node> <n>      physical errors on the cable INTO node (between
 *                             node-1 and node) for the next n frames (L5-06)
 *   late <us> <n>             reply of the next n frames delayed by <us> (L5-07)
 *   dup <n>                   reply of the next n frames sent twice (L5-08)
 *   reorder <n>               n times: hold a reply and send it right after
 *                             the reply of the following frame (L5-08)
 *   stale <node> <n>          node's TxPDO frozen for n PD frames while WKC
 *                             stays correct; needs --app-seq (L5-09)
 *   drop_node <node>          node powered off: bus ends at node-1 (L5-11)
 *   restore_node <node>       node powered on again, fresh from reset (L5-11)
 *   mbx_repeat [node]         lose the reply of the next frame that fetches
 *                             node's SM1 response -> master must use the
 *                             repeat request (L5-12)
 *   mbx_dup [node]            post node's next mailbox response twice, same
 *                             Cnt (L5-12)
 *   reject_al <node|all>      reject the next AL Control request (L2-05;
 *                             same as SIGUSR1 for all nodes)
 *   clear                     cancel all pending injections (does not
 *                             restore dropped nodes)
 *   status                    print injection state and error counters
 * ========================================================================== */

#define FAULT_TXQ_LEN        64
#define FAULT_FRAME_MAX      2048
#define FAULT_REORDER_MAX_NS (50ull * 1000 * 1000)  /* held reply sent alone after 50 ms */

typedef struct {
    uint8_t  used;
    uint8_t  held;          /* reorder: waiting for the next reply           */
    uint16_t len;
    uint64_t due_ns;
    uint64_t seq;           /* tie-break: equal due times keep enqueue order */
    uint8_t  buf[FAULT_FRAME_MAX];
} esc_fault_txq_slot_t;

typedef struct {
    /* ---- configuration ---- */
    int      app_seq_offset;   /* byte offset of the 16-bit sequence counter in
                                * each node's TxPDO; -1 = feature off        */
    FILE    *log;              /* event log, NULL = silent                   */

    /* ---- bus-level injections, remaining frames ---- */
    uint32_t mute_left;
    uint32_t late_left, late_us;
    uint32_t dup_left;
    uint32_t reorder_left;
    int      bad_cable_node;
    uint32_t bad_cable_left;

    /* ---- per-frame decision, frame_begin -> frame_end ---- */
    int      f_np;             /* nodes that process this frame              */
    int      f_has_pd;
    int      f_drop;           /* the reply never reaches the master         */

    /* ---- transmit queue ---- */
    esc_fault_txq_slot_t q[FAULT_TXQ_LEN];
    uint64_t seq;

    /* ---- statistics ---- */
    uint64_t rx, tx, muted, bad_cable, late, dup, reordered, reorder_alone,
             mbx_lost, mbx_dup, no_link, q_overflow, wd_drops;
} esc_fault_bus_t;

void esc_fault_init(esc_fault_bus_t *f, FILE *log, int app_seq_offset);

/* Parse and apply one command line. Returns 0 on success, -1 on error (a
 * message is written to f->log either way). */
int  esc_fault_command(esc_fault_bus_t *f, esc_t *chain, int n,
                       const char *line, uint64_t now_ns);

/* Returns how many nodes (0..n) process this frame, in chain order. 0 means
 * the frame reaches no ESC at all. Also evaluates every powered node's
 * process data watchdog at now_ns. */
int  esc_fault_frame_begin(esc_fault_bus_t *f, esc_t *chain, int n,
                           const uint8_t *buf, size_t len, uint64_t now_ns);

/* After process_frame(): slave application step (--app-seq), mailbox
 * injections, then queues the reply (or not) according to the active
 * injections. */
void esc_fault_frame_end(esc_fault_bus_t *f, esc_t *chain, int n,
                         const uint8_t *buf, size_t len, uint64_t now_ns);

/* Next reply to send whose due time has passed, in (due, seq) order.
 * Returns 1 and a pointer valid until the next call, 0 if none is due. */
int  esc_fault_pop_due(esc_fault_bus_t *f, uint64_t now_ns,
                       const uint8_t **buf, uint16_t *len);

/* Earliest due time in the queue, 0 if the queue is empty. */
uint64_t esc_fault_next_due(const esc_fault_bus_t *f);

void esc_fault_status(const esc_fault_bus_t *f, const esc_t *chain, int n, FILE *out);

#endif /* ESC_FAULT_H */
