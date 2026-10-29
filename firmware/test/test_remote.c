/* The remote bring-up's core: base64, SHA-256, the OTA session, signed
 * commands, captures and the trial boot. With OpenSSL on the host the
 * commands are signed and checked with real Ed25519 (the board uses
 * libsodium); without it, a stand-in signature tests the same logic. */
#include <stdlib.h>

#include "check.h"
#include "hifi_api.h"
#include "hifi_b64.h"
#include "hifi_boot.h"
#include "hifi_json.h"
#include "hifi_remote.h"
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
static char wifi_ssid[33], wifi_pw[65], relay_url[96];

static int f_open(void *ctx, size_t size, const char **slot, const char **err) {
	(void) ctx, (void) err, (void) size;
	flash_n = 0;
	committed = aborted = false;
	*slot = "ota_1";
	return 0;
}
static int f_write(void *ctx, const uint8_t *d, size_t n) {
	(void) ctx;
	if (flash_n + n > sizeof(flash)) return -1;
	memcpy(flash + flash_n, d, n);
	flash_n += n;
	return 0;
}
static int f_commit(void *ctx, const char **err) {
	(void) ctx, (void) err;
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
	r->keys = 1;
}
static int m_remote_set(const char *url, const char *user, const char *pw, const char *prefix) {
	(void) user, (void) pw, (void) prefix;
	snprintf(relay_url, sizeof(relay_url), "%s", url);
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
}

static char out[200000];

static int api(const char *method, const char *path, const char *body) {
	return hifi_api_handle(&ops, method, path, body, out, sizeof(out));
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
	CHECK(api("GET", "/api/v1/remote", NULL) == 200);
	CHECK_HAS(out, "\"configured\":false");
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"http://x\"}") == 400);
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"mqtts://b.example:8883\",\"prefix\":\"a/b\"}") == 400);
	CHECK(api("POST", "/api/v1/remote", "{\"url\":\"mqtts://b.example:8883\",\"user\":\"board\",\"password\":\"x\"}") == 200);
	CHECK_HAS(out, "\"connected\":true");
	CHECK_HAS(out, "\"dev\":\"240ac4123456\"");
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
	t.relay_configured = t.relay_ok = false; /* no relay set up: Wi-Fi is enough */
	t.wifi_ok = false;
	CHECK(hifi_trial_judge(&t, 200, why, sizeof(why)) == HIFI_TRIAL_BAD);
	t.wifi_configured = false; /* updated over USB, no network set up */
	CHECK(hifi_trial_judge(&t, 1, why, sizeof(why)) == HIFI_TRIAL_GOOD);
	t.component_ok = false;
	CHECK(hifi_trial_judge(&t, 1, why, sizeof(why)) == HIFI_TRIAL_BAD);
}

int main(void) {
	test_b64();
	test_sha256();
	test_ota_core();
	test_ota_api();
	test_capture_api();
	test_wifi_remote_api();
	test_remote();
	test_trial();
	return check_report("remote");
}
