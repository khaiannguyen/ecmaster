#!/usr/bin/env python3
"""apply_soem_txtime_v2.py -- ecmaster Phase 8.5, R-02 fix: only the scheduled frame uses ETF.

Run inside the SOEM tree AFTER apply_soem_txtime.py (v1):
    cd ~/projects/SOEM && python3 ~/projects/ecmaster/tools/etf/apply_soem_txtime_v2.py
Edits oshw/linux/nicdrv.h and oshw/linux/nicdrv.c in place (backups *.v1), idempotent.
Then regenerate the project patch:
    git diff -- oshw/linux/nicdrv.c oshw/linux/nicdrv.h > ~/projects/ecmaster/patches/soem-txtime.patch

Why (R-02, 29/9): with v1 every frame without a scheduled time (IO group, mailbox,
diag, config) went through ETF at now + asap. The IO group is sent only after the
motion reply, so at N=32 (soft_bus RTT ~250 us) it waited another asap = 150 us
and missed the tick deadline in 44 % of its cycles.

v2 rule, inside tx_mutex:
- the frame scheduled with ecx_txtime_set_next() and still >= asap_ns ahead:
  SO_PRIORITY = etf prio (mqprio -> ETF queue) + SCM_TXTIME, monotonic rule as v1;
- every other frame (and a scheduled frame that is already too late, counted in
  txtime_late): SO_PRIORITY 0 (plain queue, no ETF) + send(), leaves now,
  counted in txtime_bypass.
SO_PRIORITY is only changed when it differs (one setsockopt, twice per tick at most).
Consequence: --link etf needs mqprio with ETF on the etf class only. A root ETF
qdisc (e.g. the old veth test) drops the bypass frames, config included.
"""
import os
import shutil
import sys

H = "oshw/linux/nicdrv.h"
C = "oshw/linux/nicdrv.c"
for f in (H, C):
    if not os.path.isfile(f):
        sys.exit(f"{f} not found -- run from the SOEM source root")

MARK = "txtime_bypass"

# ---------------------------------------------------------------- nicdrv.h
h = open(H).read()
if "ecmaster txtime" not in h:
    sys.exit(f"{H}: v1 patch missing -- run apply_soem_txtime.py first")
if MARK in h:
    print(f"{H}: already v2")
else:
    old = "   uint32_t txtime_send_err;   /* sendmsg() failures (ETF drop -> ENOBUFS) */"
    if h.count(old) != 1:
        sys.exit(f"{H}: field anchor not found -- send nicdrv.h to Claude")
    new = old + """
   uint32_t txtime_bypass;     /* v2: frames sent now on SO_PRIORITY 0 (no ETF) */
   int txtime_etf_prio;        /* v2: SO_PRIORITY that mqprio maps to the ETF queue */
   int txtime_cur_prio;        /* v2: SO_PRIORITY currently set on the socket */"""
    h = h.replace(old, new, 1)
    shutil.copy2(H, H + ".v1")
    open(H, "w").write(h)
    print(f"{H}: v2 fields added (backup {H}.v1)")

# ---------------------------------------------------------------- nicdrv.c
c = open(C).read()
if "ecmaster txtime" not in c:
    sys.exit(f"{C}: v1 patch missing -- run apply_soem_txtime.py first")
if MARK in c:
    print(f"{C}: already v2")
    sys.exit(0)


def rep(old, new, what):
    global c
    if c.count(old) != 1:
        sys.exit(f"{C}: anchor for '{what}' found {c.count(old)} times -- send nicdrv.c to Claude")
    c = c.replace(old, new, 1)


# enable: remember the ETF priority and what the socket has now
rep("""   port->txtime_late = 0;
   port->txtime_send_err = 0;
   port->txtime_enabled = 1;""",
    """   port->txtime_late = 0;
   port->txtime_send_err = 0;
   port->txtime_bypass = 0;
   port->txtime_etf_prio = so_priority;
   port->txtime_cur_prio = so_priority;   /* set by the setsockopt above */
   port->txtime_enabled = 1;""",
    "enable")

# replace the whole send helper
i = c.find("static int ecx_send_txtime(ecx_portt *port, int sock, const void *buf, int len)")
j = c.find("/* ---- end ecmaster txtime", i)
if i < 0 or j < 0:
    sys.exit(f"{C}: ecx_send_txtime() not found -- send nicdrv.c to Claude")

helper = r'''/* v2: SO_PRIORITY selects the mqprio traffic class (etf prio -> ETF queue,
 * 0 -> a plain queue). Called with tx_mutex held; changes it only if needed. */
static int ecx_txtime_prio(ecx_portt *port, int sock, int prio)
{
   if (port->txtime_cur_prio == prio)
      return 0;
   if (setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0)
      return -1;
   port->txtime_cur_prio = prio;
   return 0;
}

/* v2: only the frame scheduled with ecx_txtime_set_next(), still at least
 * asap_ns ahead, goes through ETF with SCM_TXTIME. Every other frame (config,
 * mailbox, IO group, diag, or a scheduled frame already too late) leaves NOW
 * on SO_PRIORITY 0, a queue without ETF: it needs no launch time and must not
 * wait asap_ns (R-02: that wait cost the IO group its receive budget at N=32).
 * Requires mqprio with ETF on the etf class only; a root ETF drops these. */
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
   {
      port->txtime_late++;                   /* too late for ETF: send now */
      t = 0;
   }
   if (t == 0)
   {
      rval = ecx_txtime_prio(port, sock, 0) < 0 ? -1 : (int)send(sock, buf, (size_t)len, 0);
      if (rval == -1)
         port->txtime_send_err++;
      else
         port->txtime_bypass++;
      pthread_mutex_unlock(&(port->tx_mutex));
      return rval;
   }
   /* ETF drops a frame whose txtime is below one already dequeued: keep the
    * launch times of the ETF frames strictly increasing, spaced by the wire
    * time of the previous one (+ preamble, FCS, inter-frame gap = 24 bytes). */
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

   rval = ecx_txtime_prio(port, sock, port->txtime_etf_prio) < 0 ? -1 : (int)sendmsg(sock, &msg, 0);
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
'''
c = c[:i] + helper + c[j:]
shutil.copy2(C, C + ".v1")
open(C, "w").write(c)
print(f"{C}: v2 applied (backup {C}.v1)")
print("next: rebuild SOEM, regenerate patches/soem-txtime.patch, then apply_ecm_run_etf_v2.py")
