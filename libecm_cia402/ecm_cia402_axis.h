/* ==========================================================================
 * ecm_cia402_axis.h — Phase 10.5: master-side CiA402 axis, run in the RT hook.
 *
 * Per axis and per tick (hook of tick k, outputs go out in frame k+1):
 *   1. inputs  statusword 0x6041 -> drive state (CiA 402 masks), position /
 *              velocity actual, error code, mode display
 *   2. latch   bus lost -> target DISABLED, error BUS_LOST (stays until a new
 *              ENABLE command); drive Fault while ENABLED -> error FAULT
 *   3. command from the command ring: ENABLE, DISABLE, QUICKSTOP,
 *              FAULT_RESET, SET_MODE (owner ops of libecmaster/xchg)
 *   4. control the controlword walks the drive one transition per tick:
 *              ENABLED:  SOD -0x06-> RTSO -0x07-> SO -0x0F-> OE, the last
 *                        step only once 0x6061 shows the requested mode and
 *                        with target = actual (CSP) / 0 (CSV) in that frame
 *              DISABLED: OE -0x07-> SO -0x06-> RTSO -0x00-> SOD
 *              QUICKSTOP: 0x02 until the drive leaves Operation enabled
 *              FAULT_RESET: one 0x80 pulse (rising edge), then 0x00 --
 *                        the axis stays disabled afterwards
 *   5. timeout a commanded step that does not happen within step_timeout
 *              -> error TIMEOUT naming the state the drive is stuck in,
 *              target DISABLED (no retry loop)
 *   6. setpoints (CSP: position, CSV: velocity) from the axis' ring, keyed
 *              by tick like libecmaster/xchg; only used while Operation
 *              enabled with the mode confirmed, else dropped (counted).
 *              Underrun: CSP holds the last position, CSV commands 0.
 *              Not enabled: CSP target follows actual, CSV target 0.
 *   7. state   one seqlock record per axis (ecm_cia402_state_t)
 *
 * The master never enables an axis by itself: target ENABLED only comes
 * from an ENABLE command. RT-safe: no syscalls, locks or allocation.
 * Modes handled here: CSP, CSV (10.5), PP, PV, HM (10.7).
 *
 * Phase 10.7, profile modes (commands, not a setpoint stream):
 *   PP  PP_POINT target [flags: 1 relative, 2 change immediately]: queued
 *       (ECM_CIA402_PP_QUEUE). One point at a time: 0x607A + controlword
 *       bit 4 (new set-point) once statusword bit 12 (set-point acknowledge)
 *       is low and, unless "immediately", bit 10 (target reached) is high;
 *       bit 4 cleared on bit 12 high. No point is lost or overwritten.
 *       Ack timeout = step timeout -> error HANDSHAKE.
 *   PV  PV_VEL v: 0x60FF = v while Operation enabled in PV (the drive
 *       ramps with 0x6083/0x6084); reset to 0 when the axis stops running.
 *   HM  HOME: controlword bit 4 until statusword bit 12 (homing attained)
 *       + bit 10, or bit 13 (homing error) -> error HOMING, or the homing
 *       timeout. Bit 12 must reflect this start: read from the second tick
 *       after the start and, if it never went low (methods 35/37 finish at
 *       once), from ECM_CIA402_HM_SETTLE ticks on.
 * Profile parameters (0x6081/0x6083/0x6084, 0x6098/0x6099/0x609A/0x607C)
 * are SDO objects: set by the caller at PREOP (ecm_run --axis-pp,
 * --axis-homing). S4 applies to the cyclic modes; PP / PV / HM motion is
 * bounded by the drive's profile parameters.
 *
 * Phase 10.6, safety latches (docs/safety_boundary.md):
 *   S1  never enabled by the master: an axis that leaves Operation enabled
 *       without a command (recovery, drive-side quick stop / DI) is latched
 *       disabled (DROPPED), it is not walked back up
 *   S2  target = actual before and while enabling (CSP)
 *   S3  setpoint underrun: CSP holds the last position (no extrapolation)
 *   S4  step limit: |setpoint - last sent| > max step per cycle -> not sent,
 *       quick stop, error STEP (CSP position; CSV velocity if set)
 *   S5  bus lost -> disabled, kept when the bus is back (BUS_LOST)
 *   S6  shutdown: ecm_cia402_shutdown() walks every axis down (0x07, 0x06,
 *       0x00) and refuses ENABLE; the caller keeps cycling until
 *       ecm_cia402_all_down() before it leaves OP
 *   S7  fault reset only on command; the axis stays disabled after it
 * ECM_CIA402_BROKEN (bit n = Sn) switches a latch off: test builds only,
 * for the negative controls. Never set it in a product build. 10.7 adds
 * bit 8 (PP without the set-point handshake) and bit 9 (homing done on a
 * statusword that may predate the start).
 * ========================================================================== */
#ifndef ECM_CIA402_AXIS_H
#define ECM_CIA402_AXIS_H

#include <stdint.h>

#include "ecm_cia402_cfg.h"
#include "../libecmaster/xchg/ecm_xchg.h"

/* drive state as read from the statusword */
typedef enum {
    ECM_DS_UNKNOWN = 0, ECM_DS_NOT_READY, ECM_DS_SOD, ECM_DS_RTSO, ECM_DS_SO,
    ECM_DS_OE, ECM_DS_QSA, ECM_DS_FRA, ECM_DS_FAULT, ECM_DS_COUNT
} ecm_ds_t;

enum { ECM_TGT_DISABLED = 0, ECM_TGT_ENABLED, ECM_TGT_QUICKSTOP };

enum {
    ECM_AXERR_NONE = 0,
    ECM_AXERR_TIMEOUT,        /* a commanded transition did not happen       */
    ECM_AXERR_FAULT,          /* the drive went to Fault (0x603F in the state) */
    ECM_AXERR_BUS_LOST,       /* process data lost while enabled             */
    ECM_AXERR_MODE,           /* mode not configured / not handled           */
    ECM_AXERR_IN_FAULT,       /* ENABLE refused: drive in Fault, reset first */
    ECM_AXERR_DROPPED,        /* S1: left Operation enabled without a command */
    ECM_AXERR_STEP,           /* S4: setpoint step above the limit            */
    ECM_AXERR_SHUTDOWN,       /* S6: ENABLE refused, the master is stopping   */
    ECM_AXERR_HANDSHAKE,      /* PP: set-point not acknowledged in time       */
    ECM_AXERR_HOMING,         /* HM: homing error (bit 13) or homing timeout  */
};

#define ECM_CIA402_PP_QUEUE   16     /* PP points waiting, per axis          */
#define ECM_CIA402_HM_SETTLE  10     /* ticks, see HM above                  */
enum { ECM_PP_REL = 1, ECM_PP_IMM = 2 };
enum { ECM_HM_IDLE = 0, ECM_HM_RUNNING, ECM_HM_DONE, ECM_HM_FAILED };

/* Phase 10.6: negative controls (test builds only) */
#ifndef ECM_CIA402_BROKEN
#define ECM_CIA402_BROKEN 0
#endif
#define ECM_CIA402_LATCH_OFF(n) (((ECM_CIA402_BROKEN) >> (n)) & 1)

/* commands (ecm_xcmd_t.op), arg0 = mode for SET_MODE */
enum {
    ECM_CIA_OP_ENABLE = ECM_XOP_USER + 1,
    ECM_CIA_OP_DISABLE,
    ECM_CIA_OP_QUICKSTOP,
    ECM_CIA_OP_FAULT_RESET,
    ECM_CIA_OP_SET_MODE,
    ECM_CIA_OP_PP_POINT,      /* arg0 target, arg1 ECM_PP_* flags (10.7)   */
    ECM_CIA_OP_PV_VEL,        /* arg0 velocity                             */
    ECM_CIA_OP_HOME,          /* start homing (method set by SDO)          */
};

/* CiA 402 modes of operation (0x6060 values) */
enum { ECM_OPMODE_PP = 1, ECM_OPMODE_PV = 3, ECM_OPMODE_HM = 6, ECM_OPMODE_CSP = 8, ECM_OPMODE_CSV = 9, ECM_OPMODE_CST = 10 };

typedef struct {
    ecm_axis_cfg_t  cfg;
    ecm_axis_bind_t b;
    /* RT state */
    ecm_ds_t ds, ds_wait;
    uint16_t sw, cw;
    uint8_t  target, err, err_ds, reset_pulse, armed, mode_ok;
    int8_t   mode_req, mode_disp;
    int32_t  apos, avel;
    uint16_t ecode;
    int64_t  last_sp, last_delta;   /* last_delta: S3 negative control only */
    int64_t  last_sent;            /* S4: what went out last (pos or vel)  */
    int64_t  max_step_pos, max_step_vel;   /* S4, per cycle; 0 = off        */
    int64_t  step_seen;            /* S4: the refused step                 */
    uint8_t  reached_oe;           /* S1: OE reached under this ENABLE     */
    /* 10.7 */
    int64_t  pp_q[ECM_CIA402_PP_QUEUE];
    uint8_t  pp_qf[ECM_CIA402_PP_QUEUE];
    uint8_t  pp_head, pp_n, pp_phase, pp_flags; /* phase 0 idle, 1 bit 4 up, 2 wait ack low */
    int64_t  pp_cur;
    uint64_t pp_since, pp_sent, pp_acked, pp_lost;
    int64_t  pv_vel;
    uint8_t  hm_phase, hm_seen_low, homed;
    uint8_t  running;              /* last tick: OE, ENABLED, mode confirmed */
    uint64_t hm_since;
    uint64_t step_since;
    uint64_t used, late, underrun, dropped, transitions, step_refused;
    uint32_t last_seq;
} ecm_cia402_axis_t;

typedef struct {
    uint64_t tick;
    uint16_t sw, cw, ecode;
    uint8_t  ds, target, err, err_ds, mode_ok;
    int8_t   mode_disp, mode_req;
    int32_t  apos, avel;
    uint64_t used, late, underrun, dropped;
    uint32_t last_seq, step_refused;
    uint8_t  homed, hm_phase, pp_phase, pp_n;     /* 10.7 */
    uint32_t pp_acked;
} ecm_cia402_state_t;

typedef struct {
    int               naxes;
    uint64_t          cycle_ns;
    uint32_t          step_timeout_ticks;
    uint32_t          home_timeout_ticks;   /* 10.7, default 30 s */
    ecm_cia402_axis_t ax[ECM_AXIS_MAX];
    ecm_xring_t       cmd;
    ecm_xring_t       sp[ECM_AXIS_MAX];
    ecm_xst_t         st[ECM_AXIS_MAX];
    ecm_xst_t         clock;              /* tick, t_send_ns, in_valid, bus_lost */
    uint64_t          cmd_applied, cmd_deferred, cmd_bad;
    uint8_t           shutdown;           /* S6, RT thread only */
} ecm_cia402_t;

/* statusword -> drive state (CiA 402, bits 0..3, 5, 6) */
ecm_ds_t    ecm_cia402_decode(uint16_t sw);
const char *ecm_cia402_ds_str(ecm_ds_t ds);
const char *ecm_cia402_err_str(int err);

/* Not RT. step_timeout_ms: per commanded transition (plan: 500 ms). */
void ecm_cia402_init(ecm_cia402_t *c, uint64_t cycle_ns, uint32_t step_timeout_ms);
/* The first mode of cfg->modes (CSP before CSV) is the initial request. -1 if full. */
int  ecm_cia402_add_axis(ecm_cia402_t *c, const ecm_axis_cfg_t *cfg, const ecm_axis_bind_t *b);
/* Not RT. S4 limits per cycle (CSP: increments, CSV: velocity units); 0 = off. */
void ecm_cia402_set_step_limit(ecm_cia402_t *c, int axis, int64_t max_pos, int64_t max_vel);
/* Not RT. Homing timeout (10.7), every axis. */
void ecm_cia402_set_home_timeout(ecm_cia402_t *c, uint32_t ms);
/* CiA 402 mode value (ECM_OPMODE_*) <-> name ("PP" ...) */
const char *ecm_cia402_mode_str(int m);

/* S6, RT thread (the caller of ecm_cia402_rt): from now on every axis is
 * walked down and ENABLE is refused. ecm_cia402_all_down(): 1 once no axis
 * is above Switch on disabled (Fault / unknown count as down; with the bus
 * lost nothing can be sent, so every axis counts as down). */
void ecm_cia402_shutdown(ecm_cia402_t *c);
int  ecm_cia402_all_down(const ecm_cia402_t *c, int bus_lost);

/* RT hook, once per tick. */
void ecm_cia402_rt(ecm_cia402_t *c, uint8_t *iomap, uint64_t tick, uint64_t t_send_ns,
                   int in_valid, int bus_lost);

/* Application side (one producer thread). 0 ok, -1 full / bad axis. */
int  ecm_cia402_cmd(ecm_cia402_t *c, int axis, int op, int64_t arg, uint32_t seq, uint64_t tick);
/* 10.7: with a second argument (PP_POINT flags) */
int  ecm_cia402_cmd2(ecm_cia402_t *c, int axis, int op, int64_t arg0, int64_t arg1, uint32_t seq, uint64_t tick);
int  ecm_cia402_setpoint(ecm_cia402_t *c, int axis, uint64_t tick, int64_t v);
/* Reader side. 0 ok, -1 torn every try. */
int  ecm_cia402_read(ecm_cia402_t *c, int axis, ecm_cia402_state_t *s);
int  ecm_cia402_read_clock(ecm_cia402_t *c, uint64_t *tick, uint64_t *t_send_ns);

#endif /* ECM_CIA402_AXIS_H */
