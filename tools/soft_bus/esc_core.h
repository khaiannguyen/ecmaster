#ifndef ESC_CORE_H
#define ESC_CORE_H

#include <stdint.h>
#include <stddef.h>
#include "esc_types.h"

/* ==========================================================================
 * esc_core.h — API of the ESC logic layer.
 *
 * This layer does NOT touch sockets/network: it takes a frame buffer in
 * memory, mutates it in place, done. That is what makes it fully testable
 * offline (test_offline.c) — no veth, no root, no Jetson required.
 * ========================================================================== */

#define ETH_P_ECAT   0x88A4
#define ETH_HDR_LEN  14
#define EC_HDR_LEN   2
#define DG_HDR_LEN   10   /* cmd(1) idx(1) adp(2) ado(2) len_flags(2) irq(2) */
#define DG_WKC_LEN   2

/* Command code (Section I Table 6) */
enum {
    CMD_NOP  = 0x00,
    CMD_APRD = 0x01, CMD_APWR = 0x02, CMD_APRW = 0x03,
    CMD_FPRD = 0x04, CMD_FPWR = 0x05, CMD_FPRW = 0x06,
    CMD_BRD  = 0x07, CMD_BWR  = 0x08, CMD_BRW  = 0x09,
    CMD_LRD  = 0x0A, CMD_LWR  = 0x0B, CMD_LRW  = 0x0C,
};

/* Processes one full Ethernet frame (may contain several chained
 * datagrams). Fills in the data read and updates WKC directly in buf. */
void process_frame(esc_t *chain, int n, uint8_t *buf, size_t frame_len);

/* Processes a single datagram through the whole chain — exported so it can
 * be exercised directly by tests. */
void process_datagram(esc_t *chain, int n, uint8_t cmd,
                      uint16_t adp, uint16_t ado,
                      uint8_t *data, uint16_t dlen, uint16_t *wkc);

#endif /* ESC_CORE_H */
