#!/usr/bin/env python3
"""apply_soem_txtime.py -- ecmaster Phase 8.5 step 6.3: SO_TXTIME launch time in SOEM.

Run inside the SOEM tree:   cd ~/projects/SOEM && python3 apply_soem_txtime.py
Edits oshw/linux/nicdrv.h and oshw/linux/nicdrv.c in place (backups *.orig),
idempotent. Afterwards make the project patch with
    git diff -- oshw/linux/nicdrv.c oshw/linux/nicdrv.h > ~/projects/ecmaster/patches/soem-txtime.patch

Design (giai_doan_8_ke_hoach.md section 6.3):
- txtime_enabled == 0 (default): ecx_outframe() is byte-for-byte the old send() path,
  so the af_packet backend and the golden pcaps do not change.
- txtime_enabled == 1: every frame on the PRIMARY socket goes out with sendmsg() +
  SCM_TXTIME (CLOCK_TAI). ETF drops any frame without a valid txtime, so non-cyclic
  frames (config, mailbox, diag) get now_tai + asap_ns.
- The cyclic thread calls ecx_txtime_set_next(port, t_target_tai) right before
  ecx_send_processdata*(); only the next frame uses it, later frames of the same
  cycle (split frame, second group) fall back to the monotonic rule.
- Monotonic invariant, inside tx_mutex: txtime = max(requested, last + wire_time(last_len)).
- Redundant secondary socket: unchanged plain send() (only the primary NIC runs ETF).
- Errors (SO_EE_ORIGIN_TXTIME) land on the socket error queue; the caller drains it
  (libecmaster's TX-timestamp reader), SOEM does not read MSG_ERRQUEUE.
"""
import os
import re
import shutil
import sys

H = "oshw/linux/nicdrv.h"
C = "oshw/linux/nicdrv.c"
for f in (H, C):
    if not os.path.isfile(f):
        sys.exit(f"{f} not found -- run from the SOEM source root")

MARK = "ecmaster txtime"

# ---------------------------------------------------------------- nicdrv.h
h = open(H).read()
DEFINE = "#define ECMASTER_SOEM_TXTIME_PATCH 1   /* ecmaster txtime: lets apps test for the patch */\n"
if MARK in h:
    print(f"{H}: already patched")
    if "ECMASTER_SOEM_TXTIME_PATCH" not in h:
        k = h.find("/* ecmaster txtime (Phase 8.5) */\nint ecx_txtime_enable")
        if k < 0:
            sys.exit(f"{H}: prototypes not found, cannot add the define")
        h = h[:k] + DEFINE + h[k:]
        open(H, "w").write(h)
        print(f"{H}: added ECMASTER_SOEM_TXTIME_PATCH")
else:
    m = re.search(r"\n\}\s*ecx_portt\s*;", h)
    if not m:
        sys.exit(f"{H}: '}} ecx_portt;' not found -- send the file to Claude")
    fields = """
   /* ecmaster txtime (Phase 8.5): SO_TXTIME launch time on the primary socket.
    * All times are CLOCK_TAI ns. txtime_enabled == 0 keeps the plain send() path. */
   int txtime_enabled;
   int64_t txtime_next;        /* launch time for the next frame, 0 = none */
   int64_t txtime_last;        /* launch time given to the previous frame */
   int txtime_last_len;        /* its length in bytes */
   int64_t txtime_asap_ns;     /* lead for frames without a scheduled time */
   uint32_t txtime_ns_per_byte;/* wire time per byte: 8 at 1 Gbit/s, 80 at 100 Mbit/s */
   uint32_t txtime_late;       /* scheduled time already too close: sent at now + asap */
   uint32_t txtime_send_err;   /* sendmsg() failures (ETF drop -> ENOBUFS) */"""
    h = h[:m.start()] + fields + h[m.start():]
    protos = "\n" + DEFINE + """/* ecmaster txtime (Phase 8.5) */
int ecx_txtime_enable(ecx_portt *port, int so_priority, int64_t asap_ns, uint32_t ns_per_byte);
void ecx_txtime_set_next(ecx_portt *port, int64_t tai_ns);
"""
    m2 = re.search(r"\nint\s+ecx_outframe\s*\(", h)
    if m2:
        h = h[:m2.start()] + "\n" + protos + h[m2.start():]
    else:
        m3 = h.rfind("#endif")
        if m3 < 0:
            sys.exit(f"{H}: no place for prototypes -- send the file to Claude")
        h = h[:m3] + protos + "\n" + h[m3:]
    if "#include <stdint.h>" not in h:
        h = h.replace("#include <pthread.h>", "#include <pthread.h>\n#include <stdint.h>", 1) \
            if "#include <pthread.h>" in h else "#include <stdint.h>\n" + h
    shutil.copy2(H, H + ".orig")
    open(H, "w").write(h)
    print(f"{H}: patched (backup {H}.orig)")

# ---------------------------------------------------------------- nicdrv.c
c = open(C).read()
if MARK in c:
    print(f"{C}: already patched")
else:
    inc_old = "#include <poll.h>\n"
    inc_new = """#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <linux/net_tstamp.h>   /* struct sock_txtime, SOF_TXTIME_* (ecmaster txtime) */

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif
#ifndef SO_TXTIME
#define SO_TXTIME 61
#define SCM_TXTIME SO_TXTIME
#endif
"""
    if inc_old not in c:
        sys.exit(f"{C}: include anchor not found")
    c = c.replace(inc_old, inc_new, 1)

    out_old = """   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;
   rval = send(*stack->sock, (*stack->txbuf)[idx], lp, 0);
   if (rval == -1)"""
    out_new = """   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;
   if (!stacknumber && port->txtime_enabled)
      rval = ecx_send_txtime(port, *stack->sock, (*stack->txbuf)[idx], lp); /* ecmaster txtime */
   else
      rval = send(*stack->sock, (*stack->txbuf)[idx], lp, 0);
   if (rval == -1)"""
    if out_old not in c:
        sys.exit(f"{C}: ecx_outframe body not found -- send the file to Claude")
    c = c.replace(out_old, out_new, 1)

    helper = r'''
/* ---- ecmaster txtime (Phase 8.5) -------------------------------------------
 * Launch time via SO_TXTIME + ETF qdisc. See ecmaster docs / giai_doan_8. */

static int64_t ecx_tai_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_TAI, &ts);
   return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** Enable launch time on the primary socket.
 * @param[in] so_priority  SO_PRIORITY that the mqprio map sends to the ETF queue
 * @param[in] asap_ns      launch lead for frames without a scheduled time,
 *                         must exceed the send path latency (e.g. 150000)
 * @param[in] ns_per_byte  wire time per byte (8 = 1 Gbit/s, 80 = 100 Mbit/s)
 * @return 1 on success, 0 on failure (errno from setsockopt) */
int ecx_txtime_enable(ecx_portt *port, int so_priority, int64_t asap_ns, uint32_t ns_per_byte)
{
   struct sock_txtime st;
   int prio = so_priority;

   if (port->sockhandle < 0)
      return 0;
   if (setsockopt(port->sockhandle, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0)
      return 0;
   memset(&st, 0, sizeof(st));
   st.clockid = CLOCK_TAI;
   st.flags = SOF_TXTIME_REPORT_ERRORS;
   if (setsockopt(port->sockhandle, SOL_SOCKET, SO_TXTIME, &st, sizeof(st)) < 0)
      return 0;
   pthread_mutex_lock(&(port->tx_mutex));
   port->txtime_next = 0;
   port->txtime_last = 0;
   port->txtime_last_len = 0;
   port->txtime_asap_ns = asap_ns;
   port->txtime_ns_per_byte = ns_per_byte;
   port->txtime_late = 0;
   port->txtime_send_err = 0;
   port->txtime_enabled = 1;
   pthread_mutex_unlock(&(port->tx_mutex));
   return 1;
}

/** Launch time (CLOCK_TAI ns) for the next frame sent on this port. */
void ecx_txtime_set_next(ecx_portt *port, int64_t tai_ns)
{
   pthread_mutex_lock(&(port->tx_mutex));
   port->txtime_next = tai_ns;
   pthread_mutex_unlock(&(port->tx_mutex));
}

static int ecx_send_txtime(ecx_portt *port, int sock, const void *buf, int len)
{
   char cbuf[CMSG_SPACE(sizeof(uint64_t))];
   struct iovec iov;
   struct msghdr msg;
   struct cmsghdr *cm;
   int64_t now, t, min_t;
   uint64_t txtime;
   int rval;

   pthread_mutex_lock(&(port->tx_mutex));
   now = ecx_tai_now_ns();
   t = port->txtime_next;
   port->txtime_next = 0;
   if (t != 0 && t < now + port->txtime_asap_ns)
      port->txtime_late++;                   /* scheduled too late: send asap */
   if (t == 0 || t < now + port->txtime_asap_ns)
      t = now + port->txtime_asap_ns;
   /* ETF drops a frame whose txtime is below one already dequeued: keep the
    * launch times of this socket strictly increasing, spaced by the wire time
    * of the previous frame (+ preamble, FCS, inter-frame gap = 24 bytes). */
   if (port->txtime_last_len > 0)
   {
      min_t = port->txtime_last +
              (int64_t)(port->txtime_last_len + 24) * port->txtime_ns_per_byte;
      if (t < min_t)
         t = min_t;
   }
   txtime = (uint64_t)t;

   memset(cbuf, 0, sizeof(cbuf));
   iov.iov_base = (void *)buf;
   iov.iov_len = (size_t)len;
   memset(&msg, 0, sizeof(msg));
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;
   msg.msg_control = cbuf;
   msg.msg_controllen = sizeof(cbuf);
   cm = CMSG_FIRSTHDR(&msg);
   cm->cmsg_level = SOL_SOCKET;
   cm->cmsg_type = SCM_TXTIME;
   cm->cmsg_len = CMSG_LEN(sizeof(uint64_t));
   memcpy(CMSG_DATA(cm), &txtime, sizeof(txtime));

   rval = (int)sendmsg(sock, &msg, 0);
   if (rval == -1)
   {
      port->txtime_send_err++;
   }
   else
   {
      port->txtime_last = t;
      port->txtime_last_len = len < 60 ? 60 : len;
   }
   pthread_mutex_unlock(&(port->tx_mutex));
   return rval;
}
/* ---- end ecmaster txtime ------------------------------------------------ */

'''
    anchor = "/** Transmit buffer over socket (non blocking).\n * @param[in] port        = port context struct\n * @param[in] idx         = index in tx buffer array"
    if anchor not in c:
        sys.exit(f"{C}: ecx_outframe doc-comment anchor not found")
    c = c.replace(anchor, helper + anchor, 1)

    # zero the new fields at setup (primary port only)
    setup_old = "      port->redstate = ECT_RED_NONE;\n"
    setup_new = ("      port->redstate = ECT_RED_NONE;\n"
                 "      port->txtime_enabled = 0; /* ecmaster txtime */\n"
                 "      port->txtime_next = 0;\n"
                 "      port->txtime_last = 0;\n"
                 "      port->txtime_last_len = 0;\n")
    if setup_old not in c:
        sys.exit(f"{C}: setup anchor not found")
    c = c.replace(setup_old, setup_new, 1)

    shutil.copy2(C, C + ".orig")
    open(C, "w").write(c)
    print(f"{C}: patched (backup {C}.orig)")

print("next: rebuild SOEM, then the ecmaster apps; golden must stay identical (txtime off)")
