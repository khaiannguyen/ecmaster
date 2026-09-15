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
