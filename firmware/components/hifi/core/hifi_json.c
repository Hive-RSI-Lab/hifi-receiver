#include "hifi_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ writer */

void hj_init(hj_t *j, char *buf, size_t cap) {
	memset(j, 0, sizeof(*j));
	j->buf = buf;
	j->cap = cap;
	if (cap) buf[0] = '\0';
}

static void put(hj_t *j, const char *s, size_t n) {
	if (j->overflow) return;
	if (j->len + n + 1 > j->cap) {
		j->overflow = true;
		return;
	}
	memcpy(j->buf + j->len, s, n);
	j->len += n;
	j->buf[j->len] = '\0';
}

static void puts_(hj_t *j, const char *s) { put(j, s, strlen(s)); }

static void put_escaped(hj_t *j, const char *s) {
	put(j, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char) *s;
		char esc[8];
		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = (char) c;
			put(j, esc, 2);
		} else if (c < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			puts_(j, esc);
		} else {
			put(j, (const char *) &c, 1);
		}
	}
	put(j, "\"", 1);
}

static void key(hj_t *j, const char *k) {
	if (j->depth > 0) {
		if (!j->first[j->depth]) put(j, ",", 1);
		j->first[j->depth] = false;
	}
	if (k) {
		put_escaped(j, k);
		put(j, ":", 1);
	}
}

static void open_(hj_t *j, const char *k, const char *brace) {
	key(j, k);
	puts_(j, brace);
	if (j->depth + 1 < HJ_MAX_DEPTH) j->depth++;
	else j->overflow = true;
	j->first[j->depth] = true;
}

static void close_(hj_t *j, const char *brace) {
	if (j->depth > 0) j->depth--;
	puts_(j, brace);
}

void hj_obj(hj_t *j, const char *k) { open_(j, k, "{"); }
void hj_obj_end(hj_t *j) { close_(j, "}"); }
void hj_arr(hj_t *j, const char *k) { open_(j, k, "["); }
void hj_arr_end(hj_t *j) { close_(j, "]"); }

void hj_str(hj_t *j, const char *k, const char *v) {
	key(j, k);
	if (v) put_escaped(j, v);
	else puts_(j, "null");
}

void hj_num(hj_t *j, const char *k, double v, int decimals) {
	char tmp[40];
	key(j, k);
	if (isnan(v) || isinf(v)) {
		puts_(j, "null");
		return;
	}
	snprintf(tmp, sizeof(tmp), "%.*f", decimals, v);
	/* "-0.0" reads oddly in a log */
	if (tmp[0] == '-' && strspn(tmp + 1, "0.") == strlen(tmp + 1)) memmove(tmp, tmp + 1, strlen(tmp));
	puts_(j, tmp);
}

void hj_int(hj_t *j, const char *k, long long v) {
	char tmp[24];
	key(j, k);
	snprintf(tmp, sizeof(tmp), "%lld", v);
	puts_(j, tmp);
}

void hj_bool(hj_t *j, const char *k, bool v) {
	key(j, k);
	puts_(j, v ? "true" : "false");
}

void hj_null(hj_t *j, const char *k) {
	key(j, k);
	puts_(j, "null");
}

/* ------------------------------------------------------------------ reader */

static const char *ws(const char *p) {
	while (*p && isspace((unsigned char) *p)) p++;
	return p;
}

static int hex4(const char *p) {
	int v = 0;
	for (int d = 0; d < 4; d++) {
		char c = p[d];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return -1;
	}
	return v;
}

/* Reads one character of a JSON string at *pp into b and moves *pp past it.
 * Returns its length in bytes (1..4), 0 at the closing quote, or -1 when the
 * string isn't one we take: the end of the text, an unknown escape, bad hex, a
 * lone surrogate or \u0000 (no C string can hold it). Every escape JSON has is
 * decoded, \uXXXX as UTF-8 and a pair of surrogates as one character. A raw
 * UTF-8 sequence comes back whole, so a cut never splits it. skip_str and
 * hj_get_str both read through here: a body that one would refuse, the
 * other never sees (hj_valid_object turns it into a 400 before any handler). */
static int str_char(const char **pp, char b[4]) {
	const char *p = *pp;
	int len = 1;
	if (!*p) return -1;
	if (*p == '"') return 0;
	b[0] = *p;
	if (*p == '\\') {
		switch (*++p) {
		case '"': case '\\': case '/': b[0] = *p; break;
		case 'b': b[0] = '\b'; break;
		case 'f': b[0] = '\f'; break;
		case 'n': b[0] = '\n'; break;
		case 'r': b[0] = '\r'; break;
		case 't': b[0] = '\t'; break;
		case 'u': {
			long cp = hex4(p + 1);
			if (cp < 0) return -1;
			p += 4;
			if (cp >= 0xdc00 && cp <= 0xdfff) return -1;
			if (cp >= 0xd800 && cp <= 0xdbff) {
				int lo = p[1] == '\\' && p[2] == 'u' ? hex4(p + 3) : -1;
				if (lo < 0xdc00 || lo > 0xdfff) return -1;
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				p += 6;
			}
			if (cp == 0) return -1;
			if (cp < 0x80) {
				b[0] = (char) cp;
			} else if (cp < 0x800) {
				b[0] = (char) (0xc0 | cp >> 6);
				b[1] = (char) (0x80 | (cp & 0x3f));
				len = 2;
			} else if (cp < 0x10000) {
				b[0] = (char) (0xe0 | cp >> 12);
				b[1] = (char) (0x80 | (cp >> 6 & 0x3f));
				b[2] = (char) (0x80 | (cp & 0x3f));
				len = 3;
			} else {
				b[0] = (char) (0xf0 | cp >> 18);
				b[1] = (char) (0x80 | (cp >> 12 & 0x3f));
				b[2] = (char) (0x80 | (cp >> 6 & 0x3f));
				b[3] = (char) (0x80 | (cp & 0x3f));
				len = 4;
			}
			break;
		}
		default: return -1;
		}
	} else if ((unsigned char) *p >= 0xc0) {
		/* a raw lead byte and the continuation bytes after it: one unit */
		while (len < 4 && ((unsigned char) p[len] & 0xc0) == 0x80) {
			b[len] = p[len];
			len++;
		}
		p += len - 1;
	}
	*pp = p + 1;
	return len;
}

/* Skips a string starting at '"'; returns the char after the closing quote,
 * or NULL if it isn't a string hj_get_str would take. */
static const char *skip_str(const char *p) {
	char b[4];
	int len;
	for (p++; (len = str_char(&p, b)) > 0;)
		;
	return len < 0 ? NULL : p + 1;
}

static const char *skip_value(const char *p);

static const char *skip_container(const char *p, char open, char close) {
	p = ws(p + 1);
	if (*p == close) return p + 1;
	for (;;) {
		if (open == '{') {
			if (*p != '"' || !(p = skip_str(p))) return NULL;
			p = ws(p);
			if (*p++ != ':') return NULL;
		}
		if (!(p = skip_value(ws(p)))) return NULL;
		p = ws(p);
		if (*p == ',') {
			p = ws(p + 1);
			continue;
		}
		if (*p == close) return p + 1;
		return NULL;
	}
}

static const char *skip_value(const char *p) {
	if (*p == '"') return skip_str(p);
	if (*p == '{') return skip_container(p, '{', '}');
	if (*p == '[') return skip_container(p, '[', ']');
	if (!strncmp(p, "true", 4)) return p + 4;
	if (!strncmp(p, "false", 5)) return p + 5;
	if (!strncmp(p, "null", 4)) return p + 4;
	if (*p == '-' || isdigit((unsigned char) *p)) {
		char *end;
		strtod(p, &end);
		return end > p ? end : NULL;
	}
	return NULL;
}

bool hj_valid_object(const char *json) {
	if (!json) return false;
	const char *p = ws(json);
	if (*p != '{') return false;
	p = skip_container(p, '{', '}');
	return p && *ws(p) == '\0';
}

/* Finds the value of a top-level key in text already checked by
 * hj_valid_object; returns a pointer to it or NULL. */
static const char *find_in(const char *json, const char *k) {
	size_t klen = strlen(k);
	const char *p = ws(ws(json) + 1);
	while (*p == '"') {
		const char *kstart = p + 1, *kend = skip_str(p);
		bool match = (size_t) (kend - 1 - kstart) == klen && !strncmp(kstart, k, klen);
		p = ws(kend);
		p = ws(p + 1); /* ':' */
		if (match) return p;
		p = ws(skip_value(p));
		if (*p == ',') p = ws(p + 1);
	}
	return NULL;
}

static const char *find(const char *json, const char *k) {
	return hj_valid_object(json) ? find_in(json, k) : NULL;
}

/* Decodes a JSON string (see str_char) into out, cut at a whole character
 * when it doesn't fit; *cut says whether it was. 1 if read, 0 if the key is
 * missing or not a string, -1 if the text isn't an object hj_valid_object
 * takes (out is "" then, for a caller that ignores the answer). */
static int get_str(const char *json, const char *k, char *out, size_t n, bool *cut) {
	*cut = false;
	if (!hj_valid_object(json)) {
		out[0] = '\0';
		return -1;
	}
	const char *p = find_in(json, k);
	if (!p || *p != '"') return 0;
	size_t i = 0;
	char b[4];
	int len;
	for (p++; (len = str_char(&p, b)) > 0;) {
		if (*cut || i + (size_t) len >= n) {
			*cut = true; /* cut here: nothing after a character that didn't fit */
			continue;
		}
		memcpy(out + i, b, (size_t) len);
		i += (size_t) len;
	}
	if (len < 0) { /* can't happen: hj_valid_object read the same string */
		out[0] = '\0';
		return -1;
	}
	out[i] = '\0';
	return 1;
}

/* Text that hj_valid_object refuses (a bad string anywhere in it) leaves out
 * "" and returns false. A long string is cut at a whole character, escaped or
 * raw UTF-8, and still terminated. */
bool hj_get_str(const char *json, const char *k, char *out, size_t n) {
	bool cut;
	return n && get_str(json, k, out, n, &cut) > 0;
}

int hj_get_str_fit(const char *json, const char *k, char *out, size_t n) {
	bool cut;
	if (!n) return HJ_STR_CUT;
	int r = get_str(json, k, out, n, &cut);
	if (r <= 0) return HJ_STR_NONE;
	if (cut) {
		out[0] = '\0';
		return HJ_STR_CUT;
	}
	return HJ_STR_OK;
}

bool hj_get_num(const char *json, const char *k, double *out) {
	const char *p = find(json, k);
	if (!p || !(*p == '-' || isdigit((unsigned char) *p))) return false;
	/* strtod also reads -nan, -inf and 1e999, which no JSON number is and
	 * which no caller's range check catches (NaN fails every comparison) */
	double v = strtod(p, NULL);
	if (!isfinite(v)) return false;
	*out = v;
	return true;
}

bool hj_num_bad(const char *json, const char *k) {
	const char *p = find(json, k);
	return p && (*p == '-' || isdigit((unsigned char) *p)) && !isfinite(strtod(p, NULL));
}

bool hj_get_bool(const char *json, const char *k, bool *out) {
	const char *p = find(json, k);
	if (!p) return false;
	if (!strncmp(p, "true", 4)) *out = true;
	else if (!strncmp(p, "false", 5)) *out = false;
	else return false;
	return true;
}

int hj_get_num_array(const char *json, const char *k, double *out, int max) {
	const char *p = find(json, k);
	if (!p || *p != '[') return -1;
	int n = 0;
	p = ws(p + 1);
	if (*p == ']') return 0;
	for (;;) {
		char *end;
		if (!(*p == '-' || isdigit((unsigned char) *p))) return -1;
		double v = strtod(p, &end);
		if (!isfinite(v)) return -1;
		if (n < max) out[n] = v;
		n++;
		p = ws(end);
		if (*p == ']') break;
		if (*p++ != ',') return -1;
		p = ws(p);
	}
	return n < max ? n : max;
}

bool hj_get_raw(const char *json, const char *k, const char **val, size_t *len) {
	const char *p = find(json, k), *end;
	if (!p || !(end = skip_value(p))) return false;
	*val = p;
	*len = (size_t) (end - p);
	return true;
}

bool hj_get_str_span(const char *json, const char *k, const char **val, size_t *len) {
	const char *p;
	size_t n;
	if (!hj_get_raw(json, k, &p, &n) || *p != '"' || memchr(p, '\\', n)) return false;
	*val = p + 1;
	*len = n - 2;
	return true;
}
