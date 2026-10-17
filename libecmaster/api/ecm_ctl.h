/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* ==========================================================================
 * ecm_ctl.h -- Phase 11.2: ctl.sock, the command side of the remote
 * diagnostics API (docs/api.md §10, plan claude/giai_doan_11_ke_hoach.md §3).
 *
 *   AF_UNIX SOCK_STREAM, one JSON object per line, exactly one reply line per
 *   request, served by the `api` thread of ecm_run. At most ECM_CTL_MAX_CLIENTS
 *   clients, ECM_CTL_RATE commands per second in all.
 *
 *   info        identity, cycle, ENI (path, sha256, InitCmd counts), PDO table,
 *               allowlist, limits                    (backend->info)
 *   sdo_read    slave, index, sub, type              any object, no Complete Access
 *   sdo_write   slave, index, sub, type, value       only with an allowlist entry;
 *               refused whatever the allowlist says: 0x1000..0x1FFF (communication
 *               area: PDO mapping/assignment, SM, sync), the application's own
 *               hard list (backend->deny: CiA402 controlword / mode of every axis),
 *               every object mapped in the slave's RxPDO (backend->rxpdo, from the
 *               PDO table really in use), every object the ENI writes with a CoE
 *               InitCmd (backend->initcmd); value within [min, max] of the entry;
 *               read back after the write
 *   diag_dump   backend writes the diagnostic snapshot to a timestamped file
 *
 * This file knows nothing about SOEM or CiA402: the application supplies an
 * ecm_ctl_backend_t. Everything here is tested offline (test_ctl_offline.c).
 * ========================================================================== */
#ifndef ECM_CTL_H
#define ECM_CTL_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#define ECM_CTL_VERSION        1
#define ECM_CTL_MAX_CLIENTS    4
#define ECM_CTL_RATE           20          /* commands per second, all clients */
#define ECM_CTL_LINE_MAX       1024        /* request line, '\n' included       */
#define ECM_CTL_OUT_MAX        (160 * 1024) /* unsent replies per client        */
#define ECM_CTL_REPLY_MAX      (128 * 1024) /* one reply (info: PDO table)     */
#define ECM_CTL_ALLOW_MAX      64
#define ECM_CTL_DATA_MAX       256         /* = ECM_MBX_MAX_DATA                */

/* ---- types ---------------------------------------------------------------- */
typedef enum {
    ECM_CT_U8 = 0, ECM_CT_I8, ECM_CT_U16, ECM_CT_I16, ECM_CT_U32, ECM_CT_I32,
    ECM_CT_U64, ECM_CT_I64, ECM_CT_F32, ECM_CT_RAW, ECM_CT_COUNT
} ecm_ctype_t;

int         ecm_ctl_type_parse(const char *name);      /* -1 unknown */
const char *ecm_ctl_type_name(int t);
int         ecm_ctl_type_size(int t);                  /* 0 for raw */

typedef struct {
    int      kind;       /* 0 signed (i), 1 unsigned (u), 2 float (d) */
    int64_t  i;
    uint64_t u;
    double   d;
} ecm_ctl_val_t;

/* value fits type t? 0 yes. Encode little-endian into b (size of t). */
int ecm_ctl_val_fits(int t, const ecm_ctl_val_t *v);
int ecm_ctl_encode(int t, const ecm_ctl_val_t *v, uint8_t *b);
/* compare v with lim in the domain of type t: -1 less, 0 equal, 1 greater */
int ecm_ctl_val_cmp(int t, const ecm_ctl_val_t *v, const ecm_ctl_val_t *lim);
/* decoded value of a buffer of exactly the type's size, as JSON text */
size_t ecm_ctl_decode_json(int t, const uint8_t *b, int n, char *out, size_t cap);

/* ---- allowlist (config/api_sdo_allow.json) -------------------------------- */
typedef struct {
    uint32_t vendor, product;
    uint16_t index;
    uint8_t  sub;
    int      type;
    ecm_ctl_val_t min, max;
    char     note[80];
} ecm_ctl_allow_t;

typedef struct {
    int  n;
    char path[256];      /* "" = no file: empty list */
    ecm_ctl_allow_t e[ECM_CTL_ALLOW_MAX];
} ecm_ctl_allowlist_t;

typedef struct ecm_ctl_backend ecm_ctl_backend_t;

/* Load and check the file: every entry needs vendor, product, index, sub,
 * type (not raw), min <= max within the type. An entry that is refused
 * anyway (0x1000..0x1FFF, backend->deny) is a load error: the file must not
 * promise what the master will never do. path NULL: empty list. 0 ok, -1 (err). */
int ecm_ctl_allow_load(const char *path, ecm_ctl_allowlist_t *al, const ecm_ctl_backend_t *be,
                       char *err, size_t errlen);

/* ---- backend (the application) ------------------------------------------- */
enum {                                  /* sdo_read / sdo_write results */
    ECM_CTL_SDO_OK = 0,
    ECM_CTL_SDO_BUSY = -1,              /* mailbox queue full            */
    ECM_CTL_SDO_TIMEOUT = -2,           /* no completion in time          */
    ECM_CTL_SDO_TOO_LARGE = -3,
    ECM_CTL_SDO_FAILED = -4,            /* abort / no answer (abort code if known) */
    ECM_CTL_SDO_NO_MBX = -5,            /* slave has no mailbox access in SAFE-OP/OP (11.6) */
};

struct ecm_ctl_backend {
    void *ctx;
    /* identity of slave s (1-based); -1: no such slave */
    int (*identity)(void *ctx, int slave, uint32_t *vendor, uint32_t *product);
    /* application hard list (slave 0 = any, at allowlist load). 1 = refused, reason in why */
    int (*deny)(void *ctx, int slave, uint16_t index, uint8_t sub, char *why, size_t n);
    /* 1 mapped in the slave's RxPDO, 0 not, -1 the PDO table is not known */
    int (*rxpdo)(void *ctx, int slave, uint16_t index, uint8_t sub);
    /* 1 the ENI writes this object of this slave with a CoE InitCmd */
    int (*initcmd)(void *ctx, int slave, uint16_t index, uint8_t sub);
    /* 0: an SDO to this slave may be sent now; else the reason (bus not RUN, recovery, ...) */
    int (*ready)(void *ctx, int slave, char *why, size_t n);
    /* blocking SDO (the caller is the api thread). *size in: capacity, out: bytes */
    int (*sdo_read)(void *ctx, int slave, uint16_t index, uint8_t sub, uint8_t *buf, int *size, uint32_t *abort);
    int (*sdo_write)(void *ctx, int slave, uint16_t index, uint8_t sub, const uint8_t *buf, int size, uint32_t *abort);
    const char *(*abort_text)(uint32_t abort);
    /* JSON members (no braces) of the "info" object; returns length */
    size_t (*info)(void *ctx, char *buf, size_t cap);
    /* writes the dump; JSON members (no braces) for the reply; <0 error (text in buf) */
    int (*dump)(void *ctx, char *buf, size_t cap);
};

/* ---- one request -> one reply (no I/O; the server below calls it) ---------- */
typedef struct {
    double   tokens;                    /* token bucket, ECM_CTL_RATE per second */
    uint64_t last_ns;
    uint64_t cmds, ok, errors, rate_limited, refused_writes, sdo_reads, sdo_writes;
    uint64_t sdo_ns_max;                /* longest SDO command, wall time */
} ecm_ctl_state_t;

void   ecm_ctl_state_init(ecm_ctl_state_t *st);
/* line without '\n'; reply written to out with a trailing '\n'; returns its length */
size_t ecm_ctl_handle(ecm_ctl_state_t *st, const ecm_ctl_backend_t *be, const ecm_ctl_allowlist_t *al,
                      const char *line, size_t len, uint64_t now_ns, char *out, size_t cap);

/* ---- server -------------------------------------------------------------- */
typedef struct {
    int    fd;                          /* -1 free */
    size_t inlen, outlen, outoff;
    int    discard;                     /* inside an over-long line: drop until '\n' */
    char   in[ECM_CTL_LINE_MAX];
    char   out[ECM_CTL_OUT_MAX];
} ecm_ctl_client_t;

typedef struct {
    int    lfd;
    char   path[256];
    ecm_ctl_state_t st;
    uint64_t accepted, refused_clients, dropped_slow, dropped_long;
    ecm_ctl_client_t c[ECM_CTL_MAX_CLIENTS];
} ecm_ctl_srv_t;

/* Create the socket at path (0660 by default via mode; gid -1 = keep). A stale
 * socket file nobody listens on is replaced; a live one is refused (another
 * ecm_run). 0 ok, -1 (err). */
int  ecm_ctl_srv_open(ecm_ctl_srv_t *s, const char *path, unsigned mode, int gid, char *err, size_t errlen);
/* pollfds to wait on: the listening socket + every client (POLLOUT while
 * replies are pending). Returns the count written to p (<= 1 + MAX_CLIENTS). */
int  ecm_ctl_srv_fds(const ecm_ctl_srv_t *s, struct pollfd *p, int max);
/* After poll(): accept, read, run at most max_cmds requests in all (one per
 * client per call, in client order), send. Returns the number of requests run. */
int  ecm_ctl_srv_service(ecm_ctl_srv_t *s, const struct pollfd *p, int np, const ecm_ctl_backend_t *be,
                         const ecm_ctl_allowlist_t *al, uint64_t now_ns, int max_cmds);
int  ecm_ctl_srv_clients(const ecm_ctl_srv_t *s);
/* 1: a client has a complete request already read (poll() will not report
 * it again): service again without waiting */
int  ecm_ctl_srv_pending(const ecm_ctl_srv_t *s);
void ecm_ctl_srv_close(ecm_ctl_srv_t *s);

#endif /* ECM_CTL_H */
