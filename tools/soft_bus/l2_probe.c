/* ==========================================================================
 * l2_probe.c — Verifies L2-01..06 (ESM state transitions, Phase 3).
 *
 * Runs a fixed sequence of state requests via ecx_writestate/ecx_statecheck,
 * printing AL Status / AL Status Code after each one in a MACHINE-READABLE
 * form (prefix "L2:") for a grading script, plus a human-readable summary.
 *
 * Build:  see the l2_probe target in the Makefile (same pattern as l1_probe)
 * Run:    sudo ./l2_probe veth_m
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>

#include "soem/soem.h"

static uint8 IOmap[8192];
static ecx_contextt ctx;

/* Requests slave 1 into `target` state, waits up to `timeout_us`, then
 * prints AL status/code either way (success or timeout — the caller decides
 * whether that's expected). */
static void request_state(int target, int timeout_us, const char *label)
{
    ctx.slavelist[1].state = target;
    ecx_writestate(&ctx, 1);
    ecx_statecheck(&ctx, 1, target, timeout_us);

    ecx_readstate(&ctx);
    int state  = ctx.slavelist[1].state & 0x0F;
    int haderr = (ctx.slavelist[1].state & 0x10) ? 1 : 0;
    int code   = ctx.slavelist[1].ALstatuscode;

    printf("L2:REQ=%s TARGET=0x%02x STATE=0x%02x ERROR=%d ALSTATUSCODE=0x%04x (%s)\n",
           label, target, state, haderr, code, ec_ALstatuscode2string((uint16)code));
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interface>\n", argv[0]);
        return 1;
    }
    const char *ifname = argv[1];

    if (!ecx_init(&ctx, ifname)) {
        printf("L2:INIT_FAIL\n");
        return 1;
    }
    int wc = ecx_config_init(&ctx);
    if (wc <= 0) {
        printf("L2:SLAVECOUNT=0\n");
        ecx_close(&ctx);
        return 1;
    }
    ecx_config_map_group(&ctx, IOmap, 0);
    while (ctx.ecaterror) printf("%s", ecx_elist2string(&ctx));

    ecx_readstate(&ctx);
    printf("L2:SLAVECOUNT=%d\n", ctx.slavecount);
    printf("L2:INITIAL_STATE=0x%02x\n", ctx.slavelist[1].state);

    /* ---- L2-01/02/03: sequential valid transitions ---- */
    request_state(EC_STATE_PRE_OP, EC_TIMEOUTSTATE, "INIT_TO_PREOP");
    request_state(EC_STATE_SAFE_OP, EC_TIMEOUTSTATE, "PREOP_TO_SAFEOP");

    /* pause here so an external `kill -USR1` can be sent precisely before
    * the OP request below, for manual L2-05 verification */
    fprintf(stderr, "L2: pausing 5s before SAFEOP_TO_OP -- send SIGUSR1 to soft_bus now if testing L2-05\n");
    sleep(5);

    /* Send one real process-data cycle BEFORE requesting OP, so this run
     * covers the "happy path" (L2-01..03) rather than L2-04. */
    ecx_send_processdata(&ctx);
    ecx_receive_processdata(&ctx, EC_TIMEOUTRET);
    request_state(EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE, "SAFEOP_TO_OP");

    /* ---- back down to SAFEOP, then attempt L2-04 for real ---- */
    request_state(EC_STATE_SAFE_OP, EC_TIMEOUTSTATE, "OP_TO_SAFEOP");
    request_state(EC_STATE_OPERATIONAL, 200000, "SAFEOP_TO_OP_NO_PDO"); /* expect: rejected */

    /* ---- back to INIT, then attempt L2-06: direct INIT->SAFEOP ---- */
    request_state(EC_STATE_INIT, EC_TIMEOUTSTATE, "BACK_TO_INIT");
    request_state(EC_STATE_SAFE_OP, 200000, "INIT_TO_SAFEOP_DIRECT"); /* expect: rejected */

    printf("\n--- Human-readable summary ---\n");
    printf("Run run_l2_tests.sh against this output to grade L2-01..04 and L2-06.\n");
    printf("L2-05 (forced reject) is NOT covered here — needs a soft_bus-side hook.\n");

    ecx_close(&ctx);
    return 0;
}