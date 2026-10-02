/* ==========================================================================
 * ecm_xchg.h — Phase 10.4: exchange between the application thread and the RT
 * thread (design: docs/app_rt_exchange.md).
 *
 *   commands   app -> RT, one SPSC ring, at most ECM_XCHG_CMD_PER_TICK per
 *              tick, FIFO; a command for a future tick waits at the head
 *   setpoints  app -> RT, one SPSC ring per output slot, keyed by the tick
 *              whose frame carries them: the hook of tick k uses the entry
 *              for k+1; older entries are late (dropped, counted); none ->
 *              underrun (counted once armed), the last value is held
 *   state      RT -> app, one seqlock record per slot + a clock record; the
 *              RT thread never waits, a reader retries a bounded number of
 *              times
 *
 * The RT thread stays the only owner of the IOmap: ecm_xchg_rt() is called
 * from the RT hook with the group's IOmap. libecmaster attaches no meaning
 * to a slot (it is an ecm_pdo_handle_t) nor to a command op beyond SET; the
 * CiA402 layer (libecm_cia402) builds on top. Pure C11, no syscalls, no
 * locks, no allocation in the RT path. Offline tests + TSan (Q-05).
 * ========================================================================== */
#ifndef ECM_XCHG_H
#define ECM_XCHG_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "../pdo/ecm_pdo.h"

#define ECM_XRING_CAP          512u   /* power of 2; holds both element types */
#define ECM_XCHG_CMD_CAP       256u   /* commands in flight                   */
#define ECM_XCHG_MAX_SLOTS     32
#define ECM_XCHG_CMD_PER_TICK  8      /* RT budget: commands applied per tick */
#define ECM_XCHG_SP_SKIP_MAX   16     /* RT budget: late setpoints dropped/tick */
#define ECM_XST_WORDS          8

/* ---- elements (32 byte each) ------------------------------------------- */
typedef struct {
    uint64_t tick;        /* apply at this tick; 0 = at the next hook      */
    uint16_t target;      /* slot                                           */
    uint16_t op;          /* ECM_XOP_*, or owner-defined >= ECM_XOP_USER    */
    uint32_t seq;         /* app sequence number, echoed in the state       */
    int64_t  arg0, arg1;
} ecm_xcmd_t;
_Static_assert(sizeof(ecm_xcmd_t) == 32, "ecm_xcmd_t must be 32 byte");

enum { ECM_XOP_NOP = 0, ECM_XOP_SET = 1, ECM_XOP_USER = 0x100 };

typedef struct {
    uint64_t tick;        /* the frame this setpoint goes out in            */
    int64_t  v[3];
} ecm_xsp_t;
_Static_assert(sizeof(ecm_xsp_t) == 32, "ecm_xsp_t must be 32 byte");

/* ---- SPSC ring of 32-byte elements --------------------------------------- */
typedef struct {
    uint64_t        buf[ECM_XRING_CAP][4];
    size_t          mask;            /* capacity - 1, set by init        */
    _Atomic size_t  head;            /* written by the producer only     */
    _Atomic size_t  tail;            /* written by the consumer only     */
    uint64_t        full_drops;      /* producer only                    */
} ecm_xring_t;

void ecm_xring_init(ecm_xring_t *r, size_t cap);          /* cap: power of 2 <= ECM_XRING_CAP */
int  ecm_xring_push(ecm_xring_t *r, const void *e32);     /* producer: 0 ok, -1 full          */
int  ecm_xring_peek(ecm_xring_t *r, void *e32);           /* consumer: 0 ok, -1 empty         */
void ecm_xring_pop(ecm_xring_t *r);                       /* consumer, after a successful peek */

/* ---- seqlock record --------------------------------------------------- */
typedef struct {
    _Atomic uint32_t seq;            /* odd while the writer writes      */
    _Atomic uint64_t tick;
    _Atomic uint64_t w[ECM_XST_WORDS];
} ecm_xst_t;

void ecm_xst_write(ecm_xst_t *s, uint64_t tick, const uint64_t *w, int n);   /* never waits */
/* 0 ok (a consistent copy), -1 torn every one of `tries` times */
int  ecm_xst_read(ecm_xst_t *s, uint64_t *tick, uint64_t *w, int n, int tries);

/* ---- the exchange ------------------------------------------------------- */
enum {                                /* words of a slot's state record   */
    ECM_XS_VALUE = 0,                 /* value read at this hook (input) or held (output) */
    ECM_XS_USED, ECM_XS_LATE, ECM_XS_UNDERRUN, ECM_XS_DROPPED_LOST,
    ECM_XS_VALID,                     /* 1: the value is from a valid frame / a setpoint */
    ECM_XS_NWORDS
};
enum {                                /* words of the clock record        */
    ECM_XC_T_SEND_NS = 0, ECM_XC_CYCLE_NS, ECM_XC_IN_VALID, ECM_XC_BUS_LOST,
    ECM_XC_CMD_APPLIED, ECM_XC_CMD_DEFERRED, ECM_XC_LAST_SEQ, ECM_XC_CMD_UNKNOWN,
    ECM_XC_NWORDS
};
_Static_assert(ECM_XS_NWORDS <= ECM_XST_WORDS && ECM_XC_NWORDS <= ECM_XST_WORDS, "state words");

typedef struct {
    ecm_pdo_handle_t h;
    uint8_t          is_out;          /* setpoints and SET commands only for outputs */
    /* RT-only state */
    uint8_t          armed;           /* a setpoint or SET has been applied */
    int64_t          last;
    uint64_t         used, late, underrun, dropped_lost;
} ecm_xslot_t;

typedef struct {
    int          nslots;
    uint64_t     cycle_ns;
    ecm_xslot_t  slot[ECM_XCHG_MAX_SLOTS];
    ecm_xring_t  cmd;
    ecm_xring_t  sp[ECM_XCHG_MAX_SLOTS];
    ecm_xst_t    st[ECM_XCHG_MAX_SLOTS];
    ecm_xst_t    clock;
    /* RT-only counters */
    uint64_t     cmd_applied, cmd_deferred, cmd_unknown;
    uint32_t     last_seq;
} ecm_xchg_t;

/* Init before any thread uses it (not RT). */
void ecm_xchg_init(ecm_xchg_t *x, uint64_t cycle_ns);
/* Add a slot; dir from the handle. Returns the slot number or -1. */
int  ecm_xchg_add_slot(ecm_xchg_t *x, const ecm_pdo_handle_t *h);

/* RT thread, once per hook (tick k): commands, setpoints for k+1, state.
 * in_valid: the inputs of this tick come from a good frame. bus_lost:
 * no process data (setpoints for now are dropped, not replayed later). */
void ecm_xchg_rt(ecm_xchg_t *x, uint8_t *iomap, uint64_t tick, uint64_t t_send_ns,
                 int in_valid, int bus_lost);

/* Application side (one producer thread). 0 ok, -1 full / bad slot. */
int  ecm_xchg_cmd(ecm_xchg_t *x, const ecm_xcmd_t *c);
int  ecm_xchg_setpoint(ecm_xchg_t *x, int slot, uint64_t tick, int64_t v0);
/* Reader side (any non-RT thread). 0 ok, -1 torn every try. */
int  ecm_xchg_read_clock(ecm_xchg_t *x, uint64_t *tick, uint64_t w[ECM_XST_WORDS]);
int  ecm_xchg_read_slot(ecm_xchg_t *x, int slot, uint64_t *tick, uint64_t w[ECM_XST_WORDS]);

#define ECM_XST_TRIES 64

#endif /* ECM_XCHG_H */
