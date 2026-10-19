/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* Phase 12.4: interface only, NOT implemented -- no file defines these ops
 * and none includes this header. The ETF backend of master-v1.0 is the
 * SOEM patch patches/soem-txtime.patch; AF_XDP (Phase 11.8) is deferred. */
#ifndef ECM_LINK_OPS_H
#define ECM_LINK_OPS_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int (*open)(const char *ifname, void *cfg);
    int (*send)(void *ctx, const void *frame, size_t len, uint64_t txtime_ns);
    int (*recv)(void *ctx, void *buf, size_t cap, uint64_t *rx_hw_ts);
    void (*close)(void *ctx);
} ecm_link_ops_t;

extern const ecm_link_ops_t ecm_link_af_packet;
extern const ecm_link_ops_t ecm_link_af_packet_etf;
extern const ecm_link_ops_t ecm_link_af_xdp;

#endif /* ECM_LINK_OPS_H */
