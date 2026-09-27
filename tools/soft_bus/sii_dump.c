/* sii_dump.c -- write the SII image that soft_bus generates for one node,
 * as raw little-endian bytes, so it can be compared byte-for-byte with what
 * a master reads back (IgH: ethercat sii_read). GD8 X-01a.
 * Usage: ./sii_dump [pdo_size] > sii.bin */
#include <stdio.h>
#include <stdlib.h>
#include "esc_types.h"
#include "esc_core.h"

int main(int argc, char **argv)
{
    int pdo = (argc > 1) ? atoi(argv[1]) : 4;
    esc_t *e = calloc(1, sizeof(*e));
    if (!e) return 1;
    esc_init(e, 0, (uint16_t)pdo);
    for (size_t i = 0; i < e->sii_image_words; i++) {
        uint16_t w = e->sii_image_buf[i];
        putchar(w & 0xFF);            /* SII words are little-endian on the wire */
        putchar((w >> 8) & 0xFF);
    }
    fprintf(stderr, "sii_dump: %zu words (%zu bytes), pdo_size=%d\n",
            e->sii_image_words, e->sii_image_words * 2, pdo);
    free(e);
    return 0;
}