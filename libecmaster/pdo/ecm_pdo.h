#ifndef ECM_PDO_H
#define ECM_PDO_H

#include <stddef.h>
#include <stdint.h>

/* ==========================================================================
 * ecm_pdo.h — Phase 9.10: bind process data by (slave, index, subindex).
 * Pure logic, no SOEM: unit-tested offline (test_pdo_offline.c).
 *
 * SOEM builds the IOmap from bit SIZES only; it keeps no list of the PDO
 * entries, so "where is 0x6041:00 of slave 2" has to be answered here:
 *
 *   table   every mapped entry of every slave, in mapping order: slave,
 *           direction, PDO, index, sub, bit length, bit offset inside the
 *           slave's own outputs/inputs. Built from one of two sources:
 *             - the ENI (.enicfg "pdo" records, ecm_eni_pdo_table())
 *             - a scan of the bus at PREOP (ecm_pdo_soem.h): CoE 0x1C12/
 *               0x1C13 + 0x16xx/0x1Axx, SII PDO categories as fallback
 *   loc     where each slave's outputs/inputs start in its group's IOmap
 *           (bit offset), filled from SOEM after ecx_config_map_group()
 *   handle  ecm_pdo_bind(): group, direction, absolute bit offset, length.
 *           ecm_pdo_get/set read and write that many bits of a buffer --
 *           entries need not be byte aligned (bit-sized digital I/O).
 *
 * libecmaster never interprets an object: 0x6041 is just a number here
 * (the meaning belongs to the CiA402 layer of Phase 10).
 * ========================================================================== */

#define ECM_PDO_MAX_ENTRIES 1024
#define ECM_PDO_MAX_SLAVES  64

typedef enum { ECM_PDO_OUT = 0, ECM_PDO_IN = 1 } ecm_pdo_dir_t;

typedef struct {
    uint16_t slave;          /* 1-based                                      */
    uint8_t  dir;            /* ecm_pdo_dir_t                                */
    uint16_t pdo;            /* 0x16xx / 0x1Axx                              */
    uint16_t index;          /* 0 = padding gap (not bindable)               */
    uint8_t  sub;
    uint16_t bits;
    uint32_t bit_off;        /* inside the slave's outputs or inputs         */
} ecm_pdo_entry_t;

typedef struct {
    int             n;
    ecm_pdo_entry_t e[ECM_PDO_MAX_ENTRIES];
    uint32_t        bits[2][ECM_PDO_MAX_SLAVES + 1];   /* [dir][slave] total */
} ecm_pdo_table_t;

void ecm_pdo_table_init(ecm_pdo_table_t *t);

/* Append the next entry of (slave, dir) in mapping order; its bit offset is
 * the running total of that slave/direction. 0 ok, -1 full / bad args. */
int ecm_pdo_add(ecm_pdo_table_t *t, int slave, ecm_pdo_dir_t dir, uint16_t pdo,
                uint16_t index, uint8_t sub, uint16_t bits);

/* Same entries in the same order (PDO numbers included)? Returns the number
 * of differences, each one line in err. B-01: ENI table == scan table. */
int ecm_pdo_table_compare(const ecm_pdo_table_t *a, const ecm_pdo_table_t *b,
                          char *err, size_t errlen);

/* Totals against what SOEM mapped (Obits/Ibits per slave, 1-based arrays of
 * n+1). Returns mismatches, each one line in err. */
int ecm_pdo_check_sizes(const ecm_pdo_table_t *t, int n, const uint32_t *obits,
                        const uint32_t *ibits, char *err, size_t errlen);

typedef struct {
    uint8_t  group;
    uint32_t out_bit;        /* first output bit of the slave in the group's IOmap */
    uint32_t in_bit;         /* first input bit                                     */
} ecm_pdo_loc_t;             /* array index = slave (1-based)                       */

typedef struct {
    uint16_t slave;
    uint8_t  group, dir;
    uint32_t bit;            /* absolute bit offset in the group's IOmap  */
    uint16_t bits;
} ecm_pdo_handle_t;

/* 0 ok; -1 if the entry is not mapped (err names the slave and lists what
 * it does map), or is longer than 64 bit (use ecm_pdo_copy_*). */
int ecm_pdo_bind(const ecm_pdo_table_t *t, const ecm_pdo_loc_t *loc, int slave,
                 uint16_t index, uint8_t sub, ecm_pdo_handle_t *h, char *err, size_t errlen);

/* Little endian, any bit offset, 1..64 bits. RT-safe (no syscalls). */
uint64_t ecm_pdo_get(const ecm_pdo_handle_t *h, const uint8_t *iomap);
void     ecm_pdo_set(const ecm_pdo_handle_t *h, uint8_t *iomap, uint64_t v);

/* "3:0x6041:0" -> slave, index, sub. 0 ok. */
int ecm_pdo_parse_ref(const char *s, int *slave, uint16_t *index, uint8_t *sub);

/* Phase 10.1 -- when a bind needs the bus's own table.
 *
 * N-03 (Phase 9.9): a vendor device whose mapping differs from its ESI but has
 * the same size passes every size check; an ENI-only table then binds the
 * wrong bytes silently. The ESI/ENI of a slave we did not build is not
 * evidence of what the device maps, so binding into it by (index, sub)
 * needs the table scanned from the bus (--pdo-scan), where ENI == bus is
 * then enforced.
 *
 *   vendor   SII vendor id per slave, 1-based, n+1 entries
 *   bound    slaves the application binds into (repeats allowed)
 *   own      vendor ids of slaves this project builds (ESI written here)
 *
 * Returns the number of distinct bound slaves of another vendor (0 = no
 * scan needed); each named in err as "slave S (vendor 0x........)". */
int ecm_pdo_scan_required(const uint32_t *vendor, int n, const uint16_t *bound, int nbound,
                          const uint32_t *own, int nown, char *err, size_t errlen);

/* Vendor id of the soft_bus default SII and the P1 draft ESI (placeholder,
 * not a registered ETG id); ecm_run --pdo-own-vendor replaces it. */
#define ECM_PDO_OWN_VENDOR_DEFAULT 0x00000499u

/* One line per entry, for logs and tests. */
size_t ecm_pdo_table_format(const ecm_pdo_table_t *t, char *buf, size_t cap);

#endif /* ECM_PDO_H */
