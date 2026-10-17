/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* test_ctl_offline.c -- Phase 11.2: ctl.sock without SOEM or a bus.
 *
 *   ./test_ctl_offline       JSON tokenizer, SHA-256, value codec, allowlist,
 *                            request handling against a fake backend (every
 *                            refusal of sdo_write, rate limit), the server
 *                            over a real AF_UNIX socket
 *
 * Built with -DECM_CTL_BROKEN=<bit> (make negctl) one protection is removed
 * and the check named for it MUST fail. */
#define _GNU_SOURCE
#include "ecm_ctl.h"
#include "ecm_json.h"
#include "ecm_sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef ECM_CTL_BROKEN
#define ECM_CTL_BROKEN 0
#endif
static int g_pass, g_fail;
#define CHECK(c, name) do { if (c) g_pass++; else { g_fail++; printf("  FAIL [%s] %s:%d\n", name, __FILE__, __LINE__); } } while (0)

/* ------------------------------------------------------------- JSON ---- */
static int parse(const char *s, ecm_jtok_t *t, int max) { return ecm_json_parse(s, strlen(s), t, max, NULL); }

static void test_json(void)
{
    ecm_jtok_t t[64];
    const char *ok[] = { "{}", "[]", "0", "-1.5e3", "\"a\\n\\u0041\"", "{\"a\":[1,2,{\"b\":null}],\"c\":true}", " { \"x\" : 1 } \n" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) CHECK(parse(ok[i], t, 64) > 0, "json valid");
    const char *bad[] = { "", "{", "{\"a\"}", "{\"a\":1,}", "[1,]", "01", "1.", "-", "\"\\x\"", "\"a", "{} {}", "tru",
                          "{\"a\":1}x", "\"\x01\"", "{'a':1}", "[1 2]", "\"\\u12G4\"" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(parse(bad[i], t, 64) < 0, "json invalid");
    char deep[64] = "";
    for (int i = 0; i < 20; i++) strcat(deep, "[");
    for (int i = 0; i < 20; i++) strcat(deep, "]");
    CHECK(parse(deep, t, 64) == -3, "json depth limit");
    CHECK(parse("[1,2,3,4,5,6,7,8,9,10]", t, 5) == -2, "json token limit");

    const char *s = "{\"id\":7,\"cmd\":\"sdo_read\",\"nest\":{\"index\":\"0x10\"},\"index\":\"0x6061\",\"neg\":-5,\"big\":18446744073709551615,"
                    "\"oct\":\"010\",\"f\":2.5,\"s\":\"a\\\"b\"}";
    int n = parse(s, t, 64);
    CHECK(n > 0, "json parse request");
    int iv = ecm_json_get(s, t, n, 0, "index");
    uint64_t u = 0;
    int64_t i = 0;
    double d = 0;
    char buf[16];
    CHECK(iv > 0 && ecm_json_u64(s, &t[iv], &u) == 0 && u == 0x6061, "json get skips nested object");
    CHECK(ecm_json_i64(s, &t[ecm_json_get(s, t, n, 0, "neg")], &i) == 0 && i == -5, "json i64");
    CHECK(ecm_json_u64(s, &t[ecm_json_get(s, t, n, 0, "neg")], &u) == -1, "json u64 refuses negative");
    CHECK(ecm_json_u64(s, &t[ecm_json_get(s, t, n, 0, "big")], &u) == 0 && u == UINT64_MAX, "json u64 max");
    CHECK(ecm_json_i64(s, &t[ecm_json_get(s, t, n, 0, "big")], &i) == -1, "json i64 overflow");
    CHECK(ecm_json_u64(s, &t[ecm_json_get(s, t, n, 0, "oct")], &u) == -1, "json no octal");
    CHECK(ecm_json_i64(s, &t[ecm_json_get(s, t, n, 0, "f")], &i) == -1, "json int refuses 2.5");
    CHECK(ecm_json_f64(s, &t[ecm_json_get(s, t, n, 0, "f")], &d) == 0 && d == 2.5, "json f64");
    CHECK(ecm_json_str(s, &t[ecm_json_get(s, t, n, 0, "s")], buf, sizeof(buf)) == 0 && !strcmp(buf, "a\"b"), "json unescape");
    CHECK(ecm_json_get(s, t, n, 0, "absent") == -1, "json absent key");
}

/* ----------------------------------------------------------- SHA-256 --- */
static void hex(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }

static void test_sha256(void)
{
    struct { const char *in; const char *out; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (size_t k = 0; k < 3; k++) {
        ecm_sha256_t c;
        uint8_t d[32];
        char h[65];
        ecm_sha256_init(&c);
        ecm_sha256_update(&c, v[k].in, strlen(v[k].in));
        ecm_sha256_final(&c, d);
        hex(d, h);
        CHECK(!strcmp(h, v[k].out), "sha256 vector");
    }
    ecm_sha256_t c;
    uint8_t d[32];
    char h[65];
    ecm_sha256_init(&c);
    for (int i = 0; i < 1000000; i += 1000) { char a[1000]; memset(a, 'a', sizeof(a)); ecm_sha256_update(&c, a, sizeof(a)); }
    ecm_sha256_final(&c, d);
    hex(d, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256 million a");
    char p[] = "/tmp/ctl_sha_XXXXXX";
    int fd = mkstemp(p);
    if (fd >= 0) { if (write(fd, "abc", 3) != 3) {} close(fd); }
    CHECK(ecm_sha256_file(p, h) == 0 && !strcmp(h, v[1].out), "sha256 file");
    unlink(p);
    CHECK(ecm_sha256_file("/nonexistent/x", h) == -ENOENT, "sha256 missing file");
}

/* ------------------------------------------------------------- codec --- */
static ecm_ctl_val_t I(int64_t x) { return (ecm_ctl_val_t){ .kind = 0, .i = x }; }
static ecm_ctl_val_t U(uint64_t x) { return (ecm_ctl_val_t){ .kind = 1, .u = x }; }
static ecm_ctl_val_t D(double x) { return (ecm_ctl_val_t){ .kind = 2, .d = x }; }

static void test_codec(void)
{
    ecm_ctl_val_t v;
    CHECK(ecm_ctl_type_parse("u16") == ECM_CT_U16 && ecm_ctl_type_parse("x") < 0, "type parse");
    v = I(-128); CHECK(!ecm_ctl_val_fits(ECM_CT_I8, &v), "i8 -128 fits");
    v = I(-129); CHECK(ecm_ctl_val_fits(ECM_CT_I8, &v), "i8 -129 no");
    v = U(255);  CHECK(!ecm_ctl_val_fits(ECM_CT_U8, &v), "u8 255 fits");
    v = U(256);  CHECK(ecm_ctl_val_fits(ECM_CT_U8, &v), "u8 256 no");
    v = I(-1);   CHECK(ecm_ctl_val_fits(ECM_CT_U32, &v), "u32 -1 no");
    v = U((uint64_t)INT64_MAX + 1); CHECK(ecm_ctl_val_fits(ECM_CT_I64, &v), "i64 2^63 no");
    v = D(1e39); CHECK(ecm_ctl_val_fits(ECM_CT_F32, &v), "f32 1e39 no");
    uint8_t b[8];
    char o[64];
    v = I(-2); CHECK(!ecm_ctl_encode(ECM_CT_I16, &v, b) && b[0] == 0xFE && b[1] == 0xFF, "encode i16 LE");
    CHECK(ecm_ctl_decode_json(ECM_CT_I16, b, 2, o, sizeof(o)) && !strcmp(o, "-2"), "decode i16");
    v = U(0x12345678); CHECK(!ecm_ctl_encode(ECM_CT_U32, &v, b) && b[0] == 0x78 && b[3] == 0x12, "encode u32 LE");
    v = D(1.5); CHECK(!ecm_ctl_encode(ECM_CT_F32, &v, b) && ecm_ctl_decode_json(ECM_CT_F32, b, 4, o, sizeof(o)) && !strcmp(o, "1.5"), "f32 round trip");
    CHECK(ecm_ctl_decode_json(ECM_CT_U16, b, 3, o, sizeof(o)) == 0, "decode size mismatch -> 0");
    ecm_ctl_val_t a = I(-5), m = I(3);
    CHECK(ecm_ctl_val_cmp(ECM_CT_I32, &a, &m) < 0, "cmp signed");
    a = U(UINT64_MAX); m = U(1);
    CHECK(ecm_ctl_val_cmp(ECM_CT_U64, &a, &m) > 0, "cmp unsigned u64");
}

/* --------------------------------------------------------- fake backend --- */
#define VEND 0x00100000u
#define PROD 0x000C0108u

typedef struct {
    int      ready_ok;
    int      pdo_known;
    int      clamp_2008;          /* the drive stores min(value, 500) */
    uint32_t abort_next;
    uint16_t obj_2008;
    int      writes, reads;
} fake_t;

static fake_t F;

static int f_identity(void *c, int slave, uint32_t *v, uint32_t *p)
{
    (void)c;
    if (slave < 1 || slave > 2) return -1;
    *v = VEND; *p = PROD;
    return 0;
}

static int f_deny(void *c, int slave, uint16_t index, uint8_t sub, char *why, size_t n)
{
    (void)c; (void)slave; (void)sub;
    for (int a = 0; a < 8; a++)
        if (index == 0x6040 + 0x800 * a || index == 0x6060 + 0x800 * a) {
            snprintf(why, n, "CiA402 controlword / mode of operation of axis %d", a);
            return 1;
        }
    return 0;
}

static int f_rxpdo(void *c, int slave, uint16_t index, uint8_t sub)
{
    (void)c; (void)slave; (void)sub;
    if (!F.pdo_known) return -1;
    return index == 0x607F || index == 0x607A;
}

static int f_initcmd(void *c, int slave, uint16_t index, uint8_t sub) { (void)c; (void)slave; (void)sub; return index == 0x6072; }

static int f_ready(void *c, int slave, char *why, size_t n)
{
    (void)c; (void)slave;
    if (F.ready_ok) return 0;
    snprintf(why, n, "bus LOST");
    return 1;
}

static int f_read(void *c, int slave, uint16_t index, uint8_t sub, uint8_t *b, int *size, uint32_t *abort)
{
    (void)c; (void)slave; (void)sub;
    F.reads++;
    if (F.abort_next) { *abort = F.abort_next; F.abort_next = 0; return ECM_CTL_SDO_FAILED; }
    if (index == 0x6061) { b[0] = 8; *size = 1; return 0; }
    if (index == 0x2008) { b[0] = (uint8_t)F.obj_2008; b[1] = (uint8_t)(F.obj_2008 >> 8); *size = 2; return 0; }
    if (index == 0x1008) { memcpy(b, "IS620N", 6); *size = 6; return 0; }
    *abort = 0x06020000;
    return ECM_CTL_SDO_FAILED;
}

static int f_write(void *c, int slave, uint16_t index, uint8_t sub, const uint8_t *b, int size, uint32_t *abort)
{
    (void)c; (void)slave; (void)sub; (void)abort;
    F.writes++;
    if (index == 0x2008 && size == 2) {
        uint16_t v = (uint16_t)(b[0] | b[1] << 8);
        F.obj_2008 = F.clamp_2008 && v > 500 ? 500 : v;
        return 0;
    }
    return ECM_CTL_SDO_FAILED;
}

static const char *f_abort_text(uint32_t a) { return a == 0x06020000 ? "The object does not exist in the object directory" : "?"; }

static size_t f_info(void *c, char *b, size_t cap)
{
    (void)c;
    int r = snprintf(b, cap, "\"pid\":1,\"slaves\":[{\"pos\":1},{\"pos\":2}]");
    return r < 0 ? 0 : (size_t)r;
}

static int f_dump(void *c, char *b, size_t cap) { (void)c; snprintf(b, cap, "\"files\":[\"/tmp/x\"]"); return 0; }

static const ecm_ctl_backend_t BE = {
    .identity = f_identity, .deny = f_deny, .rxpdo = f_rxpdo, .initcmd = f_initcmd, .ready = f_ready,
    .sdo_read = f_read, .sdo_write = f_write, .abort_text = f_abort_text, .info = f_info, .dump = f_dump,
};

/* ----------------------------------------------------------- allowlist --- */
static int write_text(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(s, f);
    fclose(f);
    return 0;
}

static int load(const char *text, ecm_ctl_allowlist_t *al, char *err, size_t n)
{
    char p[] = "/tmp/ctl_allow_XXXXXX";
    int fd = mkstemp(p);
    if (fd < 0) return -2;
    close(fd);
    write_text(p, text);
    int r = ecm_ctl_allow_load(p, al, &BE, err, n);
    unlink(p);
    return r;
}

#define ENTRY(idx, typ, mn, mx) "{\"vendor\":\"0x00100000\",\"product\":\"0x000C0108\",\"index\":\"" idx "\",\"sub\":0,\"type\":\"" typ "\",\"min\":" mn ",\"max\":" mx "}"
static ecm_ctl_allowlist_t AL;

static void test_allow(void)
{
    char err[256];
    ecm_ctl_allowlist_t x;
    CHECK(ecm_ctl_allow_load(NULL, &x, &BE, err, sizeof(err)) == 0 && x.n == 0, "allow none -> empty");
    CHECK(load("{\"schema\":1,\"allow\":[]}", &x, err, sizeof(err)) == 0 && x.n == 0, "allow empty file");
    CHECK(load("{\"schema\":1,\"allow\":[" ENTRY("0x2008", "u16", "0", "1000") "," ENTRY("0x607F", "u32", "0", "100000") ","
               ENTRY("0x6072", "u16", "0", "3000") ",{\"vendor\":\"0x00100000\",\"product\":\"0x000C0108\",\"index\":\"0x2009\","
               "\"sub\":1,\"type\":\"i16\",\"min\":-10,\"max\":10,\"note\":\"test\"}]}", &AL, err, sizeof(err)) == 0 && AL.n == 4,
          "allow valid file");
    if (AL.n != 4) printf("    %s\n", err);
    struct { const char *text, *what; } bad[] = {
        { "{\"schema\":2,\"allow\":[]}", "schema 2" },
        { "{\"schema\":1}", "no allow" },
        { "{\"schema\":1,\"allow\":[{\"index\":\"0x2008\",\"sub\":0,\"type\":\"u16\",\"min\":0,\"max\":1}]}", "no vendor" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x2008", "raw", "0", "1") "]}", "raw type" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x2008", "u16", "5", "1") "]}", "min > max" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x2008", "u8", "0", "300") "]}", "max outside type" },
        { "{\"schema\":1,\"allow\":[{\"vendor\":\"0x00100000\",\"product\":\"0x000C0108\",\"index\":\"0x2008\",\"sub\":0,\"type\":\"u16\",\"min\":0}]}", "no max" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x1C12", "u16", "0", "1") "]}", "comm area" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x6040", "u16", "0", "15") "]}", "controlword (backend deny)" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x6860", "i8", "0", "8") "]}", "mode of axis 1 (backend deny)" },
        { "{\"schema\":1,\"allow\":[" ENTRY("0x2008", "u16", "0", "1") "," ENTRY("0x2008", "u16", "0", "2") "]}", "duplicate" },
        { "{\"schema\":1,\"allow\":[1]}", "entry not an object" },
        { "{\"schema\":1,\"allow\":[}", "bad JSON" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = load(bad[i].text, &x, err, sizeof(err));
        CHECK(r == -1, "allow refuses bad file");
        if (r != -1) printf("    accepted: %s\n", bad[i].what);
    }
    CHECK(ecm_ctl_allow_load("/nonexistent/allow.json", &x, &BE, err, sizeof(err)) == -1, "allow missing file");
}

/* ------------------------------------------------------------- handle --- */
static ecm_ctl_state_t ST;
static char OUT[ECM_CTL_REPLY_MAX];
static uint64_t NOW = 1000000000ull;

static const char *req(const char *line)
{
    NOW += 100000000ull;                    /* 10 Hz: never rate limited here */
    size_t n = ecm_ctl_handle(&ST, &BE, &AL, line, strlen(line), NOW, OUT, sizeof(OUT));
    if (!n || OUT[n - 1] != '\n') return "NO REPLY";
    return OUT;
}

static int has(const char *r, const char *s) { return strstr(r, s) != NULL; }

static void test_handle(void)
{
    ecm_ctl_state_init(&ST);
    memset(&F, 0, sizeof(F));
    F.ready_ok = 1; F.pdo_known = 1;
    const char *r;

    r = req("{\"id\":1,\"cmd\":\"info\"}");
    CHECK(has(r, "{\"id\":1,\"ok\":true,\"info\":{\"pid\":1") && has(r, "\"ctl\":{\"version\":1") &&
          has(r, "\"index\":\"0x2009\",\"sub\":1,\"type\":\"i16\",\"min\":-10,\"max\":10,\"note\":\"test\""), "info");
    r = req("{\"id\":\"a\",\"cmd\":\"sdo_read\",\"slave\":1,\"index\":\"0x6061\",\"sub\":0,\"type\":\"i8\"}");
    CHECK(has(r, "{\"id\":\"a\",\"ok\":true,\"slave\":1,\"index\":\"0x6061\",\"sub\":0,\"type\":\"i8\",\"size\":1,\"raw\":\"08\",\"value\":8"), "sdo_read typed");
    r = req("{\"id\":2,\"cmd\":\"sdo_read\",\"slave\":1,\"index\":4104,\"sub\":0}");
    CHECK(has(r, "\"ok\":true") && has(r, "\"type\":\"raw\",\"size\":6,\"raw\":\"49533632304E\"") && !has(r, "\"value\""), "sdo_read raw");
    r = req("{\"id\":3,\"cmd\":\"sdo_read\",\"slave\":1,\"index\":\"0x6061\",\"sub\":0,\"type\":\"u16\"}");
    CHECK(has(r, "\"error\":\"size_mismatch\"") && has(r, "\"raw\":\"08\""), "sdo_read size mismatch");
    r = req("{\"id\":4,\"cmd\":\"sdo_read\",\"slave\":1,\"index\":\"0x5555\",\"sub\":0}");
    CHECK(has(r, "\"error\":\"sdo_failed\",\"abort\":\"0x06020000\",\"msg\":\"The object does not exist"), "sdo_read abort");
    r = req("{\"id\":5,\"cmd\":\"sdo_read\",\"slave\":9,\"index\":\"0x6061\",\"sub\":0}");
    CHECK(has(r, "\"error\":\"no_slave\""), "no slave");
    F.ready_ok = 0;
    r = req("{\"id\":6,\"cmd\":\"sdo_read\",\"slave\":1,\"index\":\"0x6061\",\"sub\":0}");
    CHECK(has(r, "\"error\":\"not_ready\",\"msg\":\"bus LOST\""), "not ready");
    F.ready_ok = 1;
    const char *badreq[] = {
        "not json", "[1]", "{\"id\":7}", "{\"cmd\":\"reboot\"}", "{\"cmd\":\"sdo_read\",\"slave\":1,\"sub\":0}",
        "{\"cmd\":\"sdo_read\",\"slave\":0,\"index\":1,\"sub\":0}", "{\"cmd\":\"sdo_read\",\"slave\":1,\"index\":65536,\"sub\":0}",
        "{\"cmd\":\"sdo_read\",\"slave\":1,\"index\":1,\"sub\":256}", "{\"cmd\":\"sdo_read\",\"slave\":1,\"index\":1,\"sub\":0,\"type\":\"u24\"}",
        "{\"cmd\":\"sdo_read\",\"slave\":1,\"index\":1,\"sub\":0,\"ca\":true}", "{\"id\":[1],\"cmd\":\"info\"}",
        "{\"cmd\":\"sdo_write\",\"slave\":1,\"index\":\"0x2008\",\"sub\":0,\"type\":\"u16\"}",
        "{\"cmd\":\"sdo_write\",\"slave\":1,\"index\":\"0x2008\",\"sub\":0,\"value\":1}",
        "{\"cmd\":\"sdo_write\",\"slave\":1,\"index\":\"0x2008\",\"sub\":0,\"type\":\"raw\",\"value\":1}",
        "{\"cmd\":\"sdo_write\",\"slave\":1,\"index\":\"0x2008\",\"sub\":0,\"type\":\"u16\",\"value\":-1}",
    };
    for (size_t i = 0; i < sizeof(badreq) / sizeof(badreq[0]); i++) {
        r = req(badreq[i]);
        CHECK(has(r, "\"ok\":false") && (has(r, "\"error\":\"bad_request\"") || has(r, "\"error\":\"unknown_cmd\"")), "bad request refused");
        if (!has(r, "\"ok\":false")) printf("    accepted: %s\n", badreq[i]);
    }
    CHECK(F.writes == 0, "bad requests never reached the slave");
    r = req("{\"id\":8,\"cmd\":\"diag_dump\"}");
    CHECK(has(r, "{\"id\":8,\"ok\":true,\"files\":[\"/tmp/x\"]}"), "diag_dump");
}

static void test_write(void)
{
    const char *r;
    F.ready_ok = 1; F.pdo_known = 1; F.writes = 0;
#define W(idx, typ, val) req("{\"id\":9,\"cmd\":\"sdo_write\",\"slave\":1,\"index\":\"" idx "\",\"sub\":0,\"type\":\"" typ "\",\"value\":" val "}")
    r = W("0x1C12", "u16", "1");     CHECK(has(r, "\"error\":\"denied_comm_area\""), "deny comm area (0x1C12)");
    r = W("0x1600", "u32", "1");     CHECK(has(r, "\"error\":\"denied_comm_area\""), "deny comm area (0x1600)");
    r = W("0x6040", "u16", "15");    CHECK(has(r, "\"error\":\"denied_hard\"") && has(r, "controlword"), "deny controlword");
    r = W("0x7060", "i8", "8");      CHECK(has(r, "\"error\":\"denied_hard\""), "deny mode of axis 2");
    r = W("0x607F", "u32", "1000");  CHECK(has(r, "\"error\":\"denied_rxpdo\""), "deny rxpdo (allowlisted 0x607F)");
    r = W("0x6072", "u16", "100");   CHECK(has(r, "\"error\":\"denied_initcmd\""), "deny initcmd (allowlisted 0x6072)");
    r = W("0x2010", "u16", "1");     CHECK(has(r, "\"error\":\"not_allowed\""), "deny not in allowlist");
    r = W("0x2008", "u32", "1");     CHECK(has(r, "\"error\":\"type_mismatch\""), "deny type mismatch");
    r = W("0x2008", "u16", "1001");  CHECK(has(r, "\"error\":\"out_of_range\"") && has(r, "[0, 1000]"), "deny out of range");
    r = W("0x2008", "u16", "70000"); CHECK(has(r, "\"error\":\"out_of_range\""), "deny outside type");
    CHECK(F.writes == 0, "no refused write reached the slave");
    F.pdo_known = 0;
    r = W("0x2008", "u16", "10");    CHECK(has(r, "\"error\":\"denied_no_pdo_table\""), "deny rxpdo (no PDO table)");
    F.pdo_known = 1;
    F.ready_ok = 0;
    r = W("0x2008", "u16", "10");    CHECK(has(r, "\"error\":\"not_ready\""), "write not ready");
    F.ready_ok = 1;
    CHECK(F.writes == 0, "still no write");
    r = W("0x2008", "u16", "\"0x2A\"");
    CHECK(has(r, "\"ok\":true") && has(r, "\"written\":42,\"raw\":\"2A00\",\"readback\":42,\"readback_ok\":true") && F.obj_2008 == 42,
          "write allowed + readback");
    F.clamp_2008 = 1;
    r = W("0x2008", "u16", "900");
    CHECK(has(r, "\"readback\":500,\"readback_ok\":false"), "readback shows what the slave kept");
    req("{\"id\":10,\"cmd\":\"sdo_read\",\"slave\":1,\"index\":\"0x2008\",\"sub\":0,\"type\":\"u16\"}");
    r = req("{\"id\":11,\"cmd\":\"sdo_write\",\"slave\":2,\"index\":\"0x2009\",\"sub\":1,\"type\":\"i16\",\"value\":-11}");
    CHECK(has(r, "\"error\":\"out_of_range\"") && has(r, "[-10, 10]"), "signed range");
    CHECK(ST.refused_writes >= 11, "refusals counted");
}

static void test_rate(void)
{
    ecm_ctl_state_t s;
    ecm_ctl_state_init(&s);
    uint64_t t = 5000000000ull;
    int limited = 0;
    for (int i = 0; i < 30; i++) {
        size_t n = ecm_ctl_handle(&s, &BE, &AL, "{\"cmd\":\"info\"}", 14, t, OUT, sizeof(OUT));
        if (n && strstr(OUT, "\"error\":\"rate_limited\"")) limited++;
    }
    CHECK(limited == 10, "rate: 30 at once -> 20 served, 10 limited");
    t += 500000000ull;                                          /* +0.5 s: 10 tokens back */
    limited = 0;
    for (int i = 0; i < 12; i++) {
        ecm_ctl_handle(&s, &BE, &AL, "garbage", 7, t, OUT, sizeof(OUT));   /* invalid lines cost too */
        if (strstr(OUT, "\"error\":\"rate_limited\"")) limited++;
    }
    CHECK(limited == 2, "rate: refill 20/s, invalid lines counted");
}

/* -------------------------------------------------------------- server --- */
static int cli(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { close(fd); return -1; }
    return fd;
}

static ecm_ctl_srv_t S;

static void spin(int times)
{
    for (int k = 0; k < times; k++) {
        struct pollfd p[1 + ECM_CTL_MAX_CLIENTS];
        int np = ecm_ctl_srv_fds(&S, p, 1 + ECM_CTL_MAX_CLIENTS);
        poll(p, (nfds_t)np, ecm_ctl_srv_pending(&S) ? 0 : 20);
        NOW += 100000000ull;
        ecm_ctl_srv_service(&S, p, np, &BE, &AL, NOW, 8);
    }
}

static int rd(int fd, char *b, size_t cap)
{
    size_t n = 0;
    for (int k = 0; k < 50 && n + 1 < cap; k++) {
        ssize_t r = recv(fd, b + n, cap - 1 - n, MSG_DONTWAIT);
        if (r > 0) { n += (size_t)r; continue; }
        if (r == 0) break;
        spin(1);
    }
    b[n] = '\0';
    return (int)n;
}

static void test_server(void)
{
    char dir[] = "/tmp/ctl_srv_XXXXXX", path[96], err[256], b[8192];
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    snprintf(path, sizeof(path), "%s/ctl.sock", dir);
    CHECK(ecm_ctl_srv_open(&S, path, 0660, -1, err, sizeof(err)) == 0, "server open");
    struct stat sb;
    CHECK(stat(path, &sb) == 0 && S_ISSOCK(sb.st_mode) && (sb.st_mode & 0777) == 0660, "socket mode 0660");
    ecm_ctl_srv_t other;
    CHECK(ecm_ctl_srv_open(&other, path, 0660, -1, err, sizeof(err)) == -1 && strstr(err, "another process"), "live socket not stolen");
    spin(3);                                    /* the probe connection of that open: accepted, then EOF */
    CHECK(ecm_ctl_srv_clients(&S) == 0, "probe connection gone");

    int c[6];
    for (int i = 0; i < 4; i++) c[i] = cli(path);
    spin(2);
    c[4] = cli(path);
    spin(2);
    int n = rd(c[4], b, sizeof(b));
    if (!(n > 0 && strstr(b, "too_many_clients"))) printf("    5th: n=%d [%s] clients=%d\n", n, b, ecm_ctl_srv_clients(&S));
    CHECK(n > 0 && strstr(b, "too_many_clients") && ecm_ctl_srv_clients(&S) == 4, "5th client refused");
    close(c[4]);

    static const char two[] = "{\"id\":1,\"cmd\":\"info\"}\n{\"id\":2,\"cmd\":\"sdo_read\",\"slave\":1,\"index\":\"0x6061\",\"sub\":0,\"type\":\"i8\"}\n";
    if (send(c[0], two, sizeof(two) - 1, 0) < 0) {}
    spin(3);
    n = rd(c[0], b, sizeof(b));
    CHECK(strstr(b, "{\"id\":1,\"ok\":true") && strstr(b, "{\"id\":2,\"ok\":true") && strchr(b, '\n') != strrchr(b, '\n'),
          "two requests in one write -> two replies");

    char lng[3000];
    memset(lng, 'x', sizeof(lng));
    lng[sizeof(lng) - 1] = '\n';
    if (send(c[1], lng, sizeof(lng), 0) < 0) {}
    static const char three[] = "{\"id\":3,\"cmd\":\"info\"}\n";
    if (send(c[1], three, sizeof(three) - 1, 0) < 0) {}
    spin(6);
    n = rd(c[1], b, sizeof(b));
    CHECK(strstr(b, "line_too_long") && strstr(b, "{\"id\":3,\"ok\":true") && !strstr(b, "bad_request"),
          "over-long line dropped whole, next line served");

    /* a client that never reads: dropped once its replies fill the buffer */
    int big = 1 << 20;
    setsockopt(c[2], SOL_SOCKET, SO_RCVBUF, &(int){ 4096 }, sizeof(int));
    char many[22 * 64];
    for (int i = 0; i < 64; i++) memcpy(many + 22 * i, "{\"id\":4,\"cmd\":\"info\"}\n", 22);
    for (int round = 0; round < 40 && ecm_ctl_srv_clients(&S) == 4; round++) {
        if (send(c[2], many, sizeof(many), MSG_DONTWAIT) < 0) {}
        spin(64);
    }
    (void)big;
    CHECK(S.dropped_slow == 1 && ecm_ctl_srv_clients(&S) == 3, "client that never reads is dropped");
    CHECK(rd(c[3], b, sizeof(b)) == 0, "other clients untouched");

    close(c[0]); close(c[1]); close(c[2]); close(c[3]);
    spin(3);
    CHECK(ecm_ctl_srv_clients(&S) == 0, "closed clients freed");
    ecm_ctl_srv_close(&S);
    CHECK(stat(path, &sb) != 0, "socket removed at close");

    /* stale socket (nobody listens) is replaced; a regular file is not */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a))) {}
    close(fd);
    CHECK(ecm_ctl_srv_open(&S, path, 0660, -1, err, sizeof(err)) == 0, "stale socket replaced");
    ecm_ctl_srv_close(&S);
    write_text(path, "x");
    CHECK(ecm_ctl_srv_open(&S, path, 0660, -1, err, sizeof(err)) == -1 && strstr(err, "not a socket"), "regular file not removed");
    unlink(path);
    rmdir(dir);
}

int main(void)
{
    if (ECM_CTL_BROKEN) printf("NEGATIVE CONTROL BUILD: ECM_CTL_BROKEN = %d\n", ECM_CTL_BROKEN);
    test_json();
    test_sha256();
    test_codec();
    test_allow();
    test_handle();
    test_write();
    test_rate();
    test_server();
    printf("test_ctl_offline: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
