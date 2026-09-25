#ifndef ECM_POLICY_H
#define ECM_POLICY_H

#include <stdint.h>
#include <stdatomic.h>

/* ==========================================================================
 * ecm_policy.h — master fault policy (Phase 7.3), pure logic, no SOEM.
 * Design and rationale: docs/fault_policy.md.
 *
 *   - ecm_rx_timeout_us(): receive timeout from the tick deadline (§2)
 *   - ecm_bus_fsm_t:       RUN / DEGRADED / LOST / RECOVER (§3), RT thread only
 *   - ecm_srec_*:          per-slave recovery planner (§4), monitor thread only
 *   - ecm_evring_t / ecm_cmdq_t: SPSC rings RT -> monitor (events) and
 *                          monitor -> RT (commands), C11 acquire/release
 *
 * Everything here is RT-safe: no allocation, no syscalls, no locks.
 * ========================================================================== */

/* WKC classes as numbers, same values as ecm_wkc_class_t in ecm_diag.h
 * (kept separate so this header has no dependency on diag). */
enum { ECM_POL_WKC_OK = 0, ECM_POL_WKC_NOFRAME = 1, ECM_POL_WKC_ZERO = 2,
       ECM_POL_WKC_PARTIAL = 3, ECM_POL_WKC_OVER = 4 };

/* ---- §2: receive timeout from the tick deadline ------------------------- */
/* Microseconds left until deadline_ns, clamped to [min_us, max_us]. */
int ecm_rx_timeout_us(uint64_t now_ns, uint64_t deadline_ns, int min_us, int max_us);

/* ---- events (RT -> monitor) ---------------------------------------------- */
typedef enum {
    ECM_EV_BUS = 1,        /* from -> to, a = reason (WKC class), b = group (0 motion, 1 io) */
    ECM_EV_PROBE,          /* LOST probe: a = BRD WKC                                        */
    ECM_EV_CMD_DONE,       /* a = slave, b = WKC of the command write                         */
    ECM_EV_CMD_SKIPPED,    /* a = slave, b = reason (1 = no budget this tick, retried later)  */
    ECM_EV_FRESH,          /* Giai doan 7.4: a = slave, b = ecm_fresh_ev_t                    */
} ecm_ev_type_t;

typedef struct {
    uint64_t tick;
    uint16_t type;
    uint8_t  from, to;
    int32_t  a, b;
} ecm_event_t;

/* ---- §3: bus state machine ------------------------------------------------ */
typedef enum { ECM_BUS_RUN = 0, ECM_BUS_DEGRADED, ECM_BUS_LOST, ECM_BUS_RECOVER, ECM_BUS_NSTATE } ecm_bus_state_t;

typedef struct {
    uint32_t n_lost;              /* consecutive motion NOFRAME cycles -> LOST (100)   */
    uint32_t probe_ticks;         /* LOST: BRD probe period in ticks (10)               */
    uint32_t recover_backoff_ticks; /* after a failed RECOVER, wait before probing (1000) */
} ecm_bus_cfg_t;

typedef struct {
    ecm_bus_cfg_t   cfg;
    ecm_bus_state_t state;
    uint32_t        noframe_run;   /* current run of motion NOFRAME cycles          */
    int             io_ok;         /* class of the last serviced IO cycle was OK    */
    int             io_last_class; /* that class, for the transition reason         */
    uint64_t        since_tick;    /* tick of the last transition                    */
    uint64_t        next_probe_tick;
    uint64_t        entered[ECM_BUS_NSTATE];   /* transitions INTO each state          */
    uint64_t        ticks_in[ECM_BUS_NSTATE];  /* cycles spent in each state           */
} ecm_bus_fsm_t;

void ecm_bus_default_cfg(ecm_bus_cfg_t *cfg);
void ecm_bus_init(ecm_bus_fsm_t *f, const ecm_bus_cfg_t *cfg);
const char *ecm_bus_state_name(ecm_bus_state_t s);

/* RUN/DEGRADED only: account one motion cycle. io_class = -1 when the IO
 * group was not serviced this tick. Returns 1 and fills *ev on a transition. */
int ecm_bus_on_cycle(ecm_bus_fsm_t *f, uint64_t tick, int motion_class, int io_class, ecm_event_t *ev);

/* LOST only: is a probe due this tick? */
int ecm_bus_probe_due(const ecm_bus_fsm_t *f, uint64_t tick);

/* LOST only: result of a probe (BRD WKC; <= 0 = nobody answered).
 * Returns 1 (transition to RECOVER) when the bus answers. */
int ecm_bus_on_probe(ecm_bus_fsm_t *f, uint64_t tick, int brd_wkc, ecm_event_t *ev);

/* RECOVER only: the monitor finished. ok -> RUN, else back to LOST with backoff. */
int ecm_bus_on_recover_done(ecm_bus_fsm_t *f, uint64_t tick, int ok, ecm_event_t *ev);

/* Count the tick in the current state (call once per tick, any state). */
void ecm_bus_tick(ecm_bus_fsm_t *f);

/* ---- SPSC rings ----------------------------------------------------------- */
#define ECM_EVRING_LEN 256          /* power of two */
typedef struct {
    atomic_uint head;               /* written by producer */
    atomic_uint tail;               /* written by consumer */
    uint64_t    drops;              /* producer only */
    ecm_event_t e[ECM_EVRING_LEN];
} ecm_evring_t;

void ecm_evring_init(ecm_evring_t *r);
int  ecm_evring_push(ecm_evring_t *r, const ecm_event_t *ev);   /* 1 ok, 0 dropped */
int  ecm_evring_pop(ecm_evring_t *r, ecm_event_t *out);         /* 1 got one */

/* A command the RT thread executes for the monitor: one FPWR of a 16-bit
 * register on one slave (AL control writes of recovery path A). */
typedef struct {
    uint16_t slave;                 /* 1-based SOEM index, for the event */
    uint16_t configadr;
    uint16_t reg;
    uint16_t value;
} ecm_cmd_t;

#define ECM_CMDQ_LEN 16             /* power of two */
typedef struct {
    atomic_uint head, tail;
    uint64_t    drops;
    ecm_cmd_t   c[ECM_CMDQ_LEN];
} ecm_cmdq_t;

void ecm_cmdq_init(ecm_cmdq_t *q);
int  ecm_cmdq_push(ecm_cmdq_t *q, const ecm_cmd_t *c);          /* monitor */
int  ecm_cmdq_peek(ecm_cmdq_t *q, ecm_cmd_t *out);              /* RT: look without removing */
void ecm_cmdq_drop_head(ecm_cmdq_t *q);                         /* RT: remove after executing */

/* ---- §4: per-slave recovery planner (monitor only) ------------------------ */
#define ECM_SREC_MAX_SLAVES 64

typedef enum {
    ECM_SACT_NONE = 0,
    ECM_SACT_ACK,          /* path A step 1: AL control = current state | ACK */
    ECM_SACT_OP,           /* path A step 2: AL control = OP                  */
    ECM_SACT_RECONFIG,     /* path B: recover address + reconfigure (blocking, monitor) */
} ecm_sact_t;

typedef struct {
    int      in_reach;     /* slave position < BRD count (not behind a break) */
    int      answered;     /* configured-address read answered (WKC > 0)      */
    uint16_t al_status;    /* 0x0130                                          */
} ecm_srec_input_t;

typedef struct {
    uint32_t   attempts;       /* ACK/RECONFIG actions since the slave was last healthy */
    uint64_t   next_try_ns;
    int        failed;         /* gave up after max_attempts                   */
    int        unhealthy;      /* was not healthy at the last decision         */
    ecm_sact_t last;
    uint64_t   recoveries;     /* times it went unhealthy -> healthy           */
    uint64_t   actions[4];     /* by ecm_sact_t                                */
} ecm_srec_t;

typedef struct {
    int        n;
    uint32_t   max_attempts;   /* 5 */
    uint64_t   backoff0_ns;    /* 1 s, doubled per attempt */
    ecm_srec_t s[ECM_SREC_MAX_SLAVES];
} ecm_srec_plan_t;

void ecm_srec_init(ecm_srec_plan_t *p, int n, uint32_t max_attempts, uint64_t backoff0_ns);

/* Decide for slave i (0-based) from a fresh diagnostic read. *recovered is
 * set to 1 when this read shows it healthy again after being unhealthy.
 * *gave_up is set to 1 on the decision that marks it failed. */
ecm_sact_t ecm_srec_decide(ecm_srec_plan_t *p, int i, const ecm_srec_input_t *in,
                           uint64_t now_ns, int *recovered, int *gave_up);

/* Operator reset of a failed slave (not used automatically). */
void ecm_srec_reset(ecm_srec_plan_t *p, int i);

const char *ecm_sact_name(ecm_sact_t a);


/* ---- Giai doan 7.4 §3.4: input freshness (L5-09, and the L5-07/08 oracle)
 * The slave's PDO contract (NOT libecmaster) puts a counter that the slave
 * application increments every cycle somewhere in its inputs; the caller
 * reads it (offset/size come from application config / ENI) and feeds the
 * value here on every cycle whose reply was accepted.
 *   unchanged for stale_after cycles  -> STALE (WKC is fine, data is not)
 *   went backwards                    -> REGRESSION: an older reply was
 *                                        taken for this cycle's (index reuse)
 * ------------------------------------------------------------------------ */
typedef enum {
    ECM_FRESH_OK = 0,
    ECM_FRESH_FIRST,          /* first sample, nothing to compare        */
    ECM_FRESH_UNCHANGED,      /* same value, not (yet) stale             */
    ECM_FRESH_STALE_START,    /* reached stale_after: report once        */
    ECM_FRESH_STALE,          /* still stale                             */
    ECM_FRESH_RESUMED,        /* changed again after being stale         */
    ECM_FRESH_REGRESSION,     /* value older than the last one accepted  */
} ecm_fresh_ev_t;

typedef struct {
    uint32_t stale_after;     /* cycles without change -> stale          */
    int      bits;            /* 8, 16 or 32                             */
    int      have;
    uint32_t last;
    uint32_t run;             /* consecutive unchanged cycles            */
    int      stale;
    uint64_t samples, stale_episodes, stale_cycles, regressions;
    uint32_t max_run;
} ecm_fresh_t;

void           ecm_fresh_init(ecm_fresh_t *f, uint32_t stale_after, int bits);
ecm_fresh_ev_t ecm_fresh_update(ecm_fresh_t *f, uint32_t value);

#endif /* ECM_POLICY_H */
