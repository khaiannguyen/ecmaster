#!/usr/bin/env python3
"""apply_ecm_run_etf.py -- Phase 8.5 step 6.3: `ecm_run --link etf`.

Run from the ecmaster repo root:  python3 apply_ecm_run_etf.py
Edits apps/ecm_run/ecm_run.c and apps/ecm_run/Makefile in place (backups
*.pre-etf), idempotent. Needs SOEM with patches/soem-txtime.patch
(nicdrv.h defines ECMASTER_SOEM_TXTIME_PATCH).

Design (see giai_doan_8_nhat_ky_8_5.md, section 6.3):
- The MONO schedule `next` stays the tick's TARGET (DC PI unchanged). With
  --link etf the RT thread wakes at next - lead, gives the motion frame
  SCM_TXTIME = next + (TAI - MONO), and the i226 launches it at `next`.
  IO, mailbox, diag, recovery frames get now_tai + asap inside SOEM.
- lead < ETF delta on purpose: the frame is dequeued by the sending thread
  itself (txtime - delta is already past), so no qdisc hrtimer is involved
  (that hrtimer was the source of the MISSED drops in T-03/T-04).
- Receive deadline from the WAKE target: (next - lead) + cycle - guard.
- TAI - MONO is read every tick; a TAI step does not move the MONO
  schedule, it is only counted (> 100 us between ticks).
- DC(b) gets the launch time `next` as host time of the send.
- The telemetry thread counts SO_EE_ORIGIN_TXTIME errors from SOEM's
  error queue (and drains it even with --no-tx-ts).
"""
import os
import shutil
import sys

C = "apps/ecm_run/ecm_run.c"
MK = "apps/ecm_run/Makefile"
for f in (C, MK):
    if not os.path.isfile(f):
        sys.exit(f"{f} not found -- run from the ecmaster repo root")

MARK = "Phase 8.5 (R-02): --link etf"
src = open(C).read()
if MARK in src:
    print(f"{C}: already patched")
    sys.exit(0)

edits = []


def rep(old, new, what):
    global src
    if src.count(old) != 1:
        sys.exit(f"{C}: anchor for '{what}' found {src.count(old)} times (need 1) -- send ecm_run.c to Claude")
    src = src.replace(old, new, 1)
    edits.append(what)


# 1. includes
rep("#include <linux/sockios.h>\n",
    "#include <linux/sockios.h>\n#include <linux/errqueue.h>   /* Phase 8.5: SO_EE_ORIGIN_TXTIME */\n",
    "include errqueue.h")

# 2. globals
rep("static uint64_t g_foreign_replies[3];           /* [group]: reply header != what was sent */\n",
    """static uint64_t g_foreign_replies[3];           /* [group]: reply header != what was sent */

/* ---- Phase 8.5 (R-02): --link etf (patches/soem-txtime.patch) ----
 * `next` (CLOCK_MONOTONIC) stays the tick's target. With --link etf the RT
 * thread wakes g_etf_lead_us earlier and gives the motion frame
 * SCM_TXTIME = next + (TAI - MONO); the NIC launches it at `next` (ETF
 * offload). Every other frame gets now_tai + asap inside SOEM. lead < ETF
 * delta: the sender dequeues the frame itself, no qdisc hrtimer involved. */
#ifndef SO_EE_ORIGIN_TXTIME
#define SO_EE_ORIGIN_TXTIME 6
#define SO_EE_CODE_TXTIME_INVALID_PARAM 1
#define SO_EE_CODE_TXTIME_MISSED 2
#endif
#define TAI_STEP_NS 100000                       /* TAI - MONO moved more than this in one tick */
static int         g_link_etf        = 0;
static long        g_etf_lead_us     = 200;
static long        g_etf_asap_us     = 150;
static int         g_etf_prio        = 3;
static int         g_etf_ns_per_byte = 8;        /* 1 Gbit/s; 80 for a 100 Mbit/s link */
static uint64_t    g_tai_steps;                  /* RT only */
static int64_t     g_tai_step_max;               /* RT only, ns */
static atomic_ulong g_etf_missed, g_etf_invalid, g_etf_other;   /* telemetry thread */
""",
    "globals")

# 3. helpers after ts_to_ns
rep("""static uint64_t ts_to_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ULL + (uint64_t)t->tv_nsec;
}
""",
    """static uint64_t ts_to_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ULL + (uint64_t)t->tv_nsec;
}

/* Phase 8.5 */
static void ns_to_ts(uint64_t ns, struct timespec *t)
{
    t->tv_sec  = (time_t)(ns / 1000000000ULL);
    t->tv_nsec = (long)(ns % 1000000000ULL);
}

/* TAI - MONO: midpoint of TAI, MONO, TAI, narrowest of 2 (clock_check rule) */
static int64_t tai_minus_mono_ns(void)
{
    int64_t best = 0, best_w = INT64_MAX;
    for (int i = 0; i < 2; i++) {
        struct timespec a, m, b;
        clock_gettime(CLOCK_TAI, &a);
        clock_gettime(CLOCK_MONOTONIC, &m);
        clock_gettime(CLOCK_TAI, &b);
        int64_t w = ts_diff_ns(&b, &a);
        if (w < best_w) { best_w = w; best = (int64_t)ts_to_ns(&a) + w / 2 - (int64_t)ts_to_ns(&m); }
    }
    return best;
}

/* Startup only: is there an ETF qdisc with offload on this interface? */
static int etf_qdisc_state(const char *ifname)
{
    char cmd[128], line[512];
    int etf = 0, off = 0;
    snprintf(cmd, sizeof(cmd), "tc qdisc show dev %s 2>/dev/null", ifname);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof(line), p)) {
        if (strstr(line, "qdisc etf")) { etf = 1; if (strstr(line, "offload on")) off = 1; }
    }
    pclose(p);
    return etf ? (off ? 2 : 1) : 0;
}
""",
    "helpers")

# 4. telemetry: count ETF errors in the normal drain
rep("""                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                        struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
                        uint64_t tx_ts_ns = ts_to_ns(&ts[0]);  /* [0]=software, matches enable_tx_sw_timestamping */
                        turnaround_on_tx_complete_idx(&g_telemetry.turnaround, tx_ts_ns, tx_idx);
                    }
                }""",
    """                int etf_err = 0;   /* Phase 8.5: an ETF drop report, not a TX completion */
                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_PACKET && cmsg->cmsg_type == PACKET_TX_TIMESTAMP) {
                        const struct sock_extended_err *ee = (const struct sock_extended_err *)CMSG_DATA(cmsg);
                        if (ee->ee_origin == SO_EE_ORIGIN_TXTIME) {
                            etf_err = 1;
                            if (ee->ee_code == SO_EE_CODE_TXTIME_MISSED) atomic_fetch_add(&g_etf_missed, 1);
                            else if (ee->ee_code == SO_EE_CODE_TXTIME_INVALID_PARAM) atomic_fetch_add(&g_etf_invalid, 1);
                            else atomic_fetch_add(&g_etf_other, 1);
                        }
                    }
                }
                for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg && !etf_err; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                        struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
                        uint64_t tx_ts_ns = ts_to_ns(&ts[0]);  /* [0]=software, matches enable_tx_sw_timestamping */
                        turnaround_on_tx_complete_idx(&g_telemetry.turnaround, tx_ts_ns, tx_idx);
                    }
                }""",
    "telemetry: count ETF errors")

# 5. telemetry: with --no-tx-ts and --link etf, still drain + count the error queue
rep("""        turnaround_drain_tx_order(&g_telemetry.turnaround, &g_tx_order);
""",
    """        turnaround_drain_tx_order(&g_telemetry.turnaround, &g_tx_order);

        /* Phase 8.5: --no-tx-ts but --link etf: ETF drop reports still
         * land on SOEM's error queue. Drain them (a non-empty error queue
         * keeps POLLERR set and turns SOEM's ppoll() into a busy loop). */
        if (soem_raw_fd < 0 && g_link_etf) {
            int fd = ctx.port.sockhandle;
            struct pollfd pfd = { .fd = fd, .events = 0 };
            while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLERR)) {
                char control[256], buf[64];
                struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
                struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                                      .msg_control = control, .msg_controllen = sizeof(control) };
                if (recvmsg(fd, &msg, MSG_ERRQUEUE) < 0) break;
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
                    if (cm->cmsg_level != SOL_PACKET || cm->cmsg_type != PACKET_TX_TIMESTAMP) continue;
                    const struct sock_extended_err *ee = (const struct sock_extended_err *)CMSG_DATA(cm);
                    if (ee->ee_origin != SO_EE_ORIGIN_TXTIME) continue;
                    if (ee->ee_code == SO_EE_CODE_TXTIME_MISSED) atomic_fetch_add(&g_etf_missed, 1);
                    else if (ee->ee_code == SO_EE_CODE_TXTIME_INVALID_PARAM) atomic_fetch_add(&g_etf_invalid, 1);
                    else atomic_fetch_add(&g_etf_other, 1);
                }
            }
        }
""",
    "telemetry: drain errqueue with --no-tx-ts")

# 6. arguments
rep("""        } else if (strcmp(argv[i], "--eni") == 0 && i + 1 < argc) {   /* Phase 8.4 */""",
    """        } else if (strcmp(argv[i], "--link") == 0 && i + 1 < argc) {  /* Phase 8.5 */
            const char *l = argv[++i];
            if (strcmp(l, "etf") == 0) g_link_etf = 1;
            else if (strcmp(l, "af_packet") == 0) g_link_etf = 0;
            else { fprintf(stderr, "--link: af_packet or etf, not '%s'\\n", l); return 1; }
        } else if (strcmp(argv[i], "--etf-lead-us") == 0 && i + 1 < argc) {
            g_etf_lead_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--etf-asap-us") == 0 && i + 1 < argc) {
            g_etf_asap_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--etf-prio") == 0 && i + 1 < argc) {
            g_etf_prio = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--etf-ns-per-byte") == 0 && i + 1 < argc) {
            g_etf_ns_per_byte = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--eni") == 0 && i + 1 < argc) {   /* Phase 8.4 */""",
    "arguments")

rep("""            "       [--no-quarantine] [--no-reply-check] [--fresh-offset BYTE] [--fresh-stale CYCLES]\\n", argv[0]);""",
    """            "       [--no-quarantine] [--no-reply-check] [--fresh-offset BYTE] [--fresh-stale CYCLES]\\n"
            "       [--link af_packet|etf] [--etf-lead-us N] [--etf-asap-us N] [--etf-prio N] [--etf-ns-per-byte N]\\n", argv[0]);""",
    "usage")

# 7. enable txtime right after ecx_init (every later frame, config included, must carry a txtime)
rep("""    if (!ecx_init(&ctx, ifname)) {
        fprintf(stderr, "ecx_init on %s failed\\n", ifname);
        return 1;
    }
""",
    """    if (!ecx_init(&ctx, ifname)) {
        fprintf(stderr, "ecx_init on %s failed\\n", ifname);
        return 1;
    }
    /* Phase 8.5: --link etf. Before the first frame: with an ETF qdisc
     * every frame without a valid txtime is dropped, config included. */
    if (g_link_etf) {
#ifdef ECMASTER_SOEM_TXTIME_PATCH
        if (g_etf_lead_us <= 0 || g_etf_lead_us >= motion_cycle_us / 2 || g_etf_asap_us <= 0) {
            fprintf(stderr, "ecm_run: --link etf: need 0 < lead < cycle/2 and asap > 0 (lead %ld, asap %ld us)\\n",
                    g_etf_lead_us, g_etf_asap_us);
            ecx_close(&ctx); return 1;
        }
        if (!ecx_txtime_enable(&ctx.port, g_etf_prio, (int64_t)g_etf_asap_us * 1000,
                               (uint32_t)g_etf_ns_per_byte)) {
            perror("ecm_run: --link etf: SO_TXTIME");
            ecx_close(&ctx); return 1;
        }
        int q = etf_qdisc_state(ifname);
        fprintf(stderr, "ecm_run: link etf: lead %ld us, asap %ld us, SO_PRIORITY %d, %d ns/byte; qdisc on %s: %s\\n",
                g_etf_lead_us, g_etf_asap_us, g_etf_prio, g_etf_ns_per_byte, ifname,
                q == 2 ? "etf offload" : q == 1 ? "etf WITHOUT offload (software launch)" :
                q == 0 ? "NO etf -- txtime ignored, frames leave at wake time (lead early)" : "unknown");
#else
        fprintf(stderr, "ecm_run: --link etf needs SOEM with patches/soem-txtime.patch\\n");
        ecx_close(&ctx); return 1;
#endif
    }
""",
    "enable txtime after ecx_init")

# 8. RT loop: wake at next - lead, deadline from the wake target, TAI conversion
rep("""        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        struct timespec t_wake;
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        int64_t wake_jitter_ns = ts_diff_ns(&t_wake, &next);   /* quantity #1 */
""",
    """        /* Phase 8.5: --link etf wakes lead before the target `next`;
         * af_packet wakes at `next` (lead 0), unchanged. */
        struct timespec wake_at = next;
        const int64_t lead_ns = g_link_etf ? g_etf_lead_us * 1000L : 0;
        if (lead_ns) ns_to_ts(ts_to_ns(&next) - (uint64_t)lead_ns, &wake_at);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake_at, NULL);

        struct timespec t_wake;
        clock_gettime(CLOCK_MONOTONIC, &t_wake);
        int64_t wake_jitter_ns = ts_diff_ns(&t_wake, &wake_at);   /* quantity #1 */
        int64_t launch_tai_ns = 0;
        if (g_link_etf) {
            static int64_t tai_prev;
            int64_t off = tai_minus_mono_ns();
            if (tai_prev != 0) {
                int64_t d = off - tai_prev; if (d < 0) d = -d;
                if (d > TAI_STEP_NS) { g_tai_steps++; if (d > g_tai_step_max) g_tai_step_max = d; }
            }
            tai_prev = off;
            launch_tai_ns = (int64_t)ts_to_ns(&next) + off;
        }
""",
    "RT loop: wake / TAI")

rep("""        g_tick_deadline_ns = ts_to_ns(&next) + (uint64_t)(motion_cycle_us * 1000L) - RX_GUARD_NS;""",
    """        g_tick_deadline_ns = ts_to_ns(&next) - (uint64_t)lead_ns
                           + (uint64_t)(motion_cycle_us * 1000L) - RX_GUARD_NS;   /* 8.5: from the wake target */""",
    "RT loop: deadline")

rep("""        service_group(&motion, tick, &motion_prep_send_ns, &motion_total_ns, &motion_rx_ts_ns, &motion_wkc);""",
    """#ifdef ECMASTER_SOEM_TXTIME_PATCH
        if (g_link_etf) ecx_txtime_set_next(&ctx.port, launch_tai_ns);   /* 8.5: only the motion frame */
#endif
        service_group(&motion, tick, &motion_prep_send_ns, &motion_total_ns, &motion_rx_ts_ns, &motion_wkc);""",
    "RT loop: txtime for motion")

rep("""            dc_adjust_ns = ecm_dc_update(&g_dc, (uint64_t)ctx.DCtime, motion.last_send_ns);""",
    """            dc_adjust_ns = ecm_dc_update(&g_dc, (uint64_t)ctx.DCtime,
                                         g_link_etf ? ts_to_ns(&next) : motion.last_send_ns);   /* 8.5: launch time */""",
    "RT loop: DC host time")

# 9. summary
rep("""        fprintf(stderr, "  [DC] disabled\\n");
    }
""",
    """        fprintf(stderr, "  [DC] disabled\\n");
    }

    /* ---- Phase 8.5: link backend ---- */
    if (g_link_etf) {
#ifdef ECMASTER_SOEM_TXTIME_PATCH
        fprintf(stderr, "  [LINK] etf: lead=%ld us asap=%ld us; SOEM: late(sent asap)=%u send_err=%u; "
                "ETF drops (error queue): missed=%lu invalid=%lu other=%lu; TAI steps=%" PRIu64 " (max %.1f us)\\n",
                g_etf_lead_us, g_etf_asap_us, ctx.port.txtime_late, ctx.port.txtime_send_err,
                atomic_load(&g_etf_missed), atomic_load(&g_etf_invalid), atomic_load(&g_etf_other),
                g_tai_steps, g_tai_step_max / 1000.0);
#endif
    } else {
        fprintf(stderr, "  [LINK] af_packet\\n");
    }
""",
    "summary")

shutil.copy2(C, C + ".pre-etf")
open(C, "w").write(src)
print(f"{C}: {len(edits)} edits: " + ", ".join(edits))

# Makefile: rebuild when libsoem changes (a SOEM patch did not trigger a rebuild, 29/9)
mk = open(MK).read()
old = "$(TARGET): ecm_run.c $(TELEMETRY_SRC) $(MAILBOX_SRC) $(DC_SRC) $(DIAG_SRC) $(POLICY_SRC) $(ENI_SRC)\n"
if "$(filter %.c,$^)" in mk:
    print(f"{MK}: already patched")
elif old in mk:
    mk = mk.replace(old, old.rstrip("\n") + " $(SOEM_LIB)\n", 1)
    mk = mk.replace("\t$(CC) $(CFLAGS) -o $@ $^ $(SOEM_INC)", "\t$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(SOEM_INC)", 1)
    shutil.copy2(MK, MK + ".pre-etf")
    open(MK, "w").write(mk)
    print(f"{MK}: libsoem is now a prerequisite")
else:
    print(f"{MK}: target line not found, not changed (use make -B after SOEM changes)")
