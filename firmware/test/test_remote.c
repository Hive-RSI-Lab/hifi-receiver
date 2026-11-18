/* The remote bring-up's core: base64, SHA-256, the OTA session, signed
 * commands, captures and the trial boot. With OpenSSL on the host the
 * commands are signed and checked with real Ed25519 (the board uses
 * libsodium); without it, a stand-in signature tests the same logic. */
#include <errno.h>
#include <stdlib.h>

#include "check.h"
#include "hifi_api.h"
#include "hifi_b64.h"
#include "hifi_boot.h"
#include "hifi_json.h"
#include "hifi_remote.h"
#include "hifi_ws_proto.h"
#include "hifi_sha256.h"

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#endif

/* ------------------------------------------------------------ signing */

/* RFC 8032 test 1's key: the bench's Python signer is tested on the same one */
static const char *SEED_HEX __attribute__((unused)) = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char *PUB_HEX = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

static void unhex(const char *h, uint8_t *out, size_t n) {
	for (size_t i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &out[i]);
}

#ifdef HAVE_OPENSSL
static bool verify(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
	EVP_PKEY *k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	bool ok = k && c && EVP_DigestVerifyInit(c, NULL, NULL, NULL, k) == 1 &&
		  EVP_DigestVerify(c, sig, 64, msg, n) == 1;
	EVP_MD_CTX_free(c);
	EVP_PKEY_free(k);
	return ok;
}

static void sign(const uint8_t *msg, size_t n, uint8_t sig[64]) {
	uint8_t seed[32];
	unhex(SEED_HEX, seed, 32);
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
	EVP_MD_CTX *c = EVP_MD_CTX_new();
	size_t sl = 64;
	EVP_DigestSignInit(c, NULL, NULL, NULL, k);
	EVP_DigestSign(c, sig, &sl, msg, n);
	EVP_MD_CTX_free(c);
	EVP_PKEY_free(k);
}
#else
/* stand-in: the "signature" is the message's SHA-256 twice, keyed by the pubkey */
static void fake(const uint8_t pub[32], const uint8_t *msg, size_t n, uint8_t sig[64]) {
	hifi_sha256_t s;
	hifi_sha256_init(&s);
	hifi_sha256_update(&s, pub, 32);
	hifi_sha256_update(&s, msg, n);
	hifi_sha256_final(&s, sig);
	memcpy(sig + 32, sig, 32);
}
static bool verify(const uint8_t pub[32], const uint8_t *msg, size_t n, const uint8_t sig[64]) {
	uint8_t want[64];
	fake(pub, msg, n, want);
	return !memcmp(want, sig, 64);
}
static void sign(const uint8_t *msg, size_t n, uint8_t sig[64]) {
	uint8_t pub[32];
	unhex(PUB_HEX, pub, 32);
	fake(pub, msg, n, sig);
}
#endif

/* line 1, '\n', base64 signature of line 1 */
static char *signed_msg(const char *line) {
	uint8_t sig[64];
	sign((const uint8_t *) line, strlen(line), sig);
	char *m = malloc(strlen(line) + 100);
	int n = sprintf(m, "%s\n", line);
	hifi_b64_encode(sig, 64, m + n, 100);
	return m;
}

/* ------------------------------------------------------------ mock board */

static uint8_t flash[64 * 1024];
static size_t flash_n;
static bool committed, aborted, rebooted;
static int captures;
static char wifi_ssid[33], wifi_pw[65], relay_url[128], relay_key[66], relay_user[48];

static int f_open(void *ctx, size_t size, const char **slot, const char **err) {
	(void) ctx, (void) err, (void) size;
	flash_n = 0;
	committed = aborted = false;
	*slot = "ota_1";
	return 0;
}
static bool fail_write, fail_commit;
static int f_write(void *ctx, const uint8_t *d, size_t n) {
	(void) ctx;
	if (fail_write) return -1;
	if (flash_n + n > sizeof(flash)) return -1;
	memcpy(flash + flash_n, d, n);
	flash_n += n;
	return 0;
}
static int f_commit(void *ctx, const char **err) {
	(void) ctx;
	if (fail_commit) return *err = "image does not verify", -1;
	committed = true;
	return 0;
}
static void f_abort(void *ctx) {
	(void) ctx;
	aborted = true;
}
static const hifi_ota_io_t io = {.open = f_open, .write = f_write, .commit = f_commit, .abort = f_abort,
				 .slot_size = sizeof(flash)};
static hifi_ota_t ota;

static void m_info(hifi_info_t *i) {
	i->model = "hifi-lite";
	i->board = "lite-main";
	i->fw = "0.1.0";
	i->upstream = "x";
	snprintf(i->mac, sizeof(i->mac), "24:0a:c4:12:34:56");
}
static void m_snapshot(hifi_snapshot_t *s) { memset(s, 0, sizeof(*s)); }
static void m_reboot(void) { rebooted = true; }
static void m_ota_status(hifi_ota_status_t *s) {
	s->slot = "ota_0";
	s->state = "valid";
	snprintf(s->last_from, sizeof(s->last_from), "0.2.0");
	snprintf(s->last_reason, sizeof(s->last_reason), "relay not reached after 120 s");
}
static int m_capture(hifi_cap_src_t src, int16_t *pcm, int frames, hifi_cap_info_t *info, const char **err) {
	if (src == HIFI_CAP_LINE) return *err = "output idle", 409;
	captures++;
	for (int i = 0; i < frames; i++) {
		pcm[2 * i] = (int16_t) lrint(16384 * sin(2 * M_PI * 1000 * i / 48000.0)); /* -6 dBFS */
		pcm[2 * i + 1] = i % 2 ? 32767 : -32768;                                 /* clipped */
	}
	info->rate = 48000;
	info->ch[0] = "spk_l+";
	info->ch[1] = "spk_r+";
	info->v_per_fs[0] = info->v_per_fs[1] = 14.0f;
	return 0;
}
static int m_set_wifi(const char *ssid, const char *pw) {
	snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
	snprintf(wifi_pw, sizeof(wifi_pw), "%s", pw);
	return 0;
}
static void m_remote_status(hifi_remote_status_t *r) {
	r->configured = relay_url[0];
	r->connected = relay_url[0];
	snprintf(r->url, sizeof(r->url), "%s", relay_url);
	snprintf(r->prefix, sizeof(r->prefix), "hifi");
	snprintf(r->dev, sizeof(r->dev), "240ac4123456");
	r->has_key = relay_key[0];
	r->keys = 1;
}
static int m_remote_set(const char *url, const char *user, const char *pw, const char *prefix, const char *key) {
	(void) pw, (void) prefix;
	snprintf(relay_url, sizeof(relay_url), "%s", url);
	snprintf(relay_user, sizeof(relay_user), "%s", user);
	snprintf(relay_key, sizeof(relay_key), "%s", key);
	return 0;
}

static hifi_ops_t ops = {.info = m_info, .snapshot = m_snapshot, .reboot = m_reboot, .capture = m_capture,
			 .set_wifi = m_set_wifi, .remote_status = m_remote_status, .remote_set = m_remote_set,
			 .ota = &ota, .ota_status = m_ota_status};

/* ------------------------------------------------------------ tests */

static void test_b64(void) {
	char out[64];
	uint8_t back[64];
	const char *cases[][2] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foobar", "Zm9vYmFy"}};
	for (int i = 0; i < 5; i++) {
		size_t n = strlen(cases[i][0]);
		hifi_b64_encode((const uint8_t *) cases[i][0], n, out, sizeof(out));
		CHECK_STR(out, cases[i][1]);
		CHECK(hifi_b64_decode(out, strlen(out), back, sizeof(back)) == (long) n);
		CHECK(!memcmp(back, cases[i][0], n));
	}
	CHECK(hifi_b64_decode("Zm9", 3, back, sizeof(back)) == -1);  /* not a multiple of 4 */
	CHECK(hifi_b64_decode("Z=9v", 4, back, sizeof(back)) == -1); /* padding in the middle */
	CHECK(hifi_b64_decode("Zm9v", 4, back, 2) == -1);            /* over cap */
	CHECK(hifi_b64_decode("Zm!v", 4, back, sizeof(back)) == -1);
	CHECK(hifi_b64_encode((const uint8_t *) "foo", 3, out, 4) == 0); /* no room for the NUL */
}

static void hexs(const uint8_t *d, size_t n, char *out) {
	for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void test_sha256(void) {
	uint8_t h[32];
	char hex[65];
	hifi_sha256_t s;
	hifi_sha256_init(&s);
	hifi_sha256_update(&s, (const uint8_t *) "abc", 3);
	hifi_sha256_final(&s, h);
	hexs(h, 32, hex);
	CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	/* 1,000,000 'a's in uneven pieces: crosses every block boundary case */
	hifi_sha256_init(&s);
	uint8_t a[997];
	memset(a, 'a', sizeof(a));
	size_t left = 1000000;
	while (left) {
		size_t n = left < sizeof(a) ? left : sizeof(a);
		hifi_sha256_update(&s, a, n);
		left -= n;
	}
	hifi_sha256_final(&s, h);
	hexs(h, 32, hex);
	CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void make_image(uint8_t *img, size_t n, char sha_hex[65]) {
	for (size_t i = 0; i < n; i++) img[i] = (uint8_t) (i * 131 + 7);
	img[0] = HIFI_OTA_APP_MAGIC;
	uint8_t h[32];
	hifi_sha256_t s;
	hifi_sha256_init(&s);
	hifi_sha256_update(&s, img, n);
	hifi_sha256_final(&s, h);
	hexs(h, 32, sha_hex);
}

static void test_ota_core(void) {
	static uint8_t img[10000];
	char sha[65];
	const char *err = NULL;
	make_image(img, sizeof(img), sha);
	hifi_ota_init(&ota, &io);

	CHECK(hifi_ota_chunk(&ota, 0, img, 100, &err) == 409); /* nothing open */
	CHECK(hifi_ota_begin(&ota, sizeof(img), "abc", "0.2.0", &err) == 400);
	CHECK(hifi_ota_begin(&ota, sizeof(flash) + 1, sha, "0.2.0", &err) == 400); /* bigger than the slot */
	CHECK(hifi_ota_begin(&ota, sizeof(img), sha, "0.2.0", &err) == 200);
	CHECK_STR(ota.slot, "ota_1");
	CHECK(hifi_ota_chunk(&ota, 4096, img + 4096, 4096, &err) == 409); /* out of order */
	CHECK(ota.written == 0);
	CHECK(hifi_ota_chunk(&ota, 0, img, 4097, &err) == 400);           /* too big */
	CHECK(hifi_ota_finish(&ota, &err) == 409);                         /* incomplete */
	for (size_t off = 0; off < sizeof(img); off += 4096) {
		size_t n = sizeof(img) - off < 4096 ? sizeof(img) - off : 4096;
		CHECK(hifi_ota_chunk(&ota, off, img + off, n, &err) == 200);
	}
	CHECK(hifi_ota_chunk(&ota, sizeof(img), img, 1, &err) == 400); /* past the end */
	CHECK(hifi_ota_finish(&ota, &err) == 200);
	CHECK(committed && !aborted);
	CHECK(flash_n == sizeof(img) && !memcmp(flash, img, sizeof(img)));

	/* a flipped bit is caught and nothing is committed */
	CHECK(hifi_ota_begin(&ota, sizeof(img), sha, "0.2.0", &err) == 200);
	img[5000] ^= 1;
	for (size_t off = 0; off < sizeof(img); off += 4096) {
		size_t n = sizeof(img) - off < 4096 ? sizeof(img) - off : 4096;
		hifi_ota_chunk(&ota, off, img + off, n, &err);
	}
	CHECK(hifi_ota_finish(&ota, &err) == 400);
	CHECK_STR(err, "sha256 mismatch");
	CHECK(!committed && aborted && !ota.active);
	img[5000] ^= 1;

	/* not an app image */
	CHECK(hifi_ota_begin(&ota, sizeof(img), sha, "0.2.0", &err) == 200);
	img[0] = 0;
	CHECK(hifi_ota_chunk(&ota, 0, img, 4096, &err) == 400);
	CHECK(!ota.active);
	img[0] = HIFI_OTA_APP_MAGIC;

	/* the app's upload: no hash to match, the board's commit checks the image */
	CHECK(hifi_ota_begin_local(&ota, sizeof(flash) + 1, NULL, &err) == 400);
	CHECK(hifi_ota_begin_local(&ota, sizeof(img), NULL, &err) == 200);
	img[5000] ^= 1;
	for (size_t off = 0; off < sizeof(img); off += 4096) {
		size_t n = sizeof(img) - off < 4096 ? sizeof(img) - off : 4096;
		CHECK(hifi_ota_chunk(&ota, off, img + off, n, &err) == 200);
	}
	CHECK(hifi_ota_finish(&ota, &err) == 200);
	CHECK(committed);
	img[5000] ^= 1;
	/* and a hashed begin after it checks again */
	CHECK(hifi_ota_begin(&ota, sizeof(img), sha, "0.2.0", &err) == 200);
	CHECK(ota.check_sha);
	hifi_ota_abort(&ota);

	/* the hash in any case, and not a hash at all */
	char up[65];
	for (int i = 0; i < 65; i++) up[i] = (char) (sha[i] >= 'a' && sha[i] <= 'f' ? sha[i] - 32 : sha[i]);
	CHECK(hifi_ota_begin(&ota, sizeof(img), up, "0.2.0", &err) == 200);
	hifi_ota_abort(&ota);
	up[10] = 'g';
	CHECK(hifi_ota_begin(&ota, sizeof(img), up, "0.2.0", &err) == 400);

	/* the flash fails mid-image (full, worn): the slot is given up */
	CHECK(hifi_ota_begin(&ota, sizeof(img), sha, "0.2.0", &err) == 200);
	CHECK(hifi_ota_chunk(&ota, 0, img, 4096, &err) == 200);
	fail_write = true;
	CHECK(hifi_ota_chunk(&ota, 4096, img + 4096, 4096, &err) == 500);
	CHECK_STR(err, "flash write failed");
	CHECK(aborted && !ota.active && !committed);
	fail_write = false;

	/* the board's own check of the image refuses it at commit */
	CHECK(hifi_ota_begin_local(&ota, sizeof(img), NULL, &err) == 200);
	for (size_t off = 0; off < sizeof(img); off += 4096) {
		size_t n = sizeof(img) - off < 4096 ? sizeof(img) - off : 4096;
		hifi_ota_chunk(&ota, off, img + off, n, &err);
	}
	fail_commit = true;
	CHECK(hifi_ota_finish(&ota, &err) == 500);
	CHECK_STR(err, "image does not verify");
	CHECK(aborted && !ota.active && !ota.done);
	fail_commit = false;
}

/* an app description in the image: its project name (esp_app_desc_t) */
static void set_desc(uint8_t *img, const char *proj) {
	img[32] = 0x32, img[33] = 0x54, img[34] = 0xCD, img[35] = 0xAB;
	memset(img + 80, 0, 32);
	memcpy(img + 80, proj, strlen(proj));
}

#define PROJ "Squeezelite-ESP32"
#define BUILD 1789000000ULL /* the running app's build number in these tests */

/* a signed image (firmware/tools/sign_fw.py): body, then "HIFISIG2", the
 * build number (8 bytes LE) and an Ed25519 signature over
 * "hifi-fw-v2:<build>:" + the body's SHA-256 in hex */
static void sign_build(uint8_t *img, size_t n, unsigned long long build) {
	size_t body = n - HIFI_OTA_SIG_LEN;
	uint8_t h[32];
	hifi_sha256_t s;
	hifi_sha256_init(&s);
	hifi_sha256_update(&s, img, body);
	hifi_sha256_final(&s, h);
	char msg[128];
	int c = sprintf(msg, "hifi-fw-v2:%llu:", build);
	hexs(h, 32, msg + c);
	memcpy(img + body, HIFI_OTA_SIG_MAGIC, 8);
	for (int i = 0; i < 8; i++) img[body + 8 + i] = (uint8_t) (build >> (8 * i));
	sign((const uint8_t *) msg, (size_t) c + 64, img + body + 16);
}

/* this project, the running build */
static void sign_image(uint8_t *img, size_t n) {
	set_desc(img, PROJ);
	sign_build(img, n, BUILD);
}

static int feed(const uint8_t *img, size_t n, size_t step, const char **err) {
	int st = 200;
	for (size_t off = 0; off < n && st == 200; off += step)
		st = hifi_ota_chunk(&ota, off, img + off, n - off < step ? n - off : step, err);
	return st == 200 ? hifi_ota_finish(&ota, err) : st;
}

static void test_ota_sig(void) {
	static uint8_t img[10000];
	char sha[65];
	const char *err = NULL;
	uint8_t pub[32];
	unhex(PUB_HEX, pub, 32);
	make_image(img, sizeof(img), sha);
	sign_image(img, sizeof(img));

	/* no key in the build: a signed-only update can't start */
	hifi_ota_init(&ota, &io);
	hifi_ota_set_running(&ota, PROJ, BUILD);
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 403);
	CHECK(!ota.active);
	hifi_ota_set_keys(&ota, (const uint8_t (*)[32]) pub, 1, verify);

	/* a good signature, every chunk size, so the trailer lands across chunk edges */
	size_t steps[] = {4096, 1000, 71, 72, 73, 1};
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		committed = aborted = false;
		CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
		CHECK(feed(img, sizeof(img), steps[i], &err) == 200);
		CHECK(committed && !aborted);
	}
	/* with the Hive's own hash of the whole file as well */
	{
		uint8_t h[32];
		hifi_sha256_t s;
		hifi_sha256_init(&s);
		hifi_sha256_update(&s, img, sizeof(img));
		hifi_sha256_final(&s, h);
		hexs(h, 32, sha);
	}
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), sha, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 200);

	/* a changed body: the signature no longer matches */
	img[300] ^= 1;
	committed = aborted = false;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403);
	CHECK_STR(err, "the image's signature isn't the Hive's");
	CHECK(!committed && aborted && !ota.active);
	img[300] ^= 1;

	/* a changed signature */
	img[sizeof(img) - 1] ^= 0x80;
	committed = aborted = false;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403);
	CHECK(!committed);
	img[sizeof(img) - 1] ^= 0x80;

	/* another key */
	uint8_t other[32];
	memcpy(other, pub, 32);
	other[0] ^= 1;
	hifi_ota_set_keys(&ota, (const uint8_t (*)[32]) other, 1, verify);
	committed = false;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403 && !committed);
	/* the second of two keys */
	uint8_t two[2][32];
	memcpy(two[0], other, 32);
	memcpy(two[1], pub, 32);
	hifi_ota_set_keys(&ota, (const uint8_t (*)[32]) two, 2, verify);
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 200 && committed);

	/* no trailer at all: an unsigned image */
	static uint8_t plain[10000];
	char plain_sha[65];
	make_image(plain, sizeof(plain), plain_sha);
	committed = false;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(plain), NULL, NULL, true, &err) == 200);
	CHECK(feed(plain, sizeof(plain), 4096, &err) == 403);
	CHECK_STR(err, "the image isn't signed by the Hive");
	CHECK(!committed);
	/* the same image is fine where no signature is asked for (the relay, USB) */
	CHECK(hifi_ota_begin_ex(&ota, sizeof(plain), NULL, NULL, false, &err) == 200);
	CHECK(feed(plain, sizeof(plain), 4096, &err) == 200 && committed);
	/* and a signed image is fine there too: ESP-IDF ignores the trailer */
	CHECK(hifi_ota_begin(&ota, sizeof(img), sha, NULL, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 200);

	/* too small to hold an image and a trailer */
	CHECK(hifi_ota_begin_ex(&ota, 1024 + HIFI_OTA_SIG_LEN - 1, NULL, NULL, true, &err) == 400);
}

/* no going back from the LAN (L9): a signed image with a lower build number,
 * or for another project, is refused, and so is everything when the running
 * app has no build number; the relay isn't held to it */
static void test_ota_age(void) {
	static uint8_t img[10000];
	char sha[65];
	const char *err = NULL;
	uint8_t pub[32];
	unhex(PUB_HEX, pub, 32);

	CHECK(hifi_ota_build_of("HIFI-BUILD:1789000000;") == 1789000000ULL);
	CHECK(hifi_ota_build_of("HIFI-BUILD:18446744073709551615;") == 18446744073709551615ULL);
	CHECK(hifi_ota_build_of("HIFI-BUILD:0;") == 0);
	CHECK(hifi_ota_build_of("HIFI-BUILD:;") == 0);
	CHECK(hifi_ota_build_of("HIFI-BUILD:12a;") == 0);
	CHECK(hifi_ota_build_of("HIFI-BUILD:12") == 0);
	CHECK(hifi_ota_build_of("HIFI-BUILD:12;x") == 0);
	CHECK(hifi_ota_build_of("HIFI-BUILD:123456789012345678901;") == 0); /* 21 digits */
	CHECK(hifi_ota_build_of("HIFI-BUILD:HIFI_FW_BUILD;") == 0);        /* built without one */
	CHECK(hifi_ota_build_of("") == 0);
	CHECK(hifi_ota_build_of(NULL) == 0);

	hifi_ota_init(&ota, &io);
	hifi_ota_set_keys(&ota, (const uint8_t (*)[32]) pub, 1, verify);
	hifi_ota_set_running(&ota, PROJ, BUILD);

	struct {
		const char *proj;
		unsigned long long build;
		int st;
	} cases[] = {
		{PROJ, BUILD, 200},          /* the same build again */
		{PROJ, BUILD + 1, 200},      /* newer */
		{PROJ, BUILD + 86400, 200},
		{PROJ, BUILD - 1, 403},      /* older */
		{PROJ, 0, 403},
		{"recovery", BUILD + 1, 403}, /* another project */
		{"", BUILD + 1, 403},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		make_image(img, sizeof(img), sha);
		set_desc(img, cases[i].proj);
		sign_build(img, sizeof(img), cases[i].build);
		committed = aborted = false;
		CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
		/* 7-byte chunks: the description and the trailer arrive split */
		int st = feed(img, sizeof(img), i % 2 ? 7 : 4096, &err);
		CHECK(st == cases[i].st);
		CHECK(committed == (cases[i].st == 200));
		if (cases[i].st != 200) CHECK(aborted && !ota.active);
	}
	CHECK_STR(err, "the image is for another project");

	/* the build number is signed: raising it in the trailer breaks the signature */
	make_image(img, sizeof(img), sha);
	set_desc(img, PROJ);
	sign_build(img, sizeof(img), BUILD - 1);
	img[sizeof(img) - HIFI_OTA_SIG_LEN + 8] += 2;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403);
	CHECK_STR(err, "the image's signature isn't the Hive's");

	/* a v1 trailer (no build number) is refused, not let through */
	make_image(img, sizeof(img), sha);
	sign_image(img, sizeof(img));
	memcpy(img + sizeof(img) - HIFI_OTA_SIG_LEN, "HIFISIG1", 8);
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403);
	CHECK_STR(err, "the image isn't signed by the Hive");

	/* no app description at all */
	make_image(img, sizeof(img), sha);
	sign_build(img, sizeof(img), BUILD);
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403);
	CHECK_STR(err, "the image has no app description");

	/* an old image with a bad signature still says "signature" (the age is read only once it's the Hive's) */
	make_image(img, sizeof(img), sha);
	set_desc(img, PROJ);
	sign_build(img, sizeof(img), BUILD - 100);
	img[500] ^= 1;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403);
	CHECK_STR(err, "the image's signature isn't the Hive's");
	img[500] ^= 1;

	/* the relay and USB (no signature asked) may go back */
	committed = false;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, false, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 200 && committed);

	/* a running app with no build number: nothing from the LAN, fail closed */
	hifi_ota_set_running(&ota, PROJ, 0);
	make_image(img, sizeof(img), sha);
	set_desc(img, PROJ);
	sign_build(img, sizeof(img), BUILD + 1);
	committed = false;
	CHECK(hifi_ota_begin_ex(&ota, sizeof(img), NULL, NULL, true, &err) == 200);
	CHECK(feed(img, sizeof(img), 4096, &err) == 403 && !committed);
	CHECK_STR(err, "this firmware has no build number to check an update against: update it over USB");
	hifi_ota_init(&ota, &io);
}

static char out[200000];

static int api(const char *method, const char *path, const char *body) {
	return hifi_api_handle(&ops, method, path, body, out, sizeof(out));
}

static int lan(const char *method, const char *path, const char *body) {
	return hifi_api_handle_from(&ops, HIFI_SRC_LAN, method, path, body, out, sizeof(out));
}

/* what port 80 may do: no relay changes, signed firmware only, and never
 * a hand in an update the Hive is sending */
static void test_lan_api(void) {
	static uint8_t img[6000];
	char sha[65], body[8192];
	uint8_t pub[32];
	unhex(PUB_HEX, pub, 32);
	hifi_ota_init(&ota, &io);
	hifi_ota_set_keys(&ota, (const uint8_t (*)[32]) pub, 1, verify);
	hifi_ota_set_running(&ota, PROJ, BUILD);

	/* the relay: readable, not changeable */
	CHECK(lan("GET", "/api/v1/remote", NULL) == 200);
	CHECK(lan("POST", "/api/v1/remote", "{\"url\":\"wss://evil.example/r\",\"user\":\"x\",\"pass\":\"y\"}") == 403);
	CHECK(strcmp(relay_url, "wss://evil.example/r"));

	/* an unsigned image from the LAN is refused at the end */
	make_image(img, sizeof(img), sha);
	committed = false;
	snprintf(body, sizeof(body), "{\"size\":%zu,\"sha256\":\"%s\"}", sizeof(img), sha);
	CHECK(lan("POST", "/api/v1/ota/begin", body) == 200);
	CHECK(ota.need_sig);
	int n = sprintf(body, "{\"off\":0,\"data\":\"");
	hifi_b64_encode(img, 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(lan("POST", "/api/v1/ota/chunk", body) == 200);
	n = sprintf(body, "{\"off\":4096,\"data\":\"");
	hifi_b64_encode(img + 4096, sizeof(img) - 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(lan("POST", "/api/v1/ota/chunk", body) == 200);
	CHECK(lan("POST", "/api/v1/ota/finish", NULL) == 403);
	CHECK(!committed);

	/* a signed one goes in */
	sign_image(img, sizeof(img));
	snprintf(body, sizeof(body), "{\"size\":%zu}", sizeof(img));
	CHECK(lan("POST", "/api/v1/ota/begin", body) == 200);
	n = sprintf(body, "{\"off\":0,\"data\":\"");
	hifi_b64_encode(img, 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(lan("POST", "/api/v1/ota/chunk", body) == 200);
	n = sprintf(body, "{\"off\":4096,\"data\":\"");
	hifi_b64_encode(img + 4096, sizeof(img) - 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(lan("POST", "/api/v1/ota/chunk", body) == 200);
	CHECK(lan("POST", "/api/v1/ota/finish", NULL) == 200);
	CHECK(committed);

	/* the Hive's update (relay, unsigned): the LAN can't abort, restart or feed it */
	make_image(img, sizeof(img), sha);
	snprintf(body, sizeof(body), "{\"size\":%zu,\"sha256\":\"%s\"}", sizeof(img), sha);
	CHECK(api("POST", "/api/v1/ota/begin", body) == 200);
	CHECK(lan("POST", "/api/v1/ota/abort", NULL) == 403);
	CHECK(lan("POST", "/api/v1/ota/begin", body) == 403);
	CHECK(lan("POST", "/api/v1/ota/finish", NULL) == 403);
	CHECK(ota.active && !ota.need_sig);
	/* the Hive may take over a LAN upload, though */
	CHECK(api("POST", "/api/v1/ota/abort", NULL) == 200);
	CHECK(lan("POST", "/api/v1/ota/begin", body) == 200);
	CHECK(api("POST", "/api/v1/ota/begin", body) == 200);
	CHECK(!ota.need_sig);
	hifi_ota_abort(&ota);
}

static void test_ota_api(void) {
	static uint8_t img[6000];
	char sha[65], body[8192];
	make_image(img, sizeof(img), sha);
	hifi_ota_init(&ota, &io);
	rebooted = false;

	CHECK(api("GET", "/api/v1/ota", NULL) == 200);
	CHECK_HAS(out, "\"slot\":\"ota_0\"");
	CHECK_HAS(out, "\"state\":\"valid\"");
	CHECK_HAS(out, "\"update\":null");
	CHECK_HAS(out, "\"last_rollback\":{\"from\":\"0.2.0\"");

	/* found by fuzz_api: a size past size_t was cast before any range check (UB) */
	CHECK(api("POST", "/api/v1/ota/begin", "{\"size\":1e122}") == 400);
	CHECK_HAS(out, "larger than the app slot");
	CHECK(api("POST", "/api/v1/ota/begin", "{\"size\":4294967296}") == 400);
	CHECK(api("POST", "/api/v1/ota/begin", "{\"size\":-nan}") == 400);
	/* the chunk offset too, and before any update is open */
	CHECK(api("POST", "/api/v1/ota/chunk", "{\"off\":1e30,\"data\":\"AAAA\"}") == 400);
	CHECK(api("POST", "/api/v1/ota/chunk", "{\"off\":-nan,\"data\":\"AAAA\"}") == 400);

	snprintf(body, sizeof(body), "{\"size\":%zu,\"sha256\":\"%s\",\"version\":\"0.2.0\"}", sizeof(img), sha);
	CHECK(api("POST", "/api/v1/ota/begin", body) == 200);
	CHECK_HAS(out, "\"chunk_max\":4096");
	CHECK(api("GET", "/api/v1/ota/begin", NULL) == 405);

	int n = sprintf(body, "{\"off\":4096,\"data\":\"");
	hifi_b64_encode(img + 4096, sizeof(img) - 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(api("POST", "/api/v1/ota/chunk", body) == 409); /* skipped the first */
	CHECK_HAS(out, "\"next\":0");

	n = sprintf(body, "{\"off\":0,\"data\":\"");
	hifi_b64_encode(img, 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(api("POST", "/api/v1/ota/chunk", body) == 200);
	CHECK_HAS(out, "\"next\":4096");
	CHECK(api("POST", "/api/v1/ota/chunk", "{\"off\":4096,\"data\":\"not base64!\"}") == 400);
	CHECK(api("GET", "/api/v1/ota", NULL) == 200);
	CHECK_HAS(out, "\"written\":4096");

	n = sprintf(body, "{\"off\":4096,\"data\":\"");
	hifi_b64_encode(img + 4096, sizeof(img) - 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(api("POST", "/api/v1/ota/chunk", body) == 200);
	CHECK(api("POST", "/api/v1/ota/finish", NULL) == 200);
	CHECK_HAS(out, "\"reboot\":true");
	CHECK(rebooted && committed);
	CHECK(api("POST", "/api/v1/ota/abort", NULL) == 200);
}

/* L10: the URL job and the upload hold the session; each API step runs under
 * the lock, and nothing is left locked */
static int lock_depth, lock_max, locks;
static void f_lock(void *ctx) {
	(void) ctx;
	locks++;
	if (++lock_depth > lock_max) lock_max = lock_depth;
}
static void f_unlock(void *ctx) {
	(void) ctx;
	lock_depth--;
}
static void test_ota_held(void) {
	static uint8_t img[6000];
	char sha[65], body[8192];
	const char *err = NULL;
	static const hifi_ota_io_t lio = {.open = f_open, .write = f_write, .commit = f_commit, .abort = f_abort,
					  .lock = f_lock, .unlock = f_unlock, .slot_size = sizeof(flash)};
	make_image(img, sizeof(img), sha);
	hifi_ota_init(&ota, &lio);
	lock_depth = lock_max = locks = 0;

	/* the job claims, then begins; the API can't begin over it, from anywhere */
	CHECK(hifi_ota_claim(&ota, &err) == 0);
	CHECK(hifi_ota_claim(&ota, &err) == 409); /* nor can a second job */
	CHECK(hifi_ota_begin_local(&ota, sizeof(img), NULL, &err) == 200);
	snprintf(body, sizeof(body), "{\"size\":%zu,\"sha256\":\"%s\"}", sizeof(img), sha);
	aborted = false;
	CHECK(api("POST", "/api/v1/ota/begin", body) == 409);
	CHECK_HAS(out, "already running");
	CHECK(lan("POST", "/api/v1/ota/begin", body) == 409);
	CHECK(api("POST", "/api/v1/ota/abort", NULL) == 409);
	CHECK(lan("POST", "/api/v1/ota/finish", NULL) == 409);
	int n = sprintf(body, "{\"off\":0,\"data\":\"");
	hifi_b64_encode(img, 4096, body + n, sizeof(body) - n);
	strcat(body, "\"}");
	CHECK(api("POST", "/api/v1/ota/chunk", body) == 409);
	CHECK(ota.active && ota.written == 0 && !aborted); /* untouched */
	CHECK(api("GET", "/api/v1/ota", NULL) == 200); /* reading is fine */

	/* the job writes and finishes; release frees it */
	CHECK(hifi_ota_chunk(&ota, 0, img, 4096, &err) == 200);
	CHECK(hifi_ota_chunk(&ota, 4096, img + 4096, sizeof(img) - 4096, &err) == 200);
	CHECK(hifi_ota_finish(&ota, &err) == 200);
	hifi_ota_release(&ota);
	CHECK(!ota.held);
	snprintf(body, sizeof(body), "{\"size\":%zu,\"sha256\":\"%s\"}", sizeof(img), sha);
	CHECK(api("POST", "/api/v1/ota/begin", body) == 200);

	/* a job can't claim while the API has one open */
	CHECK(hifi_ota_claim(&ota, &err) == 409);
	CHECK(api("POST", "/api/v1/ota/abort", NULL) == 200);
	CHECK(hifi_ota_claim(&ota, &err) == 0);
	/* a job that stops part way: release drops what it left open */
	CHECK(hifi_ota_begin_local(&ota, sizeof(img), NULL, &err) == 200);
	aborted = false;
	hifi_ota_release(&ota);
	CHECK(!ota.active && !ota.held && aborted);

	CHECK(lock_depth == 0 && lock_max == 1 && locks > 10);
	hifi_ota_init(&ota, &io);
}

static void test_capture_api(void) {
	CHECK(hifi_api_reply_cap("/api/v1/capture", "{\"frames\":16384}") > 16384 * 4 * 4 / 3);
	CHECK(hifi_api_reply_cap("/api/v1/state", NULL) == 4096);
	CHECK(api("POST", "/api/v1/capture", "{\"source\":\"loopback\",\"frames\":4800}") == 200);
	CHECK_HAS(out, "\"channels\":[\"spk_l+\",\"spk_r+\"]");
	CHECK_HAS(out, "\"v_per_fs\":[14.000,14.000]");
	CHECK_HAS(out, "\"clipped\":4800");
	CHECK_HAS(out, "\"rms_fs\":0.35355"); /* -6 dBFS peak: 0.5 / sqrt 2 */
	/* the samples come back */
	const char *p;
	size_t n;
	CHECK(hj_get_str_span(out, "pcm", &p, &n));
	CHECK(n == hifi_b64_len(4800 * 4));
	static int16_t pcm[4800 * 2];
	CHECK(hifi_b64_decode(p, n, (uint8_t *) pcm, sizeof(pcm)) == 4800 * 4);
	CHECK(pcm[1] == -32768 && pcm[3] == 32767 && pcm[2 * 12] == 16384); /* 12 samples = a quarter cycle */
	CHECK(api("POST", "/api/v1/capture", "{\"source\":\"line\"}") == 409);
	CHECK(api("POST", "/api/v1/capture", "{\"source\":\"mic\"}") == 400);
	CHECK(api("POST", "/api/v1/capture", "{\"frames\":20000}") == 400);
	/* a buffer too small for the capture is an error, not an overflow */
	char small[1000];
	CHECK(hifi_api_handle(&ops, "POST", "/api/v1/capture", "{}", small, sizeof(small)) == 500);
}

static void test_wifi_remote_api(void) {
	CHECK(api("POST", "/api/v1/wifi", "{\"ssid\":\"home\",\"password\":\"p w\\\"x\"}") == 200);
	CHECK_STR(wifi_ssid, "home");
	CHECK_STR(wifi_pw, "p w\"x");
	CHECK(api("POST", "/api/v1/wifi", "{\"password\":\"x\"}") == 400);
	/* a long ssid or password is refused, not joined cut short; escapes count as
	 * what they decode to (32 bytes of ssid: 31 letters and an escaped e-acute is 33) */
	CHECK(api("POST", "/api/v1/wifi", "{\"ssid\":\"0123456789012345678901234567890\\u00e9\",\"password\":\"x\"}") == 400);
	CHECK_HAS(out, "ssid must be 32 bytes");
	CHECK(api("POST", "/api/v1/wifi", "{\"ssid\":\"012345678901234567890123456789\\u00e9\",\"password\":\"x\"}") == 200);
	CHECK_STR(wifi_ssid, "012345678901234567890123456789\xc3\xa9");
	{
		char b[256], pw[80];
		char good[80];
		memset(good, 'q', 64);
		good[64] = '\0';
		snprintf(b, sizeof(b), "{\"ssid\":\"home\",\"password\":\"%s\"}", good);
		CHECK(api("POST", "/api/v1/wifi", b) == 200);
		CHECK_STR(wifi_pw, good);
		memset(pw, 'p', 64);
		pw[64] = '\0';
		snprintf(b, sizeof(b), "{\"ssid\":\"home\",\"password\":\"%s\\/\"}", pw);
		CHECK(api("POST", "/api/v1/wifi", b) == 400);
		CHECK_HAS(out, "password must be 64");
		CHECK_STR(wifi_pw, good); /* the last good one, not the cut 64 p's */
	}
	CHECK(api("POST", "/api/v1/wifi", "{\"ssid\":\"home\",\"password\":\"p w\\\"x\"}") == 200);
	CHECK(api("GET", "/api/v1/remote", NULL) == 200);
	CHECK_HAS(out, "\"configured\":false");
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"http://x\"}") == 400);
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"mqtts://b.example:8883\",\"prefix\":\"a/b\"}") == 400);
	{
		/* a url, user, password or prefix too long once decoded is refused, with or
		 * without escapes in it; it used to be stored cut when it held an escape */
		char b[512], u[200];
		memset(u, 'a', 112);
		u[112] = '\0';
		snprintf(b, sizeof(b), "{\"url\":\"mqtts://b.example:8883/%s\"}", u);
		CHECK(api("POST", "/api/v1/remote", b) == 400);
		CHECK_HAS(out, "too long");
		snprintf(b, sizeof(b), "{\"url\":\"mqtts://b.example:8883/%s\\/\"}", u);
		CHECK(api("POST", "/api/v1/remote", b) == 400);
		CHECK_HAS(out, "too long");
		CHECK(!relay_url[0]);
		memset(u, 'u', 47);
		u[47] = '\0';
		snprintf(b, sizeof(b), "{\"url\":\"mqtts://b.example:8883\",\"user\":\"%s\\/\"}", u);
		CHECK(api("POST", "/api/v1/remote", b) == 400);
		CHECK_HAS(out, "too long");
		memset(u, 'p', 95);
		u[95] = '\0';
		snprintf(b, sizeof(b), "{\"url\":\"mqtts://b.example:8883\",\"password\":\"%s\\u00e9\"}", u);
		CHECK(api("POST", "/api/v1/remote", b) == 400);
		CHECK_HAS(out, "too long");
		CHECK(api("POST", "/api/v1/remote", "{\"url\":\"mqtts://b.example:8883\",\"prefix\":\"0123456789012345678901234\"}") == 400);
		CHECK_HAS(out, "prefix is too long");
		CHECK(!relay_url[0]);
	}
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"mqtts://b.example:8883\",\"user\":\"board\",\"password\":\"x\"}") == 200);
	CHECK_HAS(out, "\"connected\":true");
	CHECK_HAS(out, "\"dev\":\"240ac4123456\"");
	CHECK_HAS(out, "\"transport\":\"mqtt\"");
	CHECK_STR(relay_key, "");

	/* the Hive's own endpoint over wss://, with the board's key and a service token */
	const char *K = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char b[512];
	snprintf(b, sizeof(b), "{\"url\":\"wss://bench.example.net/v1/dev\",\"key\":\"%s\",\"user\":\"abc.access\",\"password\":\"s3cret\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 200);
	CHECK_STR(relay_key, K);
	CHECK_STR(relay_user, "abc.access");
	CHECK_HAS(out, "\"transport\":\"ws\"");
	CHECK_HAS(out, "\"has_key\":true");
	CHECK(!strstr(out, K) && !strstr(out, "s3cret")); /* never said back */
	CHECK(api("GET", "/api/v1/remote", NULL) == 200);
	CHECK(!strstr(out, K) && !strstr(out, "s3cret"));
	/* no key, a short one, a bad host, plain ws:// */
	relay_key[0] = '\0';
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"wss://bench.example.net/v1/dev\"}") == 400);
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"wss://bench.example.net/v1/dev\",\"key\":\"abcd\"}") == 400);
	snprintf(b, sizeof(b), "{\"url\":\"wss://bad host/v1/dev\",\"key\":\"%s\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 400);
	snprintf(b, sizeof(b), "{\"url\":\"ws://bench.example.net/v1/dev\",\"key\":\"%s\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 400);
	/* a token that would add a header of its own */
	snprintf(b, sizeof(b), "{\"url\":\"wss://bench.example.net/v1/dev\",\"key\":\"%s\",\"user\":\"a\\r\\nX-Evil: 1\",\"password\":\"p\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 400);
	snprintf(b, sizeof(b), "{\"url\":\"wss://bench.example.net/v1/dev\",\"key\":\"%s\",\"user\":\"a\",\"password\":\"p\\tq\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 400);
	snprintf(b, sizeof(b), "{\"url\":\"wss://bench.example.net/v1/dev\",\"key\":\"%s\",\"user\":\"a\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 400); /* an id without its secret */
	/* a url too long for the board is refused, not cut off */
	char longurl[300] = "{\"url\":\"wss://bench.example.net/";
	for (int i = 0; i < 140; i++) strcat(longurl, "a");
	strcat(longurl, "\",\"key\":\"");
	strcat(longurl, K);
	strcat(longurl, "\"}");
	CHECK(api("POST", "/api/v1/remote", longurl) == 400);
	CHECK_STR(relay_key, "");
	/* MQTT again drops the key */
	snprintf(b, sizeof(b), "{\"url\":\"mqtts://b.example:8883\",\"key\":\"%s\"}", K);
	CHECK(api("POST", "/api/v1/remote", b) == 200);
	CHECK_STR(relay_key, "");
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"\"}") == 200);
	CHECK_HAS(out, "\"configured\":false");
	CHECK_HAS(out, "\"transport\":null");
}

static void test_ws_drop(void) {
	char t[64];
	CHECK(hifi_ws_drop_name(HIFI_WS_DROP_NONE) == NULL);
	CHECK_STR(hifi_ws_drop_name(HIFI_WS_DROP_SOCKET), "socket");
	CHECK_STR(hifi_ws_drop_name(HIFI_WS_DROP_STOPPED), "stopped");
	CHECK_STR(hifi_ws_drop_name((hifi_ws_drop_t) 99), "unknown");
	hifi_ws_drop_text(t, sizeof(t), HIFI_WS_DROP_CLOSED, 1001, false);
	CHECK_STR(t, "endpoint closed it (1001)");
	hifi_ws_drop_text(t, sizeof(t), HIFI_WS_DROP_SOCKET, ECONNRESET, false);
	char want[64];
	snprintf(want, sizeof(want), "socket errno %d (reset)", ECONNRESET);
	CHECK_STR(t, want);
	hifi_ws_drop_text(t, sizeof(t), HIFI_WS_DROP_SOCKET, 9999, true);
	CHECK_STR(t, "socket errno 9999, Wi-Fi down");
	hifi_ws_drop_text(t, sizeof(t), HIFI_WS_DROP_WIFI, 0, true); /* not said twice */
	CHECK_STR(t, "Wi-Fi down");
	hifi_ws_drop_text(t, sizeof(t), HIFI_WS_DROP_TLS, 0x7880, false);
	CHECK_STR(t, "TLS error -0x7880");
	hifi_ws_drop_text(t, sizeof(t), HIFI_WS_DROP_WRITE_TIMEOUT, 10, false);
	CHECK_STR(t, "send stalled 10 s");
	CHECK(hifi_ws_drop_text(t, 8, HIFI_WS_DROP_EOF, 0, false) > 7); /* cut, NUL kept */
	CHECK(strlen(t) == 7);
	hifi_ws_drop_line(t, sizeof(t), 120, HIFI_WS_DROP_EOF, 0, false);
	CHECK_STR(t, "120s ago: far end closed the socket");
	/* the worst real case of each kind fits a self-test value (63 characters) */
	static const int codes[] = {0, 10, 4999, 1001, ECONNRESET, ECONNABORTED, ETIMEDOUT, EHOSTUNREACH, ENETUNREACH,
				    ENOTCONN, EPIPE, ENOMEM, 0x7880, 999};
	char line[96];
	for (int k = HIFI_WS_DROP_NONE; k <= HIFI_WS_DROP_STOPPED; k++)
		for (size_t c = 0; c < sizeof(codes) / sizeof(codes[0]); c++)
			for (int w = 0; w < 2; w++) {
				hifi_ws_drop_line(line, sizeof(line), 9999999, (hifi_ws_drop_t) k, codes[c], w);
				CHECK(strlen(line) <= 63);
			}
}

static void test_ws_proto(void) {
	char host[HIFI_WS_HOST_MAX], path[HIFI_WS_PATH_MAX];
	int port = 0;
	CHECK(hifi_ws_url_parse("wss://h2-bench.example.cc/v1/dev", host, &port, path) == 0);
	CHECK_STR(host, "h2-bench.example.cc");
	CHECK_STR(path, "/v1/dev");
	CHECK(port == 443);
	CHECK(hifi_ws_url_parse("wss://10.0.0.2:8443", host, &port, path) == 0);
	CHECK(port == 8443);
	CHECK_STR(path, "/");
	CHECK(hifi_ws_url_parse("ws://h/v1", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h:0/", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h:70000/", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h:/", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h@evil/", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://-h/", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h/a b", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h/a\r\nX: y", host, &port, path) == -1);
	CHECK(hifi_ws_url_parse("wss://h?x", host, &port, path) == -1);

	const char *K = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	CHECK(hifi_ws_key_ok(K));
	CHECK(!hifi_ws_key_ok("0123"));
	CHECK(!hifi_ws_key_ok("g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
	CHECK(!hifi_ws_key_ok(NULL));

	char hd[HIFI_WS_HEADERS_MAX];
	int n = hifi_ws_headers(hd, sizeof(hd), "abc.access", "sec", "240ac4123456", K);
	CHECK(n > 0 && (size_t) n == strlen(hd));
	CHECK_STR(hd, "CF-Access-Client-Id: abc.access\r\nCF-Access-Client-Secret: sec\r\n"
		      "X-Bench-Dev: 240ac4123456\r\nX-Bench-Key: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n");
	CHECK(hifi_ws_headers(hd, sizeof(hd), "", "", "240ac4123456", K) > 0); /* on a LAN, no token */
	CHECK(!strstr(hd, "CF-Access"));
	CHECK(hifi_ws_headers(hd, sizeof(hd), "a\r\nX: 1", "s", "240ac4123456", K) == -1);
	CHECK(hd[0] == '\0');
	CHECK(hifi_ws_headers(hd, sizeof(hd), "a", "s\n", "240ac4123456", K) == -1);
	CHECK(hifi_ws_headers(hd, sizeof(hd), "a", "s\x7f", "240ac4123456", K) == -1);
	CHECK(hifi_ws_headers(hd, sizeof(hd), "a", "", "240ac4123456", K) == -1);
	CHECK(hifi_ws_headers(hd, sizeof(hd), "a", "s", "", K) == -1);
	CHECK(hifi_ws_headers(hd, sizeof(hd), "a", "s", "240ac4123456", "nokey") == -1);
	CHECK(hifi_ws_headers(hd, 40, "abc.access", "sec", "240ac4123456", K) == -1); /* too small: nothing half-written */
	CHECK(hd[0] == '\0');

	char b[64] = "{\"id\":7,\"status\":200}";
	size_t l = hifi_ws_tag(b, strlen(b), sizeof(b), "reply");
	CHECK_STR(b, "{\"t\":\"reply\",\"id\":7,\"status\":200}");
	CHECK(l == strlen(b));
	char e[13] = "{}";
	CHECK(hifi_ws_tag(e, 2, sizeof(e), "event") == 0); /* no room */
	CHECK_STR(e, "{}");
	char e2[32] = "{}";
	CHECK(hifi_ws_tag(e2, 2, sizeof(e2), "event") == strlen("{\"t\":\"event\"}"));
	CHECK_STR(e2, "{\"t\":\"event\"}");
	char x[32] = "[1]";
	CHECK(hifi_ws_tag(x, 3, sizeof(x), "status") == 0);
	char q[32] = "{\"a\":1}";
	CHECK(hifi_ws_tag(q, strlen(q), sizeof(q), "x\"y") == 0);
	CHECK(hifi_ws_tag(q, strlen(q), 7 + 13, "status") == 0); /* exactly one short */
	CHECK(hifi_ws_tag(q, strlen(q), 7 + 14, "status") == 20);
	CHECK_STR(q, "{\"t\":\"status\",\"a\":1}");
}

static void test_ws_frames(void) {
	const uint8_t mk[4] = {1, 2, 3, 4};
	uint8_t h[HIFI_WS_HEAD_MAX];
	hifi_ws_head_t p;
	CHECK(hifi_ws_head(h, 1, 5, mk) == 6);
	CHECK(h[0] == 0x81 && h[1] == 0x85 && h[2] == 1 && h[5] == 4);
	CHECK(hifi_ws_head_len(h) == 6);
	CHECK(hifi_ws_head_parse(h, &p) == 0 && p.op == 1 && p.fin && p.masked && p.len == 5 && p.mask[3] == 4);
	CHECK(hifi_ws_head(h, 1, 200, mk) == 8); /* 200: a low byte with its top bit set */
	CHECK(h[1] == (0x80 | 126) && h[2] == 0 && h[3] == 200);
	CHECK(hifi_ws_head_len(h) == 8);
	CHECK(hifi_ws_head_parse(h, &p) == 0 && p.len == 200);
	CHECK(hifi_ws_head(h, 2, 70000, mk) == 14);
	CHECK(hifi_ws_head_len(h) == 14 && hifi_ws_head_parse(h, &p) == 0 && p.len == 70000 && p.op == 2);
	CHECK(hifi_ws_head(h, 9, 0, mk) == 6 && h[0] == 0x89 && h[1] == 0x80);
	const uint8_t srv[] = {0x81, 126, 0x01, 0xc8}; /* the Hive's: unmasked, 456 bytes */
	CHECK(hifi_ws_head_len(srv) == 4 && hifi_ws_head_parse(srv, &p) == 0 && !p.masked && p.len == 456 && p.op == 1);
	const uint8_t rsv[] = {0xc1, 5};
	CHECK(hifi_ws_head_parse(rsv, &p) == -1);
	const uint8_t bigping[] = {0x89, 126, 0, 200};
	CHECK(hifi_ws_head_parse(bigping, &p) == -1);
	const uint8_t split[] = {0x08, 2};
	CHECK(hifi_ws_head_parse(split, &p) == -1);
	const uint8_t cont[] = {0x01, 3}; /* a text frame without FIN: parsed, the caller drops it */
	CHECK(hifi_ws_head_parse(cont, &p) == 0 && !p.fin);

	const char *ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nsec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
	CHECK(hifi_ws_upgrade_ok(ok, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
	CHECK(!hifi_ws_upgrade_ok(ok, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo"));
	CHECK(!hifi_ws_upgrade_ok(ok, "x"));
	CHECK(!hifi_ws_upgrade_ok("HTTP/1.1 401 Unauthorized\r\nSec-WebSocket-Accept: a\r\n\r\n", "a"));
	CHECK(!hifi_ws_upgrade_ok("HTTP/1.1 1010 x\r\nSec-WebSocket-Accept: a\r\n\r\n", "a"));
	CHECK(!hifi_ws_upgrade_ok("HTTP/1.1 101 x\r\nX-Sec-WebSocket-Accept: a\r\n\r\n", "a"));
	CHECK(hifi_ws_upgrade_ok("HTTP/1.1 101 x\r\nSec-WebSocket-Accept:a \r\n\r\n", "a"));
}

static hifi_remote_t rem;

static int remote(const char *msg, int64_t now) {
	return hifi_remote_handle(&rem, &ops, msg, strlen(msg), now, out, sizeof(out));
}

static void test_remote(void) {
	memset(&rem, 0, sizeof(rem));
	uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0x12, 0x34, 0x56};
	hifi_remote_dev_id(mac, rem.dev);
	CHECK_STR(rem.dev, "240ac4123456");
	rem.verify = verify;

	char *m = signed_msg("{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1761700000123,\"ts\":1761700000,\"method\":\"GET\",\"path\":\"/api/v1/info\",\"body\":null}");
	CHECK(remote(m, 0) == 401); /* no keys in the build: nothing is trusted */
	CHECK_HAS(out, "no trusted key");
	char keys[80];
	snprintf(keys, sizeof(keys), "zz, %s", PUB_HEX);
	CHECK(hifi_remote_set_keys(&rem, keys) == 1);
	CHECK(remote(m, 1761700100) == 200);
	CHECK_HAS(out, "{\"id\":1761700000123,\"body\":{\"model\":\"hifi-lite\"");
	CHECK_HAS(out, ",\"status\":200}");
	CHECK(hj_valid_object(out));
	CHECK(remote(m, 1761700100) == 409); /* the same again: a replay */
	CHECK_HAS(out, "stale id");
	free(m);

	/* a POST with a nested body, and a reply bigger than 4 KB */
	m = signed_msg("{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1761700000200,\"ts\":1761700001,\"method\":\"POST\",\"path\":\"/api/v1/capture\",\"body\":{\"source\":\"loopback\",\"frames\":2048}}");
	CHECK(hifi_remote_reply_cap(m, strlen(m)) > 2048 * 4 * 4 / 3);
	int before = captures;
	CHECK(remote(m, 0) == 200);
	CHECK(captures == before + 1);
	CHECK(hj_valid_object(out));
	free(m);

	/* tampered: one character of the signed line changed */
	m = signed_msg("{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1761700000300,\"ts\":1761700002,\"method\":\"POST\",\"path\":\"/api/v1/amp\",\"body\":{\"enable\":true}}");
	char *t = strstr(m, "true");
	memcpy(t, "TRUE", 4);
	CHECK(remote(m, 0) == 401);
	CHECK(rem.last_id == 1761700000200ull); /* a rejected command moves nothing */
	free(m);

	/* unsigned, or a broken signature */
	CHECK(remote("{\"v\":1}", 0) == 401);
	CHECK(remote("{\"v\":1}\nAAAA", 0) == 401);

	/* signed, but for another board */
	m = signed_msg("{\"v\":1,\"dev\":\"240ac4999999\",\"id\":1761700000400,\"ts\":1761700003,\"method\":\"GET\",\"path\":\"/api/v1/info\",\"body\":null}");
	CHECK(remote(m, 0) == 401);
	CHECK_HAS(out, "not for this board");
	free(m);

	/* signed, but the clock says it's old */
	m = signed_msg("{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1761700000500,\"ts\":1761000000,\"method\":\"GET\",\"path\":\"/api/v1/info\",\"body\":null}");
	CHECK(remote(m, 1761700100) == 409);
	CHECK(remote(m, 0) == 200); /* before the clock is set, ts can't be checked */
	free(m);

#ifdef HAVE_OPENSSL
	/* signed by the bench's own Python signer (hifibench/relay.py), byte for byte */
	{
		const char *py = "{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1761700000999,\"ts\":1761700000,\"method\":\"GET\",\"path\":\"/api/v1/info\",\"body\":null}\n2V0218e7A/iPtVrEqDQrKCH8yqOfH2TRxwlquLOOUMx6J5GrjrEAh4Kzor0bP1u4QTuyTk0xWqhTWzVB1cqgCg==";
		CHECK(remote(py, 0) == 200);
		rem.accepted--; /* keep the count below about this file's own commands */
	}
#endif

	/* signed, but not a command */
	m = signed_msg("{\"v\":2,\"dev\":\"240ac4123456\",\"id\":1761700000600}");
	CHECK(remote(m, 0) == 400);
	free(m);
	CHECK(rem.accepted == 3 && rem.rejected >= 8);

	/* ids out of a uint64's range, before any signature is checked: no
	 * undefined cast (fuzzing found it), just a rejection */
	CHECK(remote("{\"v\":1,\"id\":1e30}\nAAAA", 0) == 401);
	CHECK(remote("{\"v\":1,\"id\":-1e300}\nAAAA", 0) == 401);
	CHECK(remote("{\"v\":1,\"id\":-nan}\nAAAA", 0) == 401);
	CHECK(remote("{\"v\":1,\"id\":18446744073709551616}\nAAAA", 0) == 401);
	m = signed_msg("{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1e30,\"ts\":1761700004,\"method\":\"GET\",\"path\":\"/api/v1/info\",\"body\":null}");
	unsigned long long last = rem.last_id;
	CHECK(remote(m, 0) == 400); /* signed, but no usable id: it can't jam the id clock */
	CHECK(rem.last_id == last);
	free(m);

	/* keys in capitals count too */
	char upk[80];
	snprintf(upk, sizeof(upk), "%s", PUB_HEX);
	for (char *c = upk; *c; c++)
		if (*c >= 'a' && *c <= 'f') *c = (char) (*c - 32);
	CHECK(hifi_remote_set_keys(&rem, upk) == 1);
	CHECK(hifi_remote_set_keys(&rem, PUB_HEX) == 1);

	/* a reply buffer too small for even the envelope */
	m = signed_msg("{\"v\":1,\"dev\":\"240ac4123456\",\"id\":1761700001700,\"ts\":1761700005,\"method\":\"GET\",\"path\":\"/api/v1/info\",\"body\":{}}");
	char small[48];
	unsigned long acc = rem.accepted, rej = rem.rejected;
	CHECK(hifi_remote_handle(&rem, &ops, m, strlen(m), 0, small, sizeof(small)) == 500);
	CHECK(strlen(small) < sizeof(small));
	CHECK(rem.accepted == acc && rem.rejected == rej + 1); /* counted once, as rejected */
	free(m);
}

static void test_trial(void) {
	char why[96];
	hifi_trial_t t = {.component_ok = true, .wifi_configured = true, .wifi_ok = true, .relay_configured = true,
			  .relay_ok = false, .i2c_ok = true, .heap_ok = true};
	CHECK(hifi_trial_judge(&t, 10, why, sizeof(why)) == HIFI_TRIAL_WAIT);
	CHECK_STR(why, "relay not reached");
	CHECK(hifi_trial_judge(&t, HIFI_TRIAL_LIMIT_S, why, sizeof(why)) == HIFI_TRIAL_BAD);
	CHECK_STR(why, "relay not reached after 120 s");
	t.relay_ok = true;
	CHECK(hifi_trial_judge(&t, 30, why, sizeof(why)) == HIFI_TRIAL_GOOD);
	/* "cannot open the home network": a stray listener holds the trial and
	 * rolls the image back at the limit */
	t.listen_wait = true;
	CHECK(hifi_trial_judge(&t, 30, why, sizeof(why)) == HIFI_TRIAL_WAIT);
	CHECK_STR(why, "listener scan not done");
	t.listen_wait = false;
	t.listen_open = "1 open: tcp/23";
	CHECK(hifi_trial_judge(&t, 30, why, sizeof(why)) == HIFI_TRIAL_WAIT);
	CHECK_STR(why, "listening outside the allow list (1 open: tcp/23)");
	CHECK(hifi_trial_judge(&t, HIFI_TRIAL_LIMIT_S, why, sizeof(why)) == HIFI_TRIAL_BAD);
	CHECK_STR(why, "listening outside the allow list (1 open: tcp/23) after 120 s");
	t.listen_open = NULL;
	t.relay_configured = t.relay_ok = false; /* no relay set up: Wi-Fi is enough */
	t.wifi_ok = false;
	CHECK(hifi_trial_judge(&t, 200, why, sizeof(why)) == HIFI_TRIAL_BAD);
	t.wifi_configured = false; /* updated over USB, no network set up */
	CHECK(hifi_trial_judge(&t, 1, why, sizeof(why)) == HIFI_TRIAL_GOOD);
	t.lab_on = true; /* a lab build whose frame never comes up rolls back */
	CHECK(hifi_trial_judge(&t, 5, why, sizeof(why)) == HIFI_TRIAL_WAIT);
	CHECK_STR(why, "the lab frame isn't up");
	CHECK(hifi_trial_judge(&t, HIFI_TRIAL_LIMIT_S, why, sizeof(why)) == HIFI_TRIAL_BAD);
	t.lab_ok = true;
	CHECK(hifi_trial_judge(&t, 5, why, sizeof(why)) == HIFI_TRIAL_GOOD);
	t.component_ok = false;
	CHECK(hifi_trial_judge(&t, 1, why, sizeof(why)) == HIFI_TRIAL_BAD);
}

/* a restart asked for on trial (an IP change) waits for the image to be kept,
 * or the bootloader rolls the new image back */
static void test_restart_hold(void) {
	hifi_restart_hold_t h = {0};
	const char *why = NULL;
	CHECK(!hifi_restart_hold(&h, "ip_change")); /* not on trial: restart now */
	CHECK(!h.held);
	CHECK(!hifi_restart_release(&h, &why));
	CHECK(why == NULL);
	h.on_trial = true; /* booted on trial */
	CHECK(hifi_restart_hold(&h, "ip_change"));
	CHECK(h.held);
	CHECK(hifi_restart_hold(&h, "ip_change")); /* a second change: still held */
	CHECK(hifi_restart_release(&h, &why)); /* marked good: restart now */
	CHECK_STR(why, "ip_change");
	CHECK(!h.held && !h.on_trial);
	CHECK(!hifi_restart_release(&h, &why)); /* once */
	CHECK(!hifi_restart_hold(&h, "ip_change")); /* kept: the next change restarts at once */
	/* marked good with nothing held: later changes restart at once */
	h = (hifi_restart_hold_t){.on_trial = true};
	CHECK(!hifi_restart_release(&h, &why));
	CHECK(!hifi_restart_hold(&h, "ip_change"));
	h = (hifi_restart_hold_t){.on_trial = true};
	CHECK(hifi_restart_hold(&h, NULL));
	CHECK(hifi_restart_release(&h, &why));
	CHECK_STR(why, "held");
}

/* the boot heap: taken once the board has settled, and the self-test's line */
static void test_boot_heap(void) {
	CHECK(!hifi_heap_boot_due(5, true, true, true)); /* too early, even with all up */
	CHECK(hifi_heap_boot_due(HIFI_HEAP_BOOT_AFTER_S, true, true, true));
	CHECK(!hifi_heap_boot_due(40, true, true, false)); /* waits for the bench link */
	CHECK(hifi_heap_boot_due(40, true, false, false)); /* none set up: the network is enough */
	CHECK(!hifi_heap_boot_due(40, false, false, false));
	CHECK(hifi_heap_boot_due(HIFI_HEAP_BOOT_LATEST_S, false, true, false)); /* taken by 90 s regardless */

	char v[64];
	CHECK(hifi_heap_line(15000, 17890, v, sizeof(v)));
	CHECK_STR(v, "15000 B free, 17890 B at boot, 5890 over the 12000 floor");
	CHECK(strlen(v) < sizeof(v));
	CHECK(hifi_heap_line(12001, 11535, v, sizeof(v)));
	CHECK_STR(v, "12001 B free, 11535 B at boot, 465 under the 12000 floor");
	CHECK(!hifi_heap_line(12000, 0, v, sizeof(v))); /* the floor itself fails, as before */
	CHECK_STR(v, "12000 B free, boot not measured yet (floor 12000)");
	CHECK(hifi_heap_line(327680, 327680, v, sizeof(v))); /* all of the ESP32's RAM still fits */
	CHECK_STR(v, "327680 B free, 327680 B at boot, 315680 over the 12000 floor");
	CHECK(hifi_heap_line(4294967295u, 4294967295u, v, sizeof(v))); /* nonsense is cut, not overrun */
	CHECK(strlen(v) == sizeof(v) - 1);
}

/* test_sign_fw.py: does an image tools/sign_fw.py signed (with SEED_HEX)
 * install from the LAN on a board running build `running`? Prints the status
 * and the error; 0 when it matches `want`. */
static int check_file(const char *path, const char *running, int want) {
	static uint8_t img[60000];
	FILE *f = fopen(path, "rb");
	if (!f) return 2;
	size_t n = fread(img, 1, sizeof(img), f);
	fclose(f);
	const char *err = "";
	uint8_t pub[32];
	unhex(PUB_HEX, pub, 32);
	hifi_ota_init(&ota, &io);
	hifi_ota_set_keys(&ota, (const uint8_t (*)[32]) pub, 1, verify);
	hifi_ota_set_running(&ota, PROJ, strtoull(running, NULL, 10));
	committed = false;
	int st = hifi_ota_begin_ex(&ota, n, NULL, NULL, true, &err);
	if (st == 200) st = feed(img, n, 4096, &err);
	printf("%d %s\n", st, st == 200 ? "" : err);
	return st == want && committed == (want == 200) ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc == 5 && !strcmp(argv[1], "--check-file")) return check_file(argv[2], argv[3], atoi(argv[4]));
	test_b64();
	test_sha256();
	test_ota_core();
	test_ota_api();
	test_ota_sig();
	test_ota_age();
	test_lan_api();
	test_ota_held();
	test_capture_api();
	test_wifi_remote_api();
	test_ws_proto();
	test_ws_drop();
	test_ws_frames();
	test_remote();
	test_trial();
	test_restart_hold();
	test_boot_heap();
	return check_report("remote");
}
