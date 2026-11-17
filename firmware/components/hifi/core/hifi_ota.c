#include <stdio.h>
#include <string.h>

#include "hifi_ota.h"

void hifi_ota_init(hifi_ota_t *o, const hifi_ota_io_t *io) {
	memset(o, 0, sizeof(*o));
	o->io = io;
	o->run_stamp = -1;
}

long long hifi_ota_build_stamp(const char *date, const char *time) {
	static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	if (!date || !time || strlen(date) != 11 || strlen(time) != 8) return -1;
	int m = 0;
	for (; m < 12; m++)
		if (!strncmp(date, mon + 3 * m, 3)) break;
	if (m == 12 || date[3] != ' ' || date[6] != ' ' || time[2] != ':' || time[5] != ':') return -1;
	int dd = 0, y = 0, hh = 0, mi = 0, ss = 0;
	for (int i = 4; i < 6; i++) {
		if (date[i] == ' ' && i == 4) continue; /* __DATE__ pads the day with a space */
		if (date[i] < '0' || date[i] > '9') return -1;
		dd = dd * 10 + (date[i] - '0');
	}
	for (int i = 7; i < 11; i++) {
		if (date[i] < '0' || date[i] > '9') return -1;
		y = y * 10 + (date[i] - '0');
	}
	for (int i = 0; i < 8; i++) {
		if (i == 2 || i == 5) continue;
		if (time[i] < '0' || time[i] > '9') return -1;
	}
	hh = (time[0] - '0') * 10 + time[1] - '0';
	mi = (time[3] - '0') * 10 + time[4] - '0';
	ss = (time[6] - '0') * 10 + time[7] - '0';
	if (dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 60) return -1;
	return ((((y * 100LL + m + 1) * 100 + dd) * 100 + hh) * 100 + mi) * 100 + ss;
}

void hifi_ota_set_running(hifi_ota_t *o, const char *project, const char *date, const char *time) {
	snprintf(o->run_project, sizeof(o->run_project), "%s", project ? project : "");
	o->run_stamp = hifi_ota_build_stamp(date, time);
}

/* a field of the incoming app descriptor, as a C string */
static void desc_str(const uint8_t *head, size_t at, size_t n, char *out) {
	memcpy(out, head + at, n);
	out[n] = 0;
}

/* 0, or 403: the image isn't this project, or is older than the running app */
static int check_age(const hifi_ota_t *o, const char **err) {
	if (o->run_stamp < 0) return 0;
	const uint8_t *h = o->head;
	uint32_t magic = (uint32_t) h[32] | (uint32_t) h[33] << 8 | (uint32_t) h[34] << 16 | (uint32_t) h[35] << 24;
	if (magic != HIFI_OTA_DESC_MAGIC) return *err = "the image has no app description", 403;
	char proj[33], tm[17], dt[17];
	desc_str(h, 80, 32, proj);
	desc_str(h, 112, 16, tm);
	desc_str(h, 128, 16, dt);
	if (o->run_project[0] && strcmp(proj, o->run_project)) return *err = "the image is for another project", 403;
	long long st = hifi_ota_build_stamp(dt, tm);
	if (st < 0) return *err = "the image's build time can't be read", 403;
	if (st < o->run_stamp) return *err = "the image is older than the running firmware: install it over USB", 403;
	return 0;
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

void hifi_ota_set_keys(hifi_ota_t *o, const uint8_t (*keys)[32], int nkeys, hifi_verify_fn verify) {
	if (nkeys > HIFI_OTA_KEYS_MAX) nkeys = HIFI_OTA_KEYS_MAX;
	if (nkeys < 0) nkeys = 0;
	for (int k = 0; k < nkeys; k++) memcpy(o->keys[k], keys[k], 32);
	o->nkeys = nkeys;
	o->verify = verify;
}

void hifi_ota_abort(hifi_ota_t *o) {
	if (o->active && o->io->abort) o->io->abort(o->io->ctx);
	o->active = false;
}

static int begin(hifi_ota_t *o, size_t size, const uint8_t *sha, const char *version, bool need_sig,
		 const char **err) {
	if (size < 1024 + (need_sig ? HIFI_OTA_SIG_LEN : 0)) return *err = "size is too small for an app image", 400;
	if (need_sig && (!o->nkeys || !o->verify))
		return *err = "this build has no Hive key to check an image with: update over USB", 403;
	if (o->io->slot_size && size > o->io->slot_size) return *err = "image is larger than the app slot", 400;
	hifi_ota_abort(o);
	const char *slot = NULL;
	if (o->io->open(o->io->ctx, size, &slot, err)) return 409;
	o->active = true;
	o->done = false;
	o->size = size;
	o->written = 0;
	o->slot = slot;
	o->check_sha = sha != NULL;
	if (sha) memcpy(o->sha, sha, 32);
	snprintf(o->version, sizeof(o->version), "%s", version ? version : "");
	hifi_sha256_init(&o->h);
	o->need_sig = need_sig;
	hifi_sha256_init(&o->hb);
	memset(o->tail, 0, sizeof(o->tail));
	memset(o->head, 0, sizeof(o->head));
	return 200;
}

static int parse_sha(const char *sha_hex, uint8_t sha[32], const char **err) {
	if (!sha_hex || strlen(sha_hex) != 64) return *err = "sha256 must be 64 hex digits", 400;
	for (int i = 0; i < 32; i++) {
		int hi = hexval(sha_hex[2 * i]), lo = hexval(sha_hex[2 * i + 1]);
		if (hi < 0 || lo < 0) return *err = "sha256 must be 64 hex digits", 400;
		sha[i] = (uint8_t) (hi << 4 | lo);
	}
	return 200;
}

int hifi_ota_begin_ex(hifi_ota_t *o, size_t size, const char *sha_hex, const char *version, bool need_sig,
		      const char **err) {
	uint8_t sha[32];
	int st;
	if (sha_hex && (st = parse_sha(sha_hex, sha, err)) != 200) return st;
	return begin(o, size, sha_hex ? sha : NULL, version, need_sig, err);
}

int hifi_ota_begin(hifi_ota_t *o, size_t size, const char *sha_hex, const char *version, const char **err) {
	if (!sha_hex) return *err = "sha256 must be 64 hex digits", 400;
	return hifi_ota_begin_ex(o, size, sha_hex, version, false, err);
}

int hifi_ota_begin_local(hifi_ota_t *o, size_t size, const char *version, const char **err) {
	return begin(o, size, NULL, version, false, err);
}

int hifi_ota_check_sig(const hifi_ota_t *o, const uint8_t body_sha[32], const uint8_t tail[HIFI_OTA_SIG_LEN],
		       const char **err) {
	static const char hx[] = "0123456789abcdef";
	if (memcmp(tail, HIFI_OTA_SIG_MAGIC, 8)) return *err = "the image isn't signed by the Hive", 403;
	if (!o->nkeys || !o->verify) return *err = "this build has no Hive key to check an image with", 403;
	char msg[sizeof(HIFI_OTA_SIG_CONTEXT) - 1 + 64];
	size_t c = sizeof(HIFI_OTA_SIG_CONTEXT) - 1;
	memcpy(msg, HIFI_OTA_SIG_CONTEXT, c);
	for (int i = 0; i < 32; i++) {
		msg[c + 2 * i] = hx[body_sha[i] >> 4];
		msg[c + 2 * i + 1] = hx[body_sha[i] & 15];
	}
	for (int k = 0; k < o->nkeys; k++)
		if (o->verify(o->keys[k], (const uint8_t *) msg, sizeof(msg), tail + 8)) return 0;
	return *err = "the image's signature isn't the Hive's", 403;
}

int hifi_ota_chunk(hifi_ota_t *o, size_t off, const uint8_t *d, size_t n, const char **err) {
	if (!o->active) return *err = "no update open: send ota/begin first", 409;
	if (off != o->written) return *err = "chunk out of order", 409;
	if (!n || n > HIFI_OTA_CHUNK_MAX) return *err = "a chunk is 1 to 4096 bytes", 400;
	if (o->written + n > o->size) return *err = "chunk runs past the image size", 400;
	if (off == 0 && d[0] != HIFI_OTA_APP_MAGIC) {
		hifi_ota_abort(o);
		return *err = "not an ESP32 app image", 400;
	}
	if (o->io->write(o->io->ctx, d, n)) {
		hifi_ota_abort(o);
		return *err = "flash write failed", 500;
	}
	hifi_sha256_update(&o->h, d, n);
	if (o->written < HIFI_OTA_DESC_END) {
		size_t k = HIFI_OTA_DESC_END - o->written < n ? HIFI_OTA_DESC_END - o->written : n;
		memcpy(o->head + o->written, d, k);
	}
	/* the signed part and the trailer, wherever the chunk boundary falls */
	size_t body = o->size - HIFI_OTA_SIG_LEN;
	size_t nb = o->written >= body ? 0 : (body - o->written < n ? body - o->written : n);
	if (o->size >= HIFI_OTA_SIG_LEN) {
		hifi_sha256_update(&o->hb, d, nb);
		if (n > nb) memcpy(o->tail + (o->written + nb - body), d + nb, n - nb);
	}
	o->written += n;
	return 200;
}

int hifi_ota_finish(hifi_ota_t *o, const char **err) {
	if (!o->active) return *err = "no update open", 409;
	if (o->written != o->size) return *err = "image incomplete", 409;
	uint8_t got[32];
	hifi_sha256_final(&o->h, got);
	if (o->check_sha && memcmp(got, o->sha, 32)) {
		hifi_ota_abort(o);
		return *err = "sha256 mismatch", 400;
	}
	if (o->need_sig) {
		uint8_t bs[32];
		hifi_sha256_final(&o->hb, bs);
		int st = hifi_ota_check_sig(o, bs, o->tail, err);
		if (!st) st = check_age(o, err); /* after the signature: only then are its bytes the Hive's */
		if (st) {
			hifi_ota_abort(o);
			return st;
		}
	}
	if (o->io->commit(o->io->ctx, err)) {
		hifi_ota_abort(o);
		return 500;
	}
	o->active = false;
	o->done = true;
	return 200;
}
