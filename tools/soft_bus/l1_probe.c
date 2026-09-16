/* ==========================================================================
 * l1_probe.c — Verifies L1-02, L1-03, L1-04, L1-05, L1-06.
 *
 * Why a dedicated app instead of just using slaveinfo:
 *   slaveinfo does NOT print ec_groupt.nsegments or IOsegment[] — the only
 *   two fields that show whether SOEM had to split the IOmap into more than
 *   one datagram (comment in ec_main.h: "IO segmentation list. Datagrams
 *   must not break SM in two"). That is the direct evidence L1-06 needs.
 *   slaveinfo also never calls send/receive_processdata, so it never
 *   observes a real process-data frame on the wire.
 *
 * Prints in a MACHINE-READABLE form (prefix "L1:") so run_l1_tests.sh can
 * grade it automatically, plus a human-readable summary at the end.
 *
 * Build:  gcc -O2 -Wall -o l1_probe l1_probe.c -I<SOEM>/include -lsoem -lpthread
 *         (see the l1_probe target in the Makefile)
 * Run:    sudo ./l1_probe veth_m
 *         or setcap cap_net_raw,cap_net_admin+ep ./l1_probe then run without sudo
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "soem/soem.h"

static uint8 IOmap[8192];
static ecx_contextt ctx;

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>   (e.g. %s veth_m)\n", argv[0], argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    if (!ecx_init(&ctx, ifname)) {
        printf("L1:INIT_FAIL\n");
        fprintf(stderr, "ecx_init on %s failed\n", ifname);
        return 1;
    }
    printf("ecx_init on %s succeeded.\n", ifname);

    int wc = ecx_config_init(&ctx);
    if (wc <= 0) {
        printf("L1:SLAVECOUNT=0\n");
        printf("No slaves found.\n");
        ecx_close(&ctx);
        return 1;
    }

    ec_groupt *group = &ctx.grouplist[0];
    ecx_config_map_group(&ctx, IOmap, 0);
    ecx_configdc(&ctx);
    while (ctx.ecaterror) printf("%s", ecx_elist2string(&ctx));

    /* ---------------- L1-01 / L1-04: slave count ---------------- */
    printf("L1:SLAVECOUNT=%d\n", ctx.slavecount);

    /* ---------------- L1-02 / L1-03 / L1-04: address + SII ---------------- */
    for (int i = 1; i <= ctx.slavecount; i++) {
        printf("L1:SLAVE=%d ADDR=0x%04x MAN=0x%08x ID=0x%08x REV=0x%08x\n",
               i,
               ctx.slavelist[i].configadr,
               (unsigned)ctx.slavelist[i].eep_man,
               (unsigned)ctx.slavelist[i].eep_id,
               (unsigned)ctx.slavelist[i].eep_rev);
    }

    /* ---------------- L1-05: per-slave IOmap offset ----------------
     * outputs/inputs are pointers into IOmap; the difference from the base
     * of IOmap is the offset. The script checks these regions don't overlap. */
    for (int i = 1; i <= ctx.slavecount; i++) {
        long out_off = ctx.slavelist[i].outputs
                     ? (long)(ctx.slavelist[i].outputs - IOmap) : -1;
        long in_off  = ctx.slavelist[i].inputs
                     ? (long)(ctx.slavelist[i].inputs - IOmap) : -1;
        printf("L1:IOMAP=%d OUT_OFF=%ld OUT_BITS=%d IN_OFF=%ld IN_BITS=%d\n",
               i, out_off, ctx.slavelist[i].Obits,
               in_off, ctx.slavelist[i].Ibits);
    }

    /* Configured FMMUs — cross-checked against the soft_bus side */
    for (int i = 1; i <= ctx.slavecount; i++) {
        for (int j = 0; j < ctx.slavelist[i].FMMUunused; j++) {
            printf("L1:FMMU=%d IDX=%d LOGSTART=0x%08x LOGLEN=%d PHYS=0x%04x TYPE=%d ACT=%d\n",
                   i, j,
                   (unsigned)etohl(ctx.slavelist[i].FMMU[j].LogStart),
                   etohs(ctx.slavelist[i].FMMU[j].LogLength),
                   etohs(ctx.slavelist[i].FMMU[j].PhysStart),
                   ctx.slavelist[i].FMMU[j].FMMUtype,
                   ctx.slavelist[i].FMMU[j].FMMUactive);
        }
    }

    /* ---------------- L1-06: frame segmentation ----------------
     * nsegments > 1 means the IOmap doesn't fit in one datagram and SOEM
     * had to split it. This is direct evidence, not inferred from a frame
     * count on the wire. */
    printf("L1:GROUP OBYTES=%u IBYTES=%u NSEGMENTS=%u\n",
           (unsigned)group->Obytes, (unsigned)group->Ibytes,
           (unsigned)group->nsegments);
    for (int s = 0; s < group->nsegments && s < EC_MAXIOSEGMENTS; s++) {
        printf("L1:SEGMENT=%d BYTES=%u\n", s, (unsigned)group->IOsegment[s]);
    }
    printf("L1:EXPECTED_WKC=%d\n", (group->outputsWKC * 2) + group->inputsWKC);

    /* ---------------- One real process-data cycle ----------------
     * Confirms the send/receive pipe actually works and WKC comes back sane. */
    ecx_send_processdata(&ctx);
    int rwkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
    printf("L1:PROCESSDATA_WKC=%d\n", rwkc);

    /* Final state */
    ecx_readstate(&ctx);
    printf("L1:STATE=0x%02x\n", ctx.slavelist[0].state);

    printf("\n--- Human-readable summary ---\n");
    printf("Slaves: %d, IOmap: %u byte out + %u byte in, segments: %u\n",
           ctx.slavecount, (unsigned)group->Obytes,
           (unsigned)group->Ibytes, (unsigned)group->nsegments);

    ecx_close(&ctx);
    return 0;
}
