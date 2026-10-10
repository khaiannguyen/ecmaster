/* ecm_sha256.h -- Phase 11.2: SHA-256 (FIPS 180-4) for `info` (which ENI is
 * loaded), no library dependency. Tested against the FIPS / NIST vectors. */
#ifndef ECM_SHA256_H
#define ECM_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t  buf[64];
    size_t   nbuf;
} ecm_sha256_t;

void ecm_sha256_init(ecm_sha256_t *c);
void ecm_sha256_update(ecm_sha256_t *c, const void *data, size_t len);
void ecm_sha256_final(ecm_sha256_t *c, uint8_t out[32]);

/* 64 hex digits + NUL of the file's content. 0 ok, -errno. */
int ecm_sha256_file(const char *path, char hex[65]);

#endif /* ECM_SHA256_H */
