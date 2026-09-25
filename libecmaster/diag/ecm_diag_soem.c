/* ==========================================================================
 * ecm_diag_soem.c — SOEM glue for ecm_diag (Phase 7.2). See ecm_diag_soem.h.
 * Frame building follows the pattern already used for the L6-05 0x0928
 * rewrite in l6_test.c: ecx_setupdatagram() for the first datagram (its data
 * sits at EC_HEADERSIZE), ecx_adddatagram() for the rest (returns the data
 * offset), WKC of datagram k at off[k] + len[k] in rxbuf[idx].
 * ========================================================================== */
#include <string.h>

#include "ecm_diag_soem.h"

#define REG_STADR   0x0010
#define REG_DLSTAT  0x0110
#define REG_ALSTAT  0x0130

static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void ecm_diag_soem_io_init(ecm_diag_io_t *io)
{
    memset(io, 0, sizeof(*io));
}

int ecm_diag_soem_send(ecx_contextt *ctx, ecm_diag_io_t *io, int clear_on_read)
{
    static uint8_t zero[ECM_DIAG_ERR_LEN];     /* FPRW write data: value ignored by the ESC */
    static uint8_t scratch[8];
    if (io->pending) return 0;
    int n = ctx->slavecount;
    if (n <= 0) return 0;
    if (n > ECM_DIAG_MAX_SLAVES) n = ECM_DIAG_MAX_SLAVES;

    int first = io->next_first < n ? io->next_first : 0;
    int count = n - first;
    if (count > ECM_DIAG_SOEM_SLAVES_PER_FRAME) count = ECM_DIAG_SOEM_SLAVES_PER_FRAME;
    io->next_first = (first + count >= n) ? 0 : first + count;

    ecx_portt *port = &ctx->port;
    uint8_t idx = ecx_getindex(port);
    int nd = 0;
    int total = 1 + 3 * count;

    ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_BRD, idx, 0, REG_ALSTAT, 2, scratch);
    io->off[nd] = EC_HEADERSIZE; io->len[nd++] = 2;
    for (int i = first; i < first + count; i++) {
        uint16_t adr = ctx->slavelist[i + 1].configadr;
        io->off[nd] = ecx_adddatagram(port, &(port->txbuf[idx]),
                                      clear_on_read ? EC_CMD_FPRW : EC_CMD_FPRD, idx, nd < total - 1,
                                      adr, ECM_DIAG_ERR_BASE, ECM_DIAG_ERR_LEN, zero);
        io->len[nd++] = ECM_DIAG_ERR_LEN;
        io->off[nd] = ecx_adddatagram(port, &(port->txbuf[idx]), EC_CMD_FPRD, idx, nd < total - 1,
                                      adr, REG_DLSTAT, 2, scratch);
        io->len[nd++] = 2;
        io->off[nd] = ecx_adddatagram(port, &(port->txbuf[idx]), EC_CMD_FPRD, idx, nd < total - 1,
                                      adr, REG_ALSTAT, 6, scratch);
        io->len[nd++] = 6;
    }
    io->nd = nd;
    io->idx = idx;
    io->first = first;
    io->count = count;
    io->clear_on_read = clear_on_read;
    io->age = 0;

    if (ecx_outframe_red(port, idx) < 0) {         /* nothing went out */
        ecx_setbufstat(port, idx, EC_BUF_EMPTY);
        io->frames_lost++;
        return 0;
    }
    io->pending = 1;
    io->frames_sent++;
    return 1;
}

static int16_t dg_wkc(const uint8_t *rx, const ecm_diag_io_t *io, int k)
{
    return (int16_t)le16(rx + io->off[k] + io->len[k]);
}

/* The reply must carry the datagrams we sent: same count, command, ADO,
 * length, and ADP for configured-address commands (BRD/APRD ADP is
 * incremented by every ESC on the way, so it is not compared). txbuf[idx]
 * still holds what was sent, with the Ethernet header in front. */
static int reply_matches(ecx_portt *port, const ecm_diag_io_t *io, const uint8_t *rx)
{
    const uint8_t *tx = port->txbuf[io->idx] + ETH_HEADERSIZE;
    if ((le16(rx) & 0x07FF) != (le16(tx) & 0x07FF)) return 0;      /* total length */
    for (int k = 0; k < io->nd; k++) {
        const uint8_t *hr = rx + io->off[k] - (EC_HEADERSIZE - EC_ELENGTHSIZE);
        const uint8_t *ht = tx + io->off[k] - (EC_HEADERSIZE - EC_ELENGTHSIZE);
        if (hr[0] != ht[0]) return 0;                                    /* command */
        if (le16(hr + 4) != le16(ht + 4)) return 0;                      /* ADO     */
        if ((le16(hr + 6) & 0x07FF) != io->len[k]) return 0;             /* length  */
        if (hr[0] != EC_CMD_BRD && le16(hr + 2) != le16(ht + 2)) return 0; /* ADP   */
    }
    return 1;
}

int ecm_diag_soem_collect(ecx_contextt *ctx, ecm_diag_io_t *io, ecm_diag_raw_t *out,
                          uint64_t now_ns, int max_age)
{
    if (!io->pending) return 0;
    ecx_portt *port = &ctx->port;

    memset(out, 0, offsetof(ecm_diag_raw_t, s));
    out->n = ctx->slavecount > ECM_DIAG_MAX_SLAVES ? ECM_DIAG_MAX_SLAVES : ctx->slavecount;
    out->first = io->first;
    out->count = io->count;
    out->clear_on_read = io->clear_on_read;
    out->t_ns = now_ns;

    /* EC_BUF_RCVD: SOEM's receive already copied the reply into rxbuf[idx]
     * (Ethernet header stripped). Read it from there: no socket access, no
     * ppoll wait (ecx_waitinframe would poll up to 50 us first). */
    if (port->rxbufstat[io->idx] != EC_BUF_RCVD) {
        if (++io->age < max_age) return 0;
        /* Give up. The index is released; a reply arriving later for it can
         * only be matched if the index is in state EC_BUF_TX again (the
         * EC_MAXBUF=16 reuse risk seen in Phase 6), which is why max_age
         * should stay small. */
        ecx_setbufstat(port, io->idx, EC_BUF_EMPTY);
        io->pending = 0;
        io->frames_lost++;
        out->seq = ++io->seq;
        out->frame_ok = 0;
        out->frames_lost = io->frames_lost;
        return -1;
    }

    const uint8_t *rx = port->rxbuf[io->idx];
    if (!reply_matches(port, io, rx)) {
        /* A late reply to an OLDER frame that used the same index (SOEM has
         * only EC_MAXBUF=16 indexes; an index released after a timeout is
         * handed out again). Seen in the Phase 7 sandbox: a process data
         * frame that timed out came back into the diagnostic buffer. Drop it
         * and keep waiting for our own reply. */
        io->foreign++;
        ecx_setbufstat(port, io->idx, EC_BUF_TX);
        if (++io->age < max_age) return 0;
        ecx_setbufstat(port, io->idx, EC_BUF_EMPTY);
        io->pending = 0;
        io->frames_lost++;
        out->seq = ++io->seq;
        out->frame_ok = 0;
        out->frames_lost = io->frames_lost;
        return -1;
    }
    out->seq = ++io->seq;
    out->frame_ok = 1;
    out->frames_lost = io->frames_lost;
    out->brd_count = dg_wkc(rx, io, 0);
    out->brd_al_or = le16(rx + io->off[0]);
    int k = 1;
    for (int i = io->first; i < io->first + io->count; i++) {
        ecm_diag_raw_slave_t *s = &out->s[i];
        s->station_addr = ctx->slavelist[i + 1].configadr;
        s->wkc_err = dg_wkc(rx, io, k);
        memcpy(s->err, rx + io->off[k], ECM_DIAG_ERR_LEN);
        k++;
        s->wkc_dl = dg_wkc(rx, io, k);
        s->dl_status = le16(rx + io->off[k]);
        k++;
        s->wkc_al = dg_wkc(rx, io, k);
        s->al_status = le16(rx + io->off[k]);
        s->al_code = le16(rx + io->off[k] + 4);
        k++;
    }
    ecx_setbufstat(port, io->idx, EC_BUF_EMPTY);
    io->pending = 0;
    return 1;
}

int ecm_diag_soem_read_positional(ecx_contextt *ctx, int n, ecm_diag_raw_t *out, uint64_t now_ns)
{
    ecx_portt *port = &ctx->port;
    uint16_t al = 0;
    memset(out, 0, sizeof(*out));
    int brd = ecx_BRD(port, 0, REG_ALSTAT, 2, &al, EC_TIMEOUTRET3);
    if (brd <= 0) return brd;
    if (n <= 0 || n > brd) n = brd;
    if (n > ECM_DIAG_MAX_SLAVES) n = ECM_DIAG_MAX_SLAVES;

    out->seq = 1;
    out->t_ns = now_ns;
    out->frame_ok = 1;
    out->clear_on_read = 0;
    out->n = n;
    out->first = 0;
    out->count = n;
    out->brd_count = brd;
    out->brd_al_or = (uint16_t)etohs(al);
    for (int i = 0; i < n; i++) {
        ecm_diag_raw_slave_t *s = &out->s[i];
        uint16_t adp = (uint16_t)(0 - i);        /* auto-increment: position i */
        uint8_t b[6] = { 0 };
        if (ecx_APRD(port, adp, REG_STADR, 2, b, EC_TIMEOUTRET) > 0) s->station_addr = le16(b);
        s->wkc_err = (int16_t)ecx_APRD(port, adp, ECM_DIAG_ERR_BASE, ECM_DIAG_ERR_LEN, s->err, EC_TIMEOUTRET);
        s->wkc_dl  = (int16_t)ecx_APRD(port, adp, REG_DLSTAT, 2, b, EC_TIMEOUTRET);
        s->dl_status = le16(b);
        memset(b, 0, sizeof(b));
        s->wkc_al  = (int16_t)ecx_APRD(port, adp, REG_ALSTAT, 6, b, EC_TIMEOUTRET);
        s->al_status = le16(b);
        s->al_code = le16(b + 4);
    }
    return brd;
}
