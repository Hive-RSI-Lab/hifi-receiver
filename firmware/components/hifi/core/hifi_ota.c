#include <stdio.h>
#include <string.h>

#include "hifi_ota.h"

void hifi_ota_init(hifi_ota_t *o, const hifi_ota_io_t *io) {
	memset(o, 0, sizeof(*o));
	o->io = io;
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

void hifi_ota_abort(hifi_ota_t *o) {
	if (o->active && o->io->abort) o->io->abort(o->io->ctx);
	o->active = false;
}

static int begin(hifi_ota_t *o, size_t size, const uint8_t *sha, const char *version, const char **err) {
	if (size < 1024) return *err = "size is too small for an app image", 400;
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
	return 200;
}

int hifi_ota_begin(hifi_ota_t *o, size_t size, const char *sha_hex, const char *version, const char **err) {
	uint8_t sha[32];
	if (!sha_hex || strlen(sha_hex) != 64) return *err = "sha256 must be 64 hex digits", 400;
	for (int i = 0; i < 32; i++) {
		int hi = hexval(sha_hex[2 * i]), lo = hexval(sha_hex[2 * i + 1]);
		if (hi < 0 || lo < 0) return *err = "sha256 must be 64 hex digits", 400;
		sha[i] = (uint8_t) (hi << 4 | lo);
	}
	return begin(o, size, sha, version, err);
}

int hifi_ota_begin_local(hifi_ota_t *o, size_t size, const char *version, const char **err) {
	return begin(o, size, NULL, version, err);
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
	if (o->io->commit(o->io->ctx, err)) {
		hifi_ota_abort(o);
		return 500;
	}
	o->active = false;
	o->done = true;
	return 200;
}
