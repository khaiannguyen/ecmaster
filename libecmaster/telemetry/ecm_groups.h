#pragma once

/* Single source of truth for the EtherCAT process-data group identifiers.
   Both apps/ecm_run/ecm_run.c and libecmaster/telemetry/turnaround.c
   include this instead of each defining their own — see the Giai đoạn 4
   review note on avoiding two sources of truth for the same group id. */
#define GROUP_MOTION 1
#define GROUP_IO     2