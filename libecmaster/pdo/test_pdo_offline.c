/*
 * test_pdo_offline.c -- Phase 9.10 B-03 / B-04 and the table itself, offline;
 * Phase 10.1 B-05: which binds need the bus's own table.
 */
#include "ecm_pdo.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
static void check(const char *name, long got, long want)
{
    int ok = got == want;
    printf("  [%s] %-62s = %ld%s\n", ok ? "PASS" : "FAIL", name, got, ok ? "" : " (unexpected)");
    if (ok) g_pass++; else g_fail++;
}

static ecm_pdo_table_t T, U;
static ecm_pdo_loc_t L[ECM_PDO_MAX_SLAVES + 1];
static char err[2048];

/* A CiA402-like slave (P1 draft) and a slave with bit-sized I/O. */
static void build(ecm_pdo_table_t *t)
{
    ecm_pdo_table_init(t);
    ecm_pdo_add(t, 1, ECM_PDO_OUT, 0x1600, 0x6040, 0, 16);
    ecm_pdo_add(t, 1, ECM_PDO_OUT, 0x1600, 0x6060, 0, 8);
    ecm_pdo_add(t, 1, ECM_PDO_OUT, 0x1600, 0x607A, 0, 32);
    ecm_pdo_add(t, 1, ECM_PDO_IN, 0x1A00, 0x6041, 0, 16);
    ecm_pdo_add(t, 1, ECM_PDO_IN, 0x1A00, 0x6064, 0, 32);
    /* slave 2: 3 output bits, a 5 bit gap, a 12 bit value (bit-oriented) */
    ecm_pdo_add(t, 2, ECM_PDO_OUT, 0x1600, 0x7000, 1, 1);
    ecm_pdo_add(t, 2, ECM_PDO_OUT, 0x1600, 0x7000, 2, 1);
    ecm_pdo_add(t, 2, ECM_PDO_OUT, 0x1600, 0x7000, 3, 1);
    ecm_pdo_add(t, 2, ECM_PDO_OUT, 0x1600, 0x0000, 0, 5);
    ecm_pdo_add(t, 2, ECM_PDO_OUT, 0x1600, 0x7010, 1, 12);
    ecm_pdo_add(t, 2, ECM_PDO_IN, 0x1A00, 0x6000, 1, 64);
}

int main(void)
{
    printf("[table]\n");
    build(&T);
    check("11 entries", T.n, 11);
    check("slave 1 out bits 56", T.bits[ECM_PDO_OUT][1], 56);
    check("0x607A at bit 24", T.e[2].bit_off, 24);
    check("slave 2 out bits 20", T.bits[ECM_PDO_OUT][2], 20);
    check("0x7010:01 after the gap at bit 8", T.e[9].bit_off, 8);
    check("bits 0 refused", ecm_pdo_add(&T, 1, ECM_PDO_OUT, 0, 1, 1, 0), -1);
    check("slave 0 refused", ecm_pdo_add(&T, 0, ECM_PDO_OUT, 0, 1, 1, 8), -1);

    printf("[sizes against SOEM]\n");
    uint32_t ob[3] = { 0, 56, 20 }, ib[3] = { 0, 48, 64 };
    check("totals match", ecm_pdo_check_sizes(&T, 2, ob, ib, err, sizeof(err)), 0);
    ib[1] = 64;
    check("slave 1 in differs -> 1, named", ecm_pdo_check_sizes(&T, 2, ob, ib, err, sizeof(err)) == 1 &&
          strstr(err, "slave 1: PDO table 56 out / 48 in bits, mapped 56 / 64") != NULL, 1);

    printf("[compare, B-01 offline]\n");
    build(&U);
    check("same tables -> 0", ecm_pdo_table_compare(&T, &U, err, sizeof(err)), 0);
    U.e[3].index = 0x6061;
    check("one entry differs -> 1, named", ecm_pdo_table_compare(&T, &U, err, sizeof(err)) == 1 &&
          strstr(err, "entry 3:") != NULL, 1);
    build(&U);
    ecm_pdo_add(&U, 2, ECM_PDO_IN, 0x1A00, 0x6000, 2, 8);
    check("extra entry -> count differs", ecm_pdo_table_compare(&T, &U, err, sizeof(err)) == 1 &&
          strstr(err, "entry count 11 vs 12") != NULL, 1);

    printf("[bind]\n");
    memset(L, 0, sizeof(L));
    L[1] = (ecm_pdo_loc_t){ .group = 0, .out_bit = 0, .in_bit = 13 * 8 };
    L[2] = (ecm_pdo_loc_t){ .group = 0, .out_bit = 7 * 8 + 3, .in_bit = 19 * 8 };  /* odd start bit */
    ecm_pdo_handle_t h;
    check("bind 1:0x607A:0", ecm_pdo_bind(&T, L, 1, 0x607A, 0, &h, err, sizeof(err)), 0);
    check("  out, bit 24, 32 bits", h.dir == ECM_PDO_OUT && h.bit == 24 && h.bits == 32, 1);
    check("bind 1:0x6041:0 -> in at loc", ecm_pdo_bind(&T, L, 1, 0x6041, 0, &h, err, sizeof(err)) == 0 &&
          h.dir == ECM_PDO_IN && h.bit == 104, 1);
    /* B-03 */
    int rc = ecm_pdo_bind(&T, L, 1, 0x6077, 0, &h, err, sizeof(err));
    check("B-03 bind 1:0x6077:0 (not mapped) -> -1", rc, -1);
    check("  message names slave and lists its entries",
          strstr(err, "slave 1 does not map 0x6077:00; it maps: out 0x6040:00 out 0x6060:00") != NULL, 1);
    check("B-03 padding is not bindable", ecm_pdo_bind(&T, L, 2, 0x0000, 0, &h, err, sizeof(err)), -1);
    check("B-03 slave 3 maps nothing", ecm_pdo_bind(&T, L, 3, 0x6041, 0, &h, err, sizeof(err)) == -1 &&
          strstr(err, "nothing") != NULL, 1);

    printf("[get/set, B-04 offline: bit offsets that are not byte aligned]\n");
    uint8_t io[32];
    memset(io, 0xAA, sizeof(io));
    ecm_pdo_handle_t b2, v12;
    ecm_pdo_bind(&T, L, 2, 0x7000, 2, &b2, err, sizeof(err));
    ecm_pdo_bind(&T, L, 2, 0x7010, 1, &v12, err, sizeof(err));
    check("2:0x7000:02 at bit 60", b2.bit, 60);
    check("2:0x7010:01 at bit 67, 12 bit", v12.bit == 67 && v12.bits == 12, 1);
    ecm_pdo_set(&v12, io, 0xABC);
    check("12-bit value read back", (long)ecm_pdo_get(&v12, io), 0xABC);
    ecm_pdo_set(&b2, io, 0);
    check("1-bit cleared", (long)ecm_pdo_get(&b2, io), 0);
    check("  12-bit neighbour untouched", (long)ecm_pdo_get(&v12, io), 0xABC);
    check("  bits around untouched: byte 7 = 0xAA with bit 4 cleared", io[7], 0xAA & ~0x10);
    check("  byte 10 above the value untouched (0xAA high bits)", io[10] & 0xF8, 0xAA & 0xF8);
    ecm_pdo_set(&b2, io, 1);
    check("1-bit set", (long)ecm_pdo_get(&b2, io), 1);
    ecm_pdo_set(&v12, io, 0x1FFFF);
    check("value wider than 12 bit is cut to 12", (long)ecm_pdo_get(&v12, io), 0xFFF);
    ecm_pdo_handle_t w64;
    ecm_pdo_bind(&T, L, 2, 0x6000, 1, &w64, err, sizeof(err));
    ecm_pdo_set(&w64, io, 0x0123456789ABCDEFull);
    check("64-bit little endian at byte 19", io[19] == 0xEF && io[26] == 0x01, 1);
    check("64-bit read back", (long)(ecm_pdo_get(&w64, io) == 0x0123456789ABCDEFull), 1);
    ecm_pdo_handle_t odd64 = { .bit = 3, .bits = 64 };
    ecm_pdo_set(&odd64, io, 0xFEDCBA9876543210ull);
    check("64-bit at bit 3 read back", (long)(ecm_pdo_get(&odd64, io) == 0xFEDCBA9876543210ull), 1);

    printf("[refs]\n");
    int s; uint16_t ix; uint8_t sb;
    check("'3:0x6041:0'", ecm_pdo_parse_ref("3:0x6041:0", &s, &ix, &sb) == 0 && s == 3 && ix == 0x6041 && sb == 0, 1);
    check("'3:0x6041' refused", ecm_pdo_parse_ref("3:0x6041", &s, &ix, &sb), -1);
    check("'0:0x6041:0' refused", ecm_pdo_parse_ref("0:0x6041:0", &s, &ix, &sb), -1);
    static char buf[4096];
    ecm_pdo_table_format(&T, buf, sizeof(buf));
    check("format line", strstr(buf, "pdo slave 1 out PDO 0x1600 0x607A:00 32 bit @24\n") != NULL, 1);

    printf("[B-05 scan required for a slave of another vendor]\n");
    {
        /* bus: 1 = P1 (own), 2 = IS620N, 3 = IS620N, 4 = own */
        const uint32_t vend[5] = { 0, 0x00000499u, 0x00100000u, 0x00100000u, 0x00000499u };
        const uint32_t own[1]  = { ECM_PDO_OWN_VENDOR_DEFAULT };
        const uint16_t b_own[3] = { 1, 4, 1 };
        const uint16_t b_mix[5] = { 1, 2, 2, 3, 2 };
        const uint16_t b_bad[2] = { 0, 9 };
        check("B-05 own slaves only -> 0", ecm_pdo_scan_required(vend, 4, b_own, 3, own, 1, err, sizeof(err)), 0);
        check("B-05 own slaves only: err empty", err[0] == '\0', 1);
        check("B-05 two vendor slaves, repeats counted once -> 2",
              ecm_pdo_scan_required(vend, 4, b_mix, 5, own, 1, err, sizeof(err)), 2);
        check("B-05 names both with vendor id",
              strcmp(err, "slave 2 (vendor 0x00100000), slave 3 (vendor 0x00100000)") == 0, 1);
        const uint32_t own2[2] = { ECM_PDO_OWN_VENDOR_DEFAULT, 0x00100000u };
        check("B-05 vendor declared own (--pdo-own-vendor) -> 0",
              ecm_pdo_scan_required(vend, 4, b_mix, 5, own2, 2, err, sizeof(err)), 0);
        check("B-05 no own vendor at all: every bound slave -> 2",
              ecm_pdo_scan_required(vend, 4, b_own, 3, own, 0, err, sizeof(err)), 2);
        check("B-05 out-of-range slaves ignored -> 0",
              ecm_pdo_scan_required(vend, 4, b_bad, 2, own, 1, err, sizeof(err)), 0);
        char tiny[12];
        check("B-05 short err buffer stays terminated",
              ecm_pdo_scan_required(vend, 4, b_mix, 5, own, 1, tiny, sizeof(tiny)) == 2 && strlen(tiny) < sizeof(tiny), 1);
    }

    printf("\nRESULT: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
