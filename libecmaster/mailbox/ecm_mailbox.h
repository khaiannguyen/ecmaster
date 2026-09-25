/*
 * ecm_mailbox.h
 *
 * SDO/CoE mailbox subsystem for libecmaster, built on top of SOEM's
 * *cyclic* mailbox handler (ecx_slavembxcyclic / ecx_mbxhandler).
 *
 * Design summary (see project docs for the full derivation):
 *   - The socket (ctx->port) is touched from exactly ONE thread family:
 *     the RT thread, once per cycle, via ecx_mbxhandler().
 *   - ecx_SDOread()/ecx_SDOwrite(), called from the mailbox worker
 *     thread, never touch the socket directly once a slave is in
 *     cyclic mode -- they only push/pop a SOEM-internal, PI-mutex
 *     protected queue (confirmed in osal.c: osal_mutex_create() sets
 *     PTHREAD_PRIO_INHERIT), and block the CALLING (non-RT) thread
 *     until the RT thread has pumped the queue via ecx_mbxhandler().
 *   - This header intentionally exposes nothing about SOEM's mailbox
 *     internals to callers: ecm_mailbox_sdo_read/write() give a plain
 *     blocking call, safe to use from any non-RT thread (CLI, config,
 *     diagnostics). NEVER call them from the RT thread.
 *
 * Known, documented limitation (see integration notes): SOEM's error
 * list (context->elist, ecx_pusherror/ecx_poperror) is a plain,
 * unlocked ring buffer that can be written both by the RT thread
 * (ecx_mbxhandler -> ecx_mbxinhandler -> ecx_mbxerror/emergencyerror)
 * and by this module's worker thread (ecx_SDOread/write ->
 * ecx_SDOerror). This module deliberately does NOT call
 * ecx_poperror()/ecx_iserror() itself, to avoid adding a second
 * concurrent reader; draining elist is left to a single existing
 * thread (the monitor/giam-sat thread) by design.
 */
#ifndef ECM_MAILBOX_H
#define ECM_MAILBOX_H

#include <stdint.h>
#include <stdbool.h>
#include <signal.h>   /* sig_atomic_t -- matches the stop-flag convention
                       * already used by every thread in ecm_run.c
                       * (g_telemetry_stop, g_app_stop, g_mailbox_stop,
                       * g_monitor_stop are all volatile sig_atomic_t) */

/* Adjust to your repo's actual SOEM include path if different. Per
 * reading_list_master_v2.md (SOEM v2.0 migration notes), the public
 * umbrella header is soem/soem.h. */
#include "soem/soem.h"

/* ecx_mbxinhandler()/ecx_mbxouthandler() are real, exported symbols in
 * libsoem (confirmed: non-static in src/ec_main.c, and this module
 * links and runs correctly against them) but are NOT declared in any
 * public SOEM header (soem/soem.h et al.) -- only the combined
 * ecx_mbxhandler() wrapper is. Declared here ourselves, matching their
 * exact signatures read directly from src/ec_main.c, so this file
 * compiles with proper type checking instead of relying on C's
 * implicit-int-declaration fallback (which gcc only warns about, but
 * gives no signature checking at all). If a future SOEM version starts
 * declaring these itself, these two lines become a harmless duplicate
 * prototype, not a conflict. */
extern int ecx_mbxinhandler(ecx_contextt *context, uint8_t group, int limit);
extern int ecx_mbxouthandler(ecx_contextt *context, uint8_t group, int limit);

#ifdef __cplusplus
extern "C" {
#endif

/* Max SDO payload this module will carry through one job. Raise if a
 * CiA402 object you need is larger (segmented transfer is handled
 * transparently by ecx_SDOread/write either way; this only bounds the
 * copy buffer in this module). */
#define ECM_MBX_MAX_DATA 256

/* Bounded job queue between non-RT producer thread(s) (CLI, config,
 * diagnostics) and this module's single mailbox worker thread. Both
 * sides of this queue are non-RT -- see rationale in ecm_mailbox.c. */
#define ECM_MBX_JOB_QUEUE_LEN 32

/* Per-cycle work budget passed to ecx_mbxhandler() from the RT thread.
 * Keep small: a large SDO (segmented) will simply take more cycles to
 * finish rather than stretching a single cycle. Start at 1 and only
 * raise it after looking at the occupancy histogram with SDO traffic
 * running (L4-03). */
#define ECM_MBX_LIMIT_PER_CYCLE 1

typedef struct ecm_mailbox ecm_mailbox_t;

/* ---------------------------------------------------------------------
 * Part 1: enabling SOEM's cyclic mailbox handler
 * ------------------------------------------------------------------ */

/**
 * Enable ecx_slavembxcyclic() for every slave that is currently in
 * >= EC_STATE_PRE_OP and reports mailbox support (SII mbxstatus != 0).
 *
 * MUST be called:
 *   - after config_init() / the slaves have reached PRE_OP,
 *   - before the mailbox thread starts calling ecm_mailbox_run(),
 *   - before the RT thread starts calling ecx_mbxhandler().
 *
 * A slave for which cyclic mode could NOT be enabled is logged to
 * stderr and must never receive SDO calls from the worker thread
 * afterwards (it would fall back to SOEM's direct-socket mailbox
 * path and race the RT thread -- see design notes).
 *
 * @return number of slaves for which cyclic mode was enabled.
 */
int ecm_mailbox_enable_cyclic(ecx_contextt *ctx);

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */

/**
 * Allocate the mailbox subsystem: job queue + internal state. Does not
 * start any thread or spawn anything -- see ecm_mailbox_run() below,
 * this module does not own a thread of its own in this integration.
 */
ecm_mailbox_t *ecm_mailbox_create(ecx_contextt *ctx);

/**
 * Part 3: the mailbox worker LOOP -- not a thread spawner.
 *
 * ecm_run.c already has a dedicated "mailbox" thread as part of its
 * fixed 5-thread skeleton (master_plan_v2.md §2.4): telemetry / ứng
 * dụng / mailbox / giám sát / RT. Rather than have this module spawn
 * its OWN worker thread (which would make it a 6th thread), call this
 * function from INSIDE that existing thread's entry point, after it
 * has already called set_non_rt_thread():
 *
 *     static void *mailbox_thread_fn(void *arg) {
 *         set_non_rt_thread();
 *         ecm_mailbox_run(g_mbx, &g_mailbox_stop);
 *         return NULL;
 *     }
 *
 * This call blocks (processing queued SDO jobs as they arrive) until
 * *stop_flag becomes non-zero, then returns. It polls stop_flag every
 * ~200ms even with nothing queued, so shutdown is timely -- same
 * pattern as this file's own queue-wait internals.
 */
void ecm_mailbox_run(ecm_mailbox_t *mbx, volatile sig_atomic_t *stop_flag);

/** Free resources. Call this AFTER the thread running ecm_mailbox_run()
 * has been joined (i.e. after pthread_join() on the mailbox thread). */
void ecm_mailbox_destroy(ecm_mailbox_t *mbx);

/* ---------------------------------------------------------------------
 * Part 2: RT-thread side -- call once per cycle, per group, AFTER
 * ecx_receive_processdata_group(ctx, group) for that group.
 * ------------------------------------------------------------------ */

/**
 * Pumps pending mailbox I/O for this group, THIS IS THE ONLY function in
 * this module allowed to run on the RT thread; every other function
 * below is for non-RT callers only.
 *
 * Deliberately calls ecx_mbxinhandler()/ecx_mbxouthandler() SEPARATELY
 * rather than the combined ecx_mbxhandler() wrapper, and reports each
 * side's own count via out_received/out_sent (either may be NULL if the
 * caller doesn't need it) -- confirmed necessary by reading ec_main.c:
 * ecx_mbxhandler()'s return value is ONLY ecx_mbxouthandler()'s count;
 * ecx_mbxinhandler()'s own count (how many real ecx_FPRD() frames it
 * issued this call, each one a genuine extra sendto() on the wire, NOT
 * gated by budget alone but by whether the SM1 "full" bit was actually
 * set) is silently discarded by that wrapper. A caller that needs to
 * keep an independent frame-order tracker in sync (e.g. Giai doan 4's
 * tx_order_ring turnaround measurement) needs BOTH counts, in the same
 * order these two calls make them: ecx_mbxinhandler()'s ecx_FPRD calls
 * happen before ecx_mbxouthandler()'s ecx_FPWR calls.
 *
 * Also note (confirmed by reading ec_main.c's ecx_FPWR/ecx_FPRD usage
 * here): each real frame this issues is a BLOCKING single-datagram
 * send+wait, done synchronously inside this call -- unlike process
 * data's own async send/receive pair. Its wall-clock cost is real RT
 * cycle time, not a measurement artifact.
 *
 * @return total real extra frames sent this cycle (received + sent),
 *         0 if nothing was pending -- same meaning the old combined
 *         wrapper's return value had, kept for callers that only care
 *         about "did anything happen".
 */
static inline int ecm_mailbox_rt_pump_group(ecx_contextt *ctx, uint8_t group,
                                             int *out_received, int *out_sent)
{
    int received = ecx_mbxinhandler(ctx, group, ECM_MBX_LIMIT_PER_CYCLE);
    int sent     = ecx_mbxouthandler(ctx, group, ECM_MBX_LIMIT_PER_CYCLE - received);
    if (out_received) *out_received = received;
    if (out_sent)      *out_sent    = sent;
    return received + sent;
}

/* ---------------------------------------------------------------------
 * Non-RT, blocking API -- for CLI / config / diagnostics callers.
 * NEVER call these from the RT thread.
 * ------------------------------------------------------------------ */

/**
 * Enqueue an SDO upload (read) and block the CALLING thread until the
 * worker thread completes it (or the wait itself times out).
 *
 * @param inout_size  in: capacity of 'out'; out: bytes actually read.
 * @param timeout_us  forwarded to ecx_SDOread() as-is (SOEM's own
 *                    protocol timeout, e.g. EC_TIMEOUTRXM).
 *
 * @return  0 success (out/inout_size hold the result)
 *         -1 job queue full (worker is backed up -- retry later)
 *         -2 worker did not finish within timeout_us + safety margin
 *            (this indicates something is stuck -- e.g. worker died,
 *            or slave stopped answering; NOT a normal SDO timeout,
 *            those are reported as -4)
 *         -3 requested size exceeds ECM_MBX_MAX_DATA
 *         -4 SOEM reported wkc <= 0 (protocol-level failure: Abort
 *            code, SDO timeout, or malformed response -- the decoded
 *            Abort code, if any, is on the monitor thread's error log,
 *            not here -- see module-level design note above)
 */
int ecm_mailbox_sdo_read(ecm_mailbox_t *mbx, uint16_t slave, uint16_t index,
                          uint8_t subindex, bool complete_access,
                          uint8_t *out, int *inout_size, int timeout_us);

/** Same contract as ecm_mailbox_sdo_read(), for an SDO download (write). */
int ecm_mailbox_sdo_write(ecm_mailbox_t *mbx, uint16_t slave, uint16_t index,
                           uint8_t subindex, bool complete_access,
                           const uint8_t *data, int size, int timeout_us);

#ifdef __cplusplus
}
#endif

#endif /* ECM_MAILBOX_H */