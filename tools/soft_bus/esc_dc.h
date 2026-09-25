#ifndef ESC_DC_H
#define ESC_DC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "esc_types.h"

/* ==========================================================================
 * esc_dc.h — Distributed Clock model for soft_bus (Phase 6 / L6).
 *
 * What is modelled (Section I §9.1, Section II §2.15):
 *   - per-node free-running local clock with configurable crystal drift
 *   - propagation delay: frame hits node i at T_rx + i*hop_ns
 *   - 0x0900 write        -> latch receive time port0/port1 + 0x0918 (SOF)
 *   - 0x0910 read (ECAT)  -> system time at SOF of this frame
 *   - 0x0910 write (ECAT) -> delta-t into the time control loop (P + I)
 *   - 0x092C              -> filtered delta-t, sign-magnitude as in §2.15.2.4
 *   - 0x0920 / 0x0928     -> offset / delay (32-bit mode: upper offset ignored)
 *   - 0x0981 + 0x0990 + 0x09A0 -> SYNC0 activation, next-pulse read-back
 *   - 32 / 64-bit DC width (0x0008 bit3)
 *
 * What is NOT modelled: SYNC1, latch units, speed counter registers,
 * SYNC0 pulse length, PDI-controlled system time.
 *
 * GROUND TRUTH: for every process-data frame, each node records the phase of
 * the frame's arrival relative to ITS OWN SYNC0 grid, and counts SYNC0
 * intervals that received no process-data frame at all ("no_frame"). This is
 * measured on the slave side, independently of what the master believes —
 * the master's own phase error cannot reveal a bug in the master's own
 * phase computation.
 * ========================================================================== */

typedef struct {
    int     width;              /* 0 = DC not available, 32 or 64            */
    int64_t hop_ns;             /* simulated forward delay per node          */
    int64_t ref_drift_ppb;      /* drift of node 0 (the future ref clock)    */
    int64_t other_drift_ppb;    /* amplitude for nodes 1..n-1 (spread +-)    */
} esc_dc_cfg_t;

/* Call once after esc_init()/esc_chain_wire() on the whole chain. */
void esc_dc_setup(esc_t *chain, int n, const esc_dc_cfg_t *cfg);

/* Called by soft_bus_main around process_frame(). t_host_ns must be taken
 * with CLOCK_MONOTONIC_RAW right after recvfrom() returns. */
void esc_dc_frame_begin(esc_t *chain, int n, uint64_t t_host_ns);
void esc_dc_frame_end(esc_t *chain, int n, int has_pd);

/* Hooks for esc_core.c physical access. */
void esc_dc_before_read(esc_t *esc, uint16_t off, uint16_t len);
void esc_dc_after_write(esc_t *esc, uint16_t off, const uint8_t *data, uint16_t len);

/* 1 if the frame carries an LRD/LWR/LRW datagram (cyclic process data). */
int  esc_dc_frame_has_pd(const uint8_t *buf, size_t len);

/* Print one line per SYNC0-active node for the current window, then reset
 * the window. */
void esc_dc_report(esc_t *chain, int n, FILE *out, double window_s);
void esc_dc_report_totals(esc_t *chain, int n, FILE *out);

/* Continuous (64-bit) system time of a node at the current frame's SOF.
 * Exposed for offline tests. */
int64_t esc_dc_frame_sys(const esc_t *esc);

#endif /* ESC_DC_H */
