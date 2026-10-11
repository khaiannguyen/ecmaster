/* ecm_api_view.h -- Phase 11.3: ecm_diag as an API client (see ecm_api_view.c) */
#ifndef ECM_API_VIEW_H
#define ECM_API_VIEW_H

#include <stddef.h>

/* stats.json: 0 ok, -1 missing, -2 not a schema-1 document. age in s (same machine). */
int ecm_view_load(const char *path, double *age, int *final);
/* print it; exit code: 0 healthy, 1 ERROR (error finding, bus LOST/RECOVER, axis in
 * fault or with an axis error), 2 stale or final (ecm_run not running) */
int ecm_view_print(const char *path, double age, int final, double max_age, int json);

/* one request line -> reply line. 0 ok:true, -1 ok:false, -2 cannot connect, -4 I/O */
int ecm_view_ctl(const char *sock, const char *request, char *reply, size_t cap);
int ecm_view_ctl_alive(const char *sock);
/* `sdo read SLAVE INDEX SUB [TYPE]` / `sdo write SLAVE INDEX SUB TYPE VALUE`;
 * -1 = wrong arguments, else an exit code (0 ok, 1 refused, 2 no socket, 4 I/O) */
int ecm_view_sdo(const char *sock, int write, int argc, char **argv, int json);
/* `info` / `diag_dump` */
int ecm_view_simple(const char *sock, const char *cmd, int json);

#endif /* ECM_API_VIEW_H */
