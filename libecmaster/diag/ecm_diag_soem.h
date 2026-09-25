#ifndef ECM_DIAG_SOEM_H
#define ECM_DIAG_SOEM_H

#include <stdint.h>
#include "soem/soem.h"
#include "ecm_diag.h"

/* ==========================================================================
 * ecm_diag_soem.h — SOEM glue for ecm_diag (Phase 7.2).
 *
 * RT path (ecm_run): one frame, several datagrams (plan §3.1):
 *     BRD  0x0130 (2)                    -> WKC = slaves answering
 *     per slave: FPRW 0x0300 (20)        -> error counters, read AND cleared
 *                FPRD 0x0110 (2)         -> DL status (links)
 *                FPRD 0x0130 (6)         -> AL status + AL status code
 * 14 + 64 bytes per slave -> at most ECM_DIAG_SOEM_SLAVES_PER_FRAME slaves
 * per frame; larger buses are read in chunks on consecutive epochs.
 *
 * Non-blocking: ecm_diag_soem_send() transmits and returns; the reply is
 * picked up on a later tick by ecm_diag_soem_collect(). SOEM's own receive
 * (ecx_inframe) stores a frame whose index is in state EC_BUF_TX when it
 * reads it while waiting for the process data frame, so collect() only
 * checks rxbufstat[idx] == EC_BUF_RCVD and never touches the socket or
 * waits. The caller must push g_tx_order once per send (one sendto()).
 *
 * Standalone path (ecm_diag --standalone): blocking APRD by position, no
 * configuration, no state change, counters only read (not cleared).
 * ========================================================================== */

#define ECM_DIAG_SOEM_SLAVES_PER_FRAME 22
#define ECM_DIAG_SOEM_MAX_DG (1 + 3 * ECM_DIAG_SOEM_SLAVES_PER_FRAME)

typedef struct {
    int      pending;
    uint8_t  idx;
    int      age;                /* collect attempts without a reply   */
    int      first, count, clear_on_read;
    int      nd;
    uint16_t off[ECM_DIAG_SOEM_MAX_DG], len[ECM_DIAG_SOEM_MAX_DG];
    uint64_t seq, frames_lost, frames_sent;
    uint64_t foreign;            /* replies to older frames with our index, dropped */
    int      next_first;         /* chunk rotation                     */
} ecm_diag_io_t;

void ecm_diag_soem_io_init(ecm_diag_io_t *io);

/* Build and send the next chunk. Returns 1 if a frame went out (push
 * g_tx_order), 0 if nothing was sent (still pending, or no slaves). */
int ecm_diag_soem_send(ecx_contextt *ctx, ecm_diag_io_t *io, int clear_on_read);

/* Returns 1 and fills *out when the reply is in; 0 if still waiting;
 * -1 when given up after max_age attempts (out->frame_ok = 0). */
int ecm_diag_soem_collect(ecx_contextt *ctx, ecm_diag_io_t *io, ecm_diag_raw_t *out,
                          uint64_t now_ns, int max_age);

/* Blocking, position-addressed read of up to n slaves. Returns the BRD
 * count (slaves answering), or <= 0 on failure. */
int ecm_diag_soem_read_positional(ecx_contextt *ctx, int n, ecm_diag_raw_t *out,
                                  uint64_t now_ns);

#endif /* ECM_DIAG_SOEM_H */
