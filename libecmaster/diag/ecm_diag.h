#ifndef ECM_DIAG_H
#define ECM_DIAG_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* ==========================================================================
 * ecm_diag.h — bus diagnostics model (Phase 7.2). No SOEM dependency: the
 * SOEM glue (ecm_diag_soem.h) fills ecm_diag_raw_t, everything here is pure
 * logic and is unit-tested offline (test_diag_offline.c).
 *
 * Data flow in ecm_run (plan §3.1/§3.2):
 *   RT thread : sends one multi-datagram frame per second, collects the
 *               reply on a later tick (never blocks), fills an
 *               ecm_diag_raw_t, hands it over with ecm_diag_handoff_put().
 *   monitor   : ecm_diag_handoff_get() -> ecm_diag_ingest() ->
 *               ecm_diag_analyze() -> ecm_diag_format() -> snapshot file.
 *   ecm_diag  : CLI that prints the snapshot file.
 *
 * Numbering: slave numbers are SOEM's, 1-based, in chain order. Array index
 * i holds slave i+1. (soft_bus "node k" is slave k+1.)
 *
 * Error counters (ESC datasheet Section I §14, Section II §2.9), read as one
 * 20-byte block 0x0300..0x0313 per slave:
 *   0x0300+2p invalid frame, 0x0301+2p RX error   -> error FIRST seen here
 *   0x0308+p  forwarded error                     -> marked by an ESC before
 *   0x030C    processing unit, 0x030D PDI, 0x0310+p lost link
 * ecm_run reads them with FPRW: the ESC returns the old values and the
 * write clears them (write value ignored), in the same datagram. So every
 * read is a delta and nothing is lost between "read" and "clear". A value
 * of 0xFF means the counter saturated inside one read period: the count
 * is a lower bound.
 * ========================================================================== */

#define ECM_DIAG_MAX_SLAVES   64
#define ECM_DIAG_MAX_GROUPS   2
#define ECM_DIAG_ERR_BASE     0x0300
#define ECM_DIAG_ERR_LEN      20      /* 0x0300..0x0313 */
#define ECM_DIAG_PORTS        4

/* ---- WKC classification (per process data group) ---------------------- */
typedef enum {
    ECM_WKC_OK = 0,
    ECM_WKC_NOFRAME,     /* no reply at all (timeout)                      */
    ECM_WKC_ZERO,        /* reply came back, nobody processed it           */
    ECM_WKC_PARTIAL,     /* 0 < wkc < expected: some slave(s) missing      */
    ECM_WKC_OVER,        /* wkc > expected: wrong config or duplicate      */
    ECM_WKC_NCLASS
} ecm_wkc_class_t;

typedef struct {
    uint64_t count[ECM_WKC_NCLASS];
    int32_t  last_missing;       /* expected - wkc of the last PARTIAL        */
    uint32_t run_bad;            /* current run of consecutive non-OK cycles  */
    uint32_t max_run_bad;
} ecm_wkc_stats_t;

ecm_wkc_class_t ecm_wkc_classify(int wkc, int expected);
/* RT-safe (no syscalls). Single writer. */
void ecm_wkc_account(ecm_wkc_stats_t *st, int wkc, int expected);
const char *ecm_wkc_class_name(ecm_wkc_class_t c);

/* ---- raw data of one diagnostic read ------------------------------------ */
typedef struct {
    uint16_t station_addr;       /* configured address (FPRD mode) or the one
                                  * read from 0x0010 (position mode)          */
    int16_t  wkc_err;            /* WKC of the 0x0300 datagram: FPRW 3, FPRD 1 */
    int16_t  wkc_dl;
    int16_t  wkc_al;
    uint8_t  err[ECM_DIAG_ERR_LEN];
    uint16_t dl_status;          /* 0x0110 */
    uint16_t al_status;          /* 0x0130 */
    uint16_t al_code;            /* 0x0134 */
} ecm_diag_raw_slave_t;

typedef struct {
    uint64_t seq;                /* epoch number, set by the producer        */
    uint64_t t_ns;               /* CLOCK_MONOTONIC of the read              */
    int      frame_ok;           /* 0: the diagnostic frame itself was lost  */
    int      clear_on_read;      /* 1: counters were cleared by this read    */
    int      n;                  /* slaves configured                        */
    int      first, count;       /* slaves [first, first+count) are in s[]   */
    int      brd_count;          /* WKC of BRD 0x0130 = slaves answering     */
    uint16_t brd_al_or;          /* OR of all AL status                      */
    uint64_t frames_lost;        /* diagnostic frames lost so far            */
    uint64_t handoff_drops;      /* producer's ecm_diag_handoff_t.drops       */
    int      ngroups;
    ecm_wkc_stats_t wkc[ECM_DIAG_MAX_GROUPS];   /* copied by the RT thread */
    ecm_diag_raw_slave_t s[ECM_DIAG_MAX_SLAVES];
} ecm_diag_raw_t;

/* ---- RT thread -> monitor handoff ---------------------------------------
 * Two slots, each owned either by the producer (state 0) or the consumer
 * (state 1), switched with release/acquire. No seqlock: a seqlock copies
 * while the writer may be writing, which is a data race by the C11 memory
 * model and would be flagged by TSan (7.6). Producer never blocks: if both
 * slots are full, the NEW epoch is dropped and counted (a full slot may be
 * being read). The consumer gets epochs in increasing order (7.6 fix in
 * ecm_diag_handoff_get), never a torn one; tests/tsan/test_spsc_stress.c. */
typedef struct {
    atomic_int     state[2];
    uint64_t       put_seq[2];
    uint64_t       drops;        /* producer side only */
    uint64_t       puts;         /* producer side only */
    int            next_put;     /* producer side only */
    ecm_diag_raw_t slot[2];
} ecm_diag_handoff_t;

void ecm_diag_handoff_init(ecm_diag_handoff_t *h);
int  ecm_diag_handoff_put(ecm_diag_handoff_t *h, const ecm_diag_raw_t *raw);   /* 1 ok, 0 dropped */
int  ecm_diag_handoff_get(ecm_diag_handoff_t *h, ecm_diag_raw_t *out);          /* 1 got one */

/* ---- accumulated model ---------------------------------------------------- */
typedef struct {
    uint64_t inv[ECM_DIAG_PORTS], rx[ECM_DIAG_PORTS], fwd[ECM_DIAG_PORTS];
    uint64_t pu, pdi, lost[ECM_DIAG_PORTS];
} ecm_diag_counts_t;

typedef struct {
    ecm_diag_counts_t tot;       /* since start                              */
    ecm_diag_counts_t last;      /* delta of the most recent read            */
    uint8_t  prev[ECM_DIAG_ERR_LEN];  /* passive mode: last raw values       */
    int      have_prev;
    uint64_t saturated;          /* reads where some counter was 0xFF        */
    int      answered;           /* last read: FPRD/FPRW WKC > 0             */
    int      ever_answered;
    uint64_t no_answer;          /* reads without an answer                  */
    uint16_t station_addr, dl_status, al_status, al_code;
    uint64_t seen_read;          /* d->reads value when last included        */
} ecm_diag_slave_t;

typedef struct {
    int      n;
    uint16_t expected_state;     /* 0 = don't judge AL state, only ERR bit   */
    uint64_t reads, frames_lost, handoff_drops;
    uint64_t last_seq, last_t_ns;
    int      brd_count;
    uint16_t brd_al_or;
    int      ngroups;
    ecm_wkc_stats_t wkc[ECM_DIAG_MAX_GROUPS];
    ecm_diag_slave_t s[ECM_DIAG_MAX_SLAVES];
} ecm_diag_t;

void ecm_diag_init(ecm_diag_t *d, int n, uint16_t expected_state);
void ecm_diag_ingest(ecm_diag_t *d, const ecm_diag_raw_t *raw);

/* ---- findings ------------------------------------------------------------- */
typedef enum {
    ECM_FIND_CHAIN_BROKEN,       /* a = last slave answering, link down behind it */
    ECM_FIND_MISSING_LINK_UP,    /* a = last slave answering, but port 1 has link */
    ECM_FIND_NO_ADDRESS,         /* a = slave on the bus that ignores its address */
    ECM_FIND_CABLE_FWD,          /* a = slave, fault between a-1 (or master) and a */
    ECM_FIND_CABLE_RET,          /* a = slave, fault on its port 1 (return path)   */
    ECM_FIND_FWD_NO_ORIGIN,      /* forwarded errors whose origin was not read     */
    ECM_FIND_PU_ONLY,            /* a = slave, processing unit errors only         */
    ECM_FIND_PDI,                /* a = slave, PDI errors (uC interface)           */
    ECM_FIND_LOST_LINK,          /* a = slave, b = port                            */
    ECM_FIND_STATE,              /* a = slave, b = AL status, c = AL status code   */
    ECM_FIND_SATURATED,          /* a = slave                                      */
    ECM_FIND_DIAG_LOST,          /* diagnostic frames lost                         */
} ecm_find_type_t;

typedef enum { ECM_SEV_INFO = 0, ECM_SEV_WARN, ECM_SEV_ERROR } ecm_sev_t;

typedef struct {
    ecm_find_type_t type;
    ecm_sev_t       sev;
    int             a, b;
    uint32_t        c;
    uint64_t        count;       /* total events behind the finding          */
    int             active;      /* seen in the most recent read             */
} ecm_diag_finding_t;

/* Fills out[] (most severe first), returns how many. */
int ecm_diag_analyze(const ecm_diag_t *d, ecm_diag_finding_t *out, int max);

/* One line of text for a finding (no newline). al_str may be NULL (then
 * only the numeric AL status code is printed); ecm_run passes SOEM's
 * ec_ALstatuscode2string. */
typedef const char *(*ecm_al_str_fn)(uint16_t code);
size_t ecm_diag_finding_str(const ecm_diag_finding_t *f, ecm_al_str_fn al_str, char *buf, size_t cap);

/* Whole snapshot: header line, bus/WKC lines, per-slave table, findings.
 * group_names may be NULL. Returns length written (always 0-terminated). */
size_t ecm_diag_format(const ecm_diag_t *d, const ecm_diag_finding_t *f, int nf,
                       const char *const *group_names, const char *source,
                       ecm_al_str_fn al_str, char *buf, size_t cap);

/* Write text atomically: <path>.tmp then rename(). 0 on success. */
int ecm_diag_write_file(const char *path, const char *text, size_t len);

/* Parse "t_mono=<seconds>" from a snapshot header; -1.0 if absent. */
double ecm_diag_snapshot_time(const char *text);

#endif /* ECM_DIAG_H */
