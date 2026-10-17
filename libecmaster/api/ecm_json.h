/* SPDX-FileCopyrightText: 2026 khaiannguyen */
/* SPDX-License-Identifier: GPL-3.0-only */
/* ==========================================================================
 * ecm_json.h -- Phase 11.2: a small JSON tokenizer (no allocation), for the
 * one-line requests of ctl.sock and for config/api_sdo_allow.json.
 *
 * The whole text is checked (strict RFC 8259 syntax, one value, nothing but
 * whitespace after it); the result is a flat token array in document order:
 * an object/array token is followed by its children, `size` = number of
 * direct children (an object counts keys AND values: 2 per member).
 * ========================================================================== */
#ifndef ECM_JSON_H
#define ECM_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum { ECM_J_OBJ = 1, ECM_J_ARR, ECM_J_STR, ECM_J_NUM, ECM_J_TRUE, ECM_J_FALSE, ECM_J_NULL } ecm_jtype_t;

typedef struct {
    uint8_t type;          /* ecm_jtype_t                                    */
    int     start, end;    /* [start, end) in the text; strings without quotes */
    int     size;          /* direct children (object: keys + values)        */
    int     parent;        /* index of the enclosing object/array, -1 = root */
} ecm_jtok_t;

#define ECM_JSON_MAX_DEPTH 16

/* Returns the number of tokens (>= 1), or a negative error:
 *   -1 syntax error   -2 more tokens than max   -3 nested deeper than ECM_JSON_MAX_DEPTH
 * *errpos (may be NULL) = text offset of the error. */
int ecm_json_parse(const char *s, size_t len, ecm_jtok_t *t, int max, int *errpos);

/* Index of the token after the subtree of token i. */
int ecm_json_skip(const ecm_jtok_t *t, int n, int i);

/* Value token of member `key` of object token `obj`, -1 if absent (or obj not an object). */
int ecm_json_get(const char *s, const ecm_jtok_t *t, int n, int obj, const char *key);

/* String value, unescaped into out (\uXXXX below 0x80 only; others -> '?').
 * 0 ok, -1 not a string or does not fit. */
int ecm_json_str(const char *s, const ecm_jtok_t *tk, char *out, size_t cap);

/* Integers: a JSON integer, or a string "0x1A2B" / "123" / "-5".
 * 0 ok, -1 not an integer or out of range of the target type. */
int ecm_json_i64(const char *s, const ecm_jtok_t *tk, int64_t *v);
int ecm_json_u64(const char *s, const ecm_jtok_t *tk, uint64_t *v);
/* Any JSON number (or an integer string as above) as a double. */
int ecm_json_f64(const char *s, const ecm_jtok_t *tk, double *v);

#endif /* ECM_JSON_H */
