/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* ==========================================================================
 * ecm_api.h -- Phase 11.1: remote diagnostics API, stats.json side
 * (docs/api.md, claude/giai_doan_11_ke_hoach.md §1.1, §2).
 *
 * Who does what:
 *   - producers (RT thread, telemetry, monitor) fill a payload struct and
 *     hand it over through an ecm_api_ho_t: two slots, overwrite-free, the
 *     producer NEVER waits (same protocol as ecm_diag_handoff_t, Phase 7.6);
 *   - one consumer, the `api` thread, drains every handoff, builds an
 *     ecm_api_stats_t and writes it as JSON with ecm_api_publish()
 *     (tmp file + rename: a reader sees the old or the new file, never half).
 *
 * Nothing here talks to SOEM or touches the network; the module is tested
 * offline (test_api_offline.c) and under TSan (test_api_tsan.c).
 * ========================================================================== */
#ifndef ECM_API_H
#define ECM_API_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "../telemetry/histogram.h"

#define ECM_API_SCHEMA        1
#define ECM_API_MAX_SLAVES    64
#define ECM_API_MAX_AXES      32
#define ECM_API_MAX_GROUPS    2
#define ECM_API_MAX_PORTS     2       /* v1.0 publishes one; Phase 13 adds the secondary */
#define ECM_API_MAX_FINDINGS  32
#define ECM_API_EMCY_KEEP     4
#define ECM_API_DC_BINS       1001    /* = ecm_run DC_HIST_US: 1 us bins, last = overflow */
#define ECM_API_HIST_BINS     (HIST_NUM_BUCKETS + 1)

/* ---- two-slot handoff ------------------------------------------------------
 * Producer: ecm_api_ho_put(), or ecm_api_ho_begin() + fill + ecm_api_ho_commit()
 * (no second copy for large payloads). begin() returns NULL when both slots
 * are still full: the producer drops this update (drops++) and goes on.
 * Consumer: ecm_api_ho_get() returns the OLDER full slot first; call it until
 * it returns 0 to end with the newest. One producer, one consumer. */
typedef struct {
    atomic_int state[2];        /* 0 free, 1 full                                */
    uint64_t   put_seq[2];      /* written before the release of state[k]        */
    uint64_t   puts, drops;     /* producer side only                            */
    int        next_put;        /* producer side only                            */
    int        open;            /* producer side only: slot of an open begin()   */
    size_t     size;
    void      *slot[2];         /* caller storage, `size` bytes each             */
} ecm_api_ho_t;

void  ecm_api_ho_init(ecm_api_ho_t *h, void *slot0, void *slot1, size_t size);
void *ecm_api_ho_begin(ecm_api_ho_t *h);           /* NULL: both full, dropped    */
void  ecm_api_ho_commit(ecm_api_ho_t *h);
int   ecm_api_ho_put(ecm_api_ho_t *h, const void *src);   /* 1 ok, 0 dropped     */
int   ecm_api_ho_get(ecm_api_ho_t *h, void *dst);         /* 1 got one, 0 empty  */

/* ---- histograms as published ------------------------------------------------ */
typedef struct {                /* ecm_hist_t without its bound table           */
    int      have;
    uint64_t count, sum_ns;
    int64_t  min_ns, max_ns;
    uint64_t b[ECM_API_HIST_BINS];
} ecm_api_hist_t;

void ecm_api_hist_from(ecm_api_hist_t *o, const ecm_hist_t *h);

typedef struct {                /* DC |e|: bins of 1 us, last bin = overflow    */
    int      have;
    uint64_t n;
    uint64_t b[ECM_API_DC_BINS];
} ecm_api_linhist_t;

/* ---- producer payloads ------------------------------------------------------ */
typedef struct {                /* per process-data group, RT thread             */
    uint64_t cycles, overrun, mismatch;
    uint64_t wkc[5];            /* by ecm_wkc_class_t: OK NOFRAME ZERO PARTIAL OVER */
    uint32_t max_run_bad;
} ecm_api_rt_group_t;

typedef struct {                /* H3 scalars, RT thread, 10 Hz                  */
    uint64_t seq, mono_ns, tick;
    int      bus_state;         /* ecm_bus_state_t                               */
    int      ngroups;
    ecm_api_rt_group_t g[ECM_API_MAX_GROUPS];
    int      dc_on, dc_state;
    uint64_t dc_locks, dc_unlocks, dc_clamps, dc_wraps, dc_stale;
    int64_t  dc_err_ns;
    int      txtime_have;
    uint64_t txtime_late, txtime_bypass, send_err, tai_steps;
    uint64_t hook_calls;
    uint64_t ho_drops;          /* drops of the three RT handoffs so far         */
} ecm_api_rt_t;

typedef struct {                /* H3 histograms, RT thread, 1 Hz each           */
    uint64_t seq, mono_ns;
    ecm_api_linhist_t h;
} ecm_api_rt_dc_t;

typedef struct {
    uint64_t seq, mono_ns;
    ecm_api_hist_t h;
} ecm_api_rt_hook_t;

typedef struct {                /* H1, telemetry thread, 10 Hz                   */
    uint64_t seq, mono_ns;
    ecm_api_hist_t wake_jitter, prep_send, occupancy, turnaround;
    uint64_t ring_drops;
    uint64_t ho_drops;          /* drops of this handoff so far                  */
} ecm_api_tel_t;

typedef struct {
    uint64_t mono_ns, tick;
    uint16_t code;
    uint8_t  reg, data[5];
} ecm_api_emcy_t;

typedef struct {                /* one slave, monitor view                       */
    uint16_t station, al_status, al_code, dl_status;
    int      answered, ever_answered;
    uint64_t inv[4], rx[4], fwd[4], lost[4], pu, pdi;
    uint64_t emcy_count;
    int      nemcy;             /* newest first                                  */
    ecm_api_emcy_t emcy[ECM_API_EMCY_KEEP];
    int      sact;              /* ecm_sact_t of the last decision               */
    int      unhealthy, failed, failed_config;
    uint16_t failed_code;
    uint32_t attempts;
    uint64_t recoveries, initcmd_failed;
} ecm_api_mon_slave_t;

typedef struct {
    char     type[24];          /* stable key (ecm_api_finding_key)              */
    char     sev[8];            /* info | warn | error                           */
    int      a, b;
    uint32_t c;
    uint64_t count;
    int      active;
    char     text[200];
} ecm_api_finding_t;

typedef struct {                /* H2, monitor thread, 10 Hz                     */
    uint64_t seq, mono_ns;
    int      diag_on;
    uint64_t diag_reads, diag_frames_lost, emcy_total, emcy_lost;
    int      n;
    ecm_api_mon_slave_t s[ECM_API_MAX_SLAVES];
    int      nf;
    ecm_api_finding_t f[ECM_API_MAX_FINDINGS];
    uint64_t ho_drops;          /* drops of this handoff so far                  */
} ecm_api_mon_t;

/* ---- the document --------------------------------------------------------- */
typedef struct {
    char     name[12], iface[32];
    int      link;              /* 1 up, 0 down, -1 unknown                      */
    uint64_t tx, noframe;
    int64_t  crc_err, rx_missed, carrier_changes;   /* -1: not available         */
} ecm_api_port_t;

typedef struct {
    char     name[12];
    int      nslaves;
    uint16_t slaves[ECM_API_MAX_SLAVES];
    int64_t  cycle_ns;
} ecm_api_group_cfg_t;

typedef struct {
    int      pos;
    uint32_t vendor, product, revision;
    char     name[48];
} ecm_api_slave_cfg_t;

typedef struct {
    char     name[24];
    int      slave, n;
    char     state[24], err[16], error_src[8];   /* error_src: 603F | EMCY | none */
    uint16_t sw, cw, error_code;
    int      mode, mode_display, homed, valid;
    int32_t  pos, vel;
    uint64_t tick, used, late, underruns, dropped;
    uint32_t step_refused, pp_acked;
    char     error_text[160];
} ecm_api_axis_t;

/* Phase 11.2+ (plan §2.3, 11.3): the RT configuration of the machine, read
 * once at start-up so a benchmark or a bug report says what it ran on. */
#define ECM_API_RT_FLAGS   16
#define ECM_API_RT_STATES  8
typedef struct {
    int  rt_cpu;                          /* the RT thread's core (3)                  */
    char isolated[64];                    /* /sys/devices/system/cpu/isolated, "" none  */
    int  have_isolated;                   /* the file was readable                     */
    char idle_driver[32];                 /* cpuidle/current_driver, "" = none/absent  */
    int  idle_off;                        /* cpuidle.off=1, or no cpuidle driver       */
    int  nstates;                         /* idle states of rt_cpu                     */
    int  ndisabled;
    char disabled[ECM_API_RT_STATES][24]; /* names of the disabled ones                */
    char kernel[96];                      /* uname -r                                  */
    int  nflags;
    char flags[ECM_API_RT_FLAGS][64];     /* RT-relevant /proc/cmdline words only      */
} ecm_api_rtcfg_t;

/* sysroot "/sys", cmdline "/proc/cmdline", kernel NULL = uname(). Missing
 * files leave the field empty/0; never fails. */
void ecm_api_rtcfg_read(const char *sysroot, const char *cmdline, const char *kernel, int rt_cpu,
                        ecm_api_rtcfg_t *o);
/* The JSON members (no braces, no leading comma) of the rt object:
 * "rt_cpu":3,"isolated_cpus":"3","cpuidle":{...},"kernel":"...","cmdline_flags":[...] */
size_t ecm_api_rtcfg_json(const ecm_api_rtcfg_t *r, char *buf, size_t cap);

typedef struct {
    /* fixed after start-up (ecm_run fills these once) */
    int      pid;
    int64_t  cycle_ns;
    char     link[12];          /* af_packet | etf                               */
    int      n_slaves, nports, ngroups;
    char     path[12];          /* single (v1.0)                                 */
    ecm_api_port_t      ports[ECM_API_MAX_PORTS];
    ecm_api_group_cfg_t gcfg[ECM_API_MAX_GROUPS];
    ecm_api_slave_cfg_t scfg[ECM_API_MAX_SLAVES];
    int      dc_ref_pos;
    int      dma_latency_held;
    char     hook[16];          /* "" = no hook                                  */
    char     api_cpus[24];
    ecm_api_rtcfg_t rtcfg;
    /* per publish */
    uint64_t pub_seq, mono_ns;
    int      final;
    char     fsm[12];           /* from the atomic bus state                     */
    int      have_rt, have_tel, have_mon, have_dc, have_hook;
    ecm_api_rt_t      rt;
    ecm_api_rt_dc_t   dc;
    ecm_api_rt_hook_t hook_h;
    ecm_api_tel_t     tel;
    ecm_api_mon_t     mon;
    uint64_t etf_missed, etf_invalid;
    int      naxes;
    ecm_api_axis_t ax[ECM_API_MAX_AXES];
} ecm_api_stats_t;

/* Stable keys (they are part of the schema: never renamed, only added). */
const char *ecm_api_bus_key(int bus_state);      /* RUN DEGRADED LOST RECOVER        */
const char *ecm_api_dc_key(int dc_state);        /* UNANCHORED ACQUIRE LOCKED        */
const char *ecm_api_sact_key(int sact);          /* none ack op reconfig             */
const char *ecm_api_finding_key(int type);       /* chain_broken ... emcy_lost       */
const char *ecm_api_sev_key(int sev);            /* info warn error                  */
const char *ecm_api_al_key(uint16_t al_status);  /* INIT PREOP BOOT SAFEOP OP ?      */
const char *ecm_api_recover_key(const ecm_api_mon_slave_t *s);   /* ok retrying failed failed_config */

/* JSON of the whole document into buf; returns the length, or 0 when buf
 * was too small (nothing usable written). */
size_t ecm_api_stats_json(const ecm_api_stats_t *st, char *buf, size_t cap);

/* Write dir/name atomically: dir/.name.tmp, then rename(). 0 ok, -errno. */
int ecm_api_publish(const char *dir, const char *name, const char *buf, size_t len);

/* mkdir -p of one level (the parent must exist); 0 ok or exists, -errno. */
int ecm_api_mkdir(const char *dir, unsigned mode);

/* NIC counters from sysfs: <sysroot>/class/net/<if>/... (sysroot "/sys").
 * Fields that cannot be read are set to -1 (link: -1). */
void ecm_api_nic_read(const char *sysroot, const char *ifname, ecm_api_port_t *p);

/* CPU list "1-2", "1,2", "2": bit i of *mask = CPU i (CPUs 0..63).
 * 0 ok, -1 syntax error or empty. */
int ecm_api_parse_cpus(const char *s, uint64_t *mask);

#endif /* ECM_API_H */
