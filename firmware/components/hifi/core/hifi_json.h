/*
 * Tiny JSON writer and flat-object reader for the test API.
 *
 * The writer appends into a caller's buffer and never overflows it (it sets
 * `overflow` instead). The reader handles what the API's request bodies are:
 * one flat object of strings, numbers, booleans and nulls; nested values are
 * skipped.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define HJ_MAX_DEPTH 8

typedef struct {
	char *buf;
	size_t cap, len;
	int depth;
	bool first[HJ_MAX_DEPTH];
	bool overflow;
} hj_t;

void hj_init(hj_t *j, char *buf, size_t cap);
/* key is NULL at the root and inside arrays */
void hj_obj(hj_t *j, const char *key);
void hj_obj_end(hj_t *j);
void hj_arr(hj_t *j, const char *key);
void hj_arr_end(hj_t *j);
void hj_str(hj_t *j, const char *key, const char *val);       /* NULL -> null */
void hj_num(hj_t *j, const char *key, double val, int decimals); /* NaN -> null */
void hj_int(hj_t *j, const char *key, long long val);
void hj_bool(hj_t *j, const char *key, bool val);
void hj_null(hj_t *j, const char *key);

/* Reader: each returns false if the key is missing or of another type.
 * hj_get_str decodes every JSON escape (\uXXXX as UTF-8) and cuts a long
 * string at a whole character. hj_valid_object, which every reader runs
 * first, refuses the whole text if any string in it holds \u0000, an unknown
 * escape, bad hex or a lone surrogate, so no handler sees one and a field
 * the caller treats as optional can't be dropped by a typo in it. */
bool hj_get_str(const char *json, const char *key, char *out, size_t n);
/* hj_get_str for a value that is wrong cut short (a URL, a password, an
 * SSID, a name): HJ_STR_OK when the whole decoded string fit in out,
 * HJ_STR_NONE when the key is missing or not a string, HJ_STR_CUT when it
 * didn't fit. A missing key leaves out as it was (the caller's default); a
 * cut one leaves it "", never a prefix of the value. Text that isn't a valid
 * object is HJ_STR_NONE with out "" (default lost), as hj_get_str leaves it. Escapes count as the bytes
 * they decode to, so the limit is the same whatever way a client writes it. */
enum { HJ_STR_CUT = -1, HJ_STR_NONE = 0, HJ_STR_OK = 1 };
int hj_get_str_fit(const char *json, const char *key, char *out, size_t n);
bool hj_get_num(const char *json, const char *key, double *out);
/* true if `key` holds what reads as a number but isn't a finite one (-nan,
 * -inf, 1e999): hj_get_num refuses those, so a caller can answer 400 */
bool hj_num_bad(const char *json, const char *key);
bool hj_get_bool(const char *json, const char *key, bool *out);
/* true if `json` is one well-formed object (whitespace around it allowed) */
bool hj_valid_object(const char *json);
/* A top-level array of numbers; returns how many were read (at most max), or
 * -1 if the key is missing, isn't an array, or holds a non-number. */
int hj_get_num_array(const char *json, const char *key, double *out, int max);
/* The raw text of a top-level key's value (an object, array, string with its
 * quotes, number...), without copying it: for a nested body or a long
 * base64 string. */
bool hj_get_raw(const char *json, const char *key, const char **val, size_t *len);
/* The same, for a value known to be one string: its characters between the
 * quotes, unescaped only if it has no escapes (base64 never does). A string
 * with an escape returns false: to size or check one, use hj_get_str_fit. */
bool hj_get_str_span(const char *json, const char *key, const char **val, size_t *len);
