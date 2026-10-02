/* ==========================================================================
 * esc_cia402.h — Phase 10.2: a virtual CiA402 drive inside a soft_bus node.
 *
 * Written from the specification (IEC 61800-7-201 / CiA 402: power drive
 * state machine, controlword / statusword, CSP, CSV, PP, PV, homing), NOT
 * from what the master does, so tests against it are not circular. Timings
 * and vendor codes have defaults here and are calibrated against the real
 * IS620N in Phase 10.0 (A-05) through esc_cia402_cal_t.
 *
 * Attach: soft_bus --profile N=FILE --cia402 N[:AXES]. The node must have a
 * profile: the drive reads and writes objects by (index, sub) through the
 * PDOs currently assigned (0x1C12/0x1C13 -> mapping objects) and through
 * the profile's object dictionary, so any mapping works (P1 0x1600/0x1A00,
 * IS620N 0x1701/0x1B01 or 0x1702/0x1B02). Axis a uses index + 0x800 * a
 * (ETG.6010 multi-axis).
 *
 * Step: once per frame that carries process data, AFTER the ESC processed
 * it (soft_bus_main.c). Outputs the master just wrote are consumed, inputs
 * are written for the master's NEXT read: one cycle from command to state,
 * like a drive sampling at SYNC0. dt = SYNC0 cycle time (0x09A0) when set,
 * else the measured frame interval (clamped 100 us .. 10 ms).
 *
 * Pure logic, no sockets: unit tested offline (test_cia402.c).
 * ========================================================================== */
#ifndef ESC_CIA402_H
#define ESC_CIA402_H

#include <stdint.h>
#include <stdio.h>

#include "esc_types.h"

#define CIA402_MAX_AXES  8
#define CIA402_AXIS_OFF  0x800u

/* power drive state machine (CiA 402 Figure "state machine") */
typedef enum {
    DS_NOT_READY = 0,      /* Not ready to switch on            */
    DS_SOD,                /* Switch on disabled                */
    DS_RTSO,               /* Ready to switch on                */
    DS_SO,                 /* Switched on                       */
    DS_OE,                 /* Operation enabled                 */
    DS_QSA,                /* Quick stop active                 */
    DS_FRA,                /* Fault reaction active             */
    DS_FAULT,              /* Fault                             */
    DS_COUNT
} cia402_ds_t;

/* modes of operation (0x6060 / 0x6061) */
enum {
    MODE_NONE = 0, MODE_PP = 1, MODE_VL = 2, MODE_PV = 3, MODE_TQ = 4,
    MODE_HM = 6, MODE_IP = 7, MODE_CSP = 8, MODE_CSV = 9, MODE_CST = 10
};

/* statusword bits */
#define SW_RTSO        0x0001
#define SW_SO          0x0002
#define SW_OE          0x0004
#define SW_FAULT       0x0008
#define SW_VE          0x0010   /* voltage enabled                         */
#define SW_QS          0x0020   /* 0 = quick stop active                   */
#define SW_SOD         0x0040
#define SW_WARNING     0x0080
#define SW_REMOTE      0x0200
#define SW_TARGET      0x0400   /* target reached                          */
#define SW_ILA         0x0800   /* internal limit active                   */
#define SW_OMS12       0x1000   /* PP set-point ack / PV speed 0 / HM attained / CSx target ignored-1 */
#define SW_OMS13       0x2000   /* PP / CSP following error, HM homing error */

/* controlword bits */
#define CW_SO          0x0001
#define CW_EV          0x0002
#define CW_QS          0x0004   /* 0 = quick stop                          */
#define CW_EO          0x0008
#define CW_OMS4        0x0010   /* PP new set-point / HM start             */
#define CW_OMS5        0x0020   /* PP change set immediately               */
#define CW_OMS6        0x0040   /* PP relative                             */
#define CW_FR          0x0080   /* fault reset (rising edge)               */
#define CW_HALT        0x0100

/* Calibration: what Phase 10.0 measures on the IS620N replaces these defaults
 * (A-05). Codes are the EMCY error codes posted and shown in 0x603F. */
typedef struct {
    uint32_t power_on_ms;        /* Not ready -> Switch on disabled         */
    uint32_t trans_cycles;       /* cycles a commanded transition takes     */
    uint16_t code_lost_op;       /* ESM left OP while enabled               */
    uint16_t code_ferr;          /* following error (0x8611 CiA402)          */
    uint16_t code_sync;          /* sync lost (drv_lose_sync)               */
    uint32_t supported_modes;    /* 0x6502 when the profile has none        */
} esc_cia402_cal_t;

typedef struct {
    /* commands and parameters (from the master) */
    uint16_t cw, cw_prev;
    int8_t   mode_req;           /* 0x6060 */
    int32_t  target_pos;         /* 0x607A */
    int32_t  target_vel;         /* 0x60FF, inc/s */
    int16_t  target_torque;      /* 0x6071 */
    uint32_t profile_vel;        /* 0x6081 */
    uint32_t max_profile_vel;    /* 0x607F */
    uint32_t accel, decel;       /* 0x6083 / 0x6084: profile modes (PP, PV) */
    uint32_t max_accel, max_decel; /* 0x60C5 / 0x60C6: cyclic modes (CSP, CSV) */
    uint32_t max_motor_speed;    /* 0x6080: cyclic modes                    */
    uint32_t qs_decel;           /* 0x6085 */
    int16_t  qs_option;          /* 0x605A */
    uint32_t ferr_window;        /* 0x6065, 0xFFFFFFFF = off */
    int8_t   hm_method;          /* 0x6098 */
    uint32_t hm_speed_fast, hm_speed_slow; /* 0x6099:01/02 */
    uint32_t hm_accel;           /* 0x609A */
    int32_t  hm_offset;          /* 0x607C */
    uint32_t supported;          /* 0x6502 */

    /* drive state */
    cia402_ds_t ds;
    int8_t   mode;               /* 0x6061 */
    uint16_t sw;                 /* 0x6041 */
    uint16_t err_code;           /* 0x603F */
    double   pos, vel;           /* actual, inc and inc/s */
    int32_t  ferr;               /* 0x60F4 */
    uint8_t  warn_mode;          /* requested mode not supported            */

    /* timing of the state machine */
    uint32_t wait_cycles;        /* commanded transition in progress        */
    cia402_ds_t pending;
    uint64_t t_since_power_ns;

    /* PP */
    uint8_t  pp_active, pp_buffered, pp_ack;
    int32_t  pp_target, pp_next;
    /* PV */
    /* HM */
    uint8_t  hm_running, hm_done, hm_error, hm_phase;
    int32_t  hm_switch;          /* virtual home switch (drv_home_switch)   */
    uint8_t  hm_has_switch;

    /* fault injection */
    uint8_t  inj_refuse_enable;
    uint32_t inj_slow_ms;
    int32_t  inj_ferr;
    uint16_t inj_fault_code;     /* fault pending, 0 = none                 */
    uint8_t  inj_latched;        /* fault stays until drv_clear             */
    uint8_t  inj_quickstop;

    /* counters */
    uint64_t steps, transitions, faults, emcy;
} esc_cia402_axis_t;

typedef struct esc_cia402 {
    int      axes;
    esc_cia402_cal_t cal;
    esc_cia402_axis_t ax[CIA402_MAX_AXES];
    uint64_t last_pd_ns;
    uint64_t dt_ns;               /* last step's dt                         */
    uint8_t  was_op;
} esc_cia402_t;

/* Defaults (spec + generic drive behaviour, before calibration). */
void esc_cia402_cal_default(esc_cia402_cal_t *c);

/* Enable the model on a profile node. 0 ok, -1 no profile / bad axes /
 * out of memory. The node keeps it across drop_node/restore_node (power
 * cycle -> esc_cia402_reset). */
int  esc_cia402_attach(esc_t *esc, int axes);
void esc_cia402_reset(esc_t *esc);          /* power-on: Not ready, all zero */
void esc_cia402_detach(esc_t *esc);

/* One drive cycle of every axis of node e: master outputs -> model ->
 * inputs. now_ns: frame arrival time. Called only for frames that carry
 * process data and for powered nodes. */
void esc_cia402_step(esc_t *e, uint64_t now_ns, FILE *log);

/* Same, with an explicit dt (offline tests). */
void esc_cia402_step_dt(esc_t *e, uint64_t dt_ns, FILE *log);

/* One line per axis (drv_status, and at soft_bus exit). */
void esc_cia402_print(const esc_t *e, FILE *log);

/* ctl commands starting with "drv_" (see the list in esc_cia402.c).
 * 0 ok, -1 error (message to log). */
int  esc_cia402_command(esc_t *chain, int n, int argc, char **argv, FILE *log, uint64_t now_ns);

/* Statusword the spec defines for a state (bits 0..6 only). */
uint16_t esc_cia402_sw_state(cia402_ds_t ds);
/* Name for logs / errors. */
const char *esc_cia402_ds_name(cia402_ds_t ds);

/* Pure state machine (A-01): next state for a controlword in a state, as the
 * spec's transition table says, ignoring timing and injections. A fault
 * reset is the rising edge of bit 7 (cw_prev -> cw) in Fault. */
cia402_ds_t esc_cia402_next(cia402_ds_t ds, uint16_t cw, uint16_t cw_prev, int16_t qs_option);

#endif /* ESC_CIA402_H */
