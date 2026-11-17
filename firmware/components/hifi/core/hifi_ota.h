/*
 * A firmware update pushed in chunks (docs/remote.md, "Firmware updates the
 * Hive pushes"): begin with the size and SHA-256, chunks in order, finish.
 * The logic is here; the board gives the flash writes (esp_ota_* on the
 * ESP32, a buffer in the host tests). Nothing boots until the whole image is
 * written and its hash matches.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_sha256.h"

#define HIFI_OTA_CHUNK_MAX 4096
#define HIFI_OTA_APP_MAGIC 0xE9 /* the first byte of every ESP32 app image */

/*
 * A signed image (firmware/tools/sign_fw.py) ends in an 80-byte trailer:
 * "HIFISIG2", the image's build number (8 bytes, little-endian) and an
 * Ed25519 signature over "hifi-fw-v2:<build>:" followed by the SHA-256 (64
 * lower-case hex digits) of everything before the trailer. The ESP-IDF image
 * format ignores bytes after the image, so a signed file still flashes
 * anywhere an unsigned one does. An update from the LAN must carry one
 * (security.md); the Hive's relay commands are signed themselves.
 *
 * The build number (L9) is the UTC second the build started
 * (firmware/tools/build.sh), built into the app as HIFI_BUILD_TAG
 * "HIFI-BUILD:<n>;", which is where the signer reads it from: one source, no
 * local time, nothing to parse wrong in the image's own description.
 */
#define HIFI_OTA_SIG_MAGIC "HIFISIG2"
#define HIFI_OTA_SIG_LEN 80
#define HIFI_OTA_SIG_CONTEXT "hifi-fw-v2:"
#define HIFI_OTA_KEYS_MAX 2
#define HIFI_BUILD_TAG_PREFIX "HIFI-BUILD:"

/* An app image's esp_app_desc_t sits at byte 32, its project_name at 80.
 * The signature covers it, so a signed image's project can be trusted. */
#define HIFI_OTA_DESC_MAGIC 0xABCD5432u
#define HIFI_OTA_DESC_END 144

typedef bool (*hifi_verify_fn)(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64]);

typedef struct {
	/* opens the slot it isn't running from; *slot names it ("ota_1") */
	int (*open)(void *ctx, size_t size, const char **slot, const char **err);
	int (*write)(void *ctx, const uint8_t *d, size_t n); /* 0 ok */
	int (*commit)(void *ctx, const char **err);          /* make it the boot image; 0 ok */
	void (*abort)(void *ctx);
	/* optional (NULL: one task only, the host tests): one session, three tasks
	 * (the web server, the relay, the URL job), so the API takes this around
	 * each of its steps (hifi_ota_lock) */
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
	size_t slot_size; /* the largest image a slot holds */
	void *ctx;
} hifi_ota_io_t;

typedef struct {
	const hifi_ota_io_t *io;
	bool active, done;
	bool held;         /* a task runs a whole update (hifi_ota_claim): the API waits */
	size_t size, written;
	uint8_t sha[32];
	bool check_sha;    /* false: the image's own appended hash only (commit checks it) */
	char version[32];
	const char *slot;
	hifi_sha256_t h;
	/* the signature check (set once by hifi_ota_set_keys; kept by begin) */
	bool need_sig;           /* this session must end in a good trailer */
	hifi_sha256_t hb;        /* everything before the trailer */
	uint8_t tail[HIFI_OTA_SIG_LEN];
	uint8_t keys[HIFI_OTA_KEYS_MAX][32];
	int nkeys;
	hifi_verify_fn verify;
	/* no going back from the LAN (hifi_ota_set_running): the running app's
	 * project and build number, and the first bytes of the incoming image */
	char run_project[32];
	unsigned long long run_build; /* 0: unknown, so nothing signed from the LAN */
	uint8_t head[HIFI_OTA_DESC_END];
} hifi_ota_t;

void hifi_ota_init(hifi_ota_t *o, const hifi_ota_io_t *io);
/* The running app's project name and build number (hifi_ota_build_of on its
 * HIFI_BUILD_TAG). A signed image from the LAN must then be the same project
 * with a build number no lower than the running one (403 otherwise): an old
 * signed image with a known bug can't be put back from the LAN. The same
 * build again is allowed (a reinstall). With no build number (0) every
 * image from the LAN is refused: the check fails closed. The Hive's relay
 * updates aren't held to it (the bench swaps builds both ways). */
void hifi_ota_set_running(hifi_ota_t *o, const char *project, unsigned long long build);
/* "HIFI-BUILD:1789000000;" -> 1789000000; anything else -> 0 */
unsigned long long hifi_ota_build_of(const char *tag);
/* The keys a trailer may be signed with (the build's Hive keys). */
void hifi_ota_set_keys(hifi_ota_t *o, const uint8_t (*keys)[32], int nkeys, hifi_verify_fn verify);

/* Each returns an HTTP status (200 ok) with *err set otherwise. A begin
 * while one is open drops the old one: the sender restarted. */
int hifi_ota_begin(hifi_ota_t *o, size_t size, const char *sha_hex, const char *version, const char **err);
/* The same for an image from the person's own network (the app's upload,
 * or a URL from a manifest without a hash): no SHA-256 to match, so only
 * the image's own appended hash, which the board's commit checks
 * (esp_ota_end), stands between it and the boot slot. */
int hifi_ota_begin_local(hifi_ota_t *o, size_t size, const char *version, const char **err);
/* Either of them for a request from the LAN when need_sig: finish refuses
 * the image (403) unless its trailer is signed by one of the keys. sha_hex
 * may be NULL. */
int hifi_ota_begin_ex(hifi_ota_t *o, size_t size, const char *sha_hex, const char *version, bool need_sig,
		      const char **err);
/* Checks a whole signed image in memory (the host tests, sign_fw.py's twin):
 * 0 good, or an HTTP status with *err set. */
int hifi_ota_check_sig(const hifi_ota_t *o, const uint8_t body_sha[32], const uint8_t tail[HIFI_OTA_SIG_LEN],
		       const char **err);
/* 409 if off isn't where the image has got to (o->written says where) */
int hifi_ota_chunk(hifi_ota_t *o, size_t off, const uint8_t *d, size_t n, const char **err);
/* checks the hash and makes the image the next boot; the caller restarts */
int hifi_ota_finish(hifi_ota_t *o, const char **err);
void hifi_ota_abort(hifi_ota_t *o);

/* ---- who may touch the session (L10) ----
 * The URL job and the app's upload each run a whole update in one go: they
 * claim the session first (0, or 409 with *err set when it is held or an
 * update is open) and release it at the end: release drops an update the
 * holder left open. While it is held,
 * the API's ota/begin, chunk, finish and abort answer 409. The API does each
 * step under hifi_ota_lock, and claim and release take it too, so no two
 * tasks are ever inside the session at once. */
int hifi_ota_claim(hifi_ota_t *o, const char **err);
void hifi_ota_release(hifi_ota_t *o);
void hifi_ota_lock(hifi_ota_t *o);
void hifi_ota_unlock(hifi_ota_t *o);
