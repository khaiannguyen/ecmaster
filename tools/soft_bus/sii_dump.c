/* sii_dump.c -- write the SII image that soft_bus generates for one node,
 * as raw little-endian bytes, so it can be compared byte-for-byte with what
 * a master reads back (IgH: ethercat sii_read). GD8 X-01a.
 * Usage: ./sii_dump [pdo_size] [--coe-pdo-od|--coe-ca] > sii.bin
 *        ./sii_dump --profile FILE > sii.bin          (GD9.9)
 * (GD9.3: the flags as for soft_bus; --coe-ca adds the General category) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esc_types.h"
#include "esc_core.h"
#include "esc_profile.h"

int main(int argc, char **argv)
{
    int pdo = 4, pdo_od = 0, ca = 0;
    const char *prof = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) prof = argv[++i];
        else if (!strcmp(argv[i], "--coe-pdo-od")) pdo_od = 1;
        else if (!strcmp(argv[i], "--coe-ca")) ca = 1;
        else pdo = atoi(argv[i]);
    }
    esc_t *e = calloc(1, sizeof(*e));
    if (!e) return 1;
    esc_init(e, 0, (uint16_t)pdo);
    if (pdo_od || ca) esc_set_coe_features(e, pdo_od, ca);
    if (prof) {
        char err[512];
        esc_profile_t *p = esc_prof_load(prof, err, sizeof(err));
        if (!p) { fprintf(stderr, "sii_dump: %s\n", err); return 1; }
        if (esc_prof_attach(e, p)) { fprintf(stderr, "sii_dump: SII image too large\n"); return 1; }
    }
    for (size_t i = 0; i < e->sii_image_words; i++) {
        uint16_t w = e->sii_image_buf[i];
        putchar(w & 0xFF);            /* SII words are little-endian on the wire */
        putchar((w >> 8) & 0xFF);
    }
    fprintf(stderr, "sii_dump: %zu words (%zu bytes), pdo_size=%d%s\n",
            e->sii_image_words, e->sii_image_words * 2, pdo,
            prof ? " --profile" : ca ? " --coe-ca" : pdo_od ? " --coe-pdo-od" : "");
    free(e);
    return 0;
}