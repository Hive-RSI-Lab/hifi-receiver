/* Party mode's lossless coder (v2): every packet round-trips bit for bit
 * (tones, full-scale edges, noise, mono, short packets), the packet falls
 * back to PCM when the code saves nothing, version 1 packets still decode,
 * and no input, random or a valid code with bits flipped, can make the
 * decoder read or write out of bounds (ASan) or return wrong-length audio.
 *
 * HIFI_PARTY_MUSIC=file.s16 (raw 16-bit little-endian stereo, any length)
 * also codes that file packet by packet, checks each round trip and prints
 * the size against PCM and the host time per packet. */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "check.h"
#include "hifi_party.h"

#define N HIFI_PARTY_FRAMES
#define RAWB (N * HIFI_PARTY_FRAME_BYTES)

static const uint8_t KEY[HIFI_LINK_KEY] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t) (rng >> 32);
}

static uint32_t pk(int32_t l, int32_t r) { return (uint32_t) (uint16_t) (int16_t) l | (uint32_t) (uint16_t) (int16_t) r << 16; }
static int32_t clip(double v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t) lrint(v); }

/* codes and decodes n frames; returns the code's bytes (0: didn't fit RAWB) */
static size_t trip(const uint32_t *f, int n) {
	uint8_t code[2 * RAWB]; /* the extremes code larger than PCM */
	uint32_t back[N];
	size_t c = hifi_party_code(f, n, code, sizeof(code));
	CHECK(c > 0);
	if (!c) return 0;
	memset(back, 0xa5, sizeof(back));
	bool ok = hifi_party_uncode(code, c, n, back);
	CHECK(ok);
	CHECK(ok && memcmp(f, back, (size_t) n * 4) == 0);
	/* the exact length, no more, no less */
	CHECK(!hifi_party_uncode(code, c - 1, n, back));
	code[c] = 0;
	CHECK(!hifi_party_uncode(code, c + 1, n, back));
	return c;
}

static void round_trips(void) {
	uint32_t f[N];
	/* silence: about 2 bits a sample */
	memset(f, 0, sizeof(f));
	size_t c = trip(f, N);
	CHECK(c > 0 && c < 100);
	/* two tones */
	for (int i = 0; i < N; i++) f[i] = pk(clip(12000 * sin(i * 0.0627)), clip(9000 * sin(i * 0.211 + 1)));
	c = trip(f, N);
	CHECK(c > 0 && c < RAWB / 2);
	/* the extremes: full-scale square at Nyquist, each order's worst case */
	for (int i = 0; i < N; i++) f[i] = (i & 1) ? pk(32767, -32768) : pk(-32768, 32767);
	trip(f, N);
	for (int i = 0; i < N; i++) f[i] = (i % 3) ? pk(-32768, -32768) : pk(32767, 32767);
	trip(f, N);
	for (int i = 0; i < N; i++) f[i] = pk((i & 2) ? 32767 : -32768, (i & 1) ? -32768 : 32767);
	trip(f, N);
	/* mono (L = R): S is all zero, so a side mode wins */
	for (int i = 0; i < N; i++) {
		int32_t v = clip(20000 * sin(i * 0.05) + (int32_t) (rnd() % 64) - 32);
		f[i] = pk(v, v);
	}
	size_t mono = trip(f, N);
	for (int i = 0; i < N; i++) f[i] = pk((int16_t) (uint16_t) f[i], 0);
	CHECK(mono > 0 && mono < trip(f, N) + 8);
	/* white noise: the code is a little larger than PCM, and still exact */
	for (int i = 0; i < N; i++) f[i] = rnd();
	uint8_t code[RAWB + 64];
	c = hifi_party_code(f, N, code, sizeof(code));
	CHECK(c > RAWB);
	uint32_t back[N];
	CHECK(c && hifi_party_uncode(code, c, N, back) && memcmp(f, back, sizeof(f)) == 0);
	CHECK(hifi_party_code(f, N, code, RAWB - 1) == 0); /* doesn't fit: 0, no overrun (ASan) */
	/* every length, random walks of every size */
	for (int n = 1; n <= N; n += (n < 8 ? 1 : 37)) {
		int32_t l = 0, r = 0;
		for (int i = 0; i < n; i++) {
			l = clip(l + (int32_t) (rnd() % 2001) - 1000);
			r = clip(r + (int32_t) (rnd() % 401) - 200);
			f[i] = pk(l, r);
		}
		trip(f, n);
	}
	CHECK(hifi_party_code(f, 0, code, sizeof(code)) == 0);
	CHECK(hifi_party_code(f, N + 1, code, sizeof(code)) == 0);
	CHECK(!hifi_party_uncode(code, 1, N, back));
}

/* random inputs and corrupted codes: never out of bounds, never a wrong count */
static void hostile(void) {
	uint8_t in[RAWB];
	uint32_t out[N + 1];
	int ok = 0;
	for (int t = 0; t < 20000; t++) {
		size_t len = 2 + rnd() % (RAWB - 2);
		for (size_t i = 0; i < len; i++) in[i] = (uint8_t) rnd();
		out[N] = 0xdeadbeef;
		ok += hifi_party_uncode(in, len, 1 + (int) (rnd() % N), out);
		CHECK(out[N] == 0xdeadbeef);
	}
	CHECK(ok < 20000); /* most random bytes aren't a whole code */
	uint32_t f[N];
	for (int i = 0; i < N; i++) f[i] = pk(clip(8000 * sin(i * 0.03) + (rnd() % 400)), clip(6000 * cos(i * 0.07)));
	uint8_t code[RAWB + 64];
	size_t c = hifi_party_code(f, N, code, sizeof(code));
	for (int t = 0; t < 20000; t++) {
		uint8_t m[RAWB + 64];
		memcpy(m, code, c);
		int flips = 1 + (int) (rnd() % 3);
		for (int j = 0; j < flips; j++) m[rnd() % c] ^= (uint8_t) (1u << (rnd() % 8));
		out[N] = 0xdeadbeef;
		hifi_party_uncode(m, c, N, out);
		CHECK(out[N] == 0xdeadbeef);
	}
}

static void retag(const hifi_link_key_t *k, uint8_t *buf, size_t n) {
	hifi_sha256_t s = k->in;
	uint8_t h[32];
	hifi_sha256_update(&s, buf, n - HIFI_PARTY_TAG);
	hifi_sha256_final(&s, h);
	s = k->out;
	hifi_sha256_update(&s, h, sizeof(h));
	hifi_sha256_final(&s, h);
	memcpy(buf + n - HIFI_PARTY_TAG, h, HIFI_PARTY_TAG);
}

static void packets(void) {
	hifi_link_key_t k;
	hifi_link_key_init(&k, KEY);
	uint32_t f[N], out[N];
	for (int i = 0; i < N; i++) f[i] = pk(clip(15000 * sin(i * 0.02)), clip(15000 * sin(i * 0.021)));
	hifi_party_pkt_t p = {.type = HIFI_PARTY_AUDIO, .group = 7, .seq = 3, .k = 352 * 9, .rate = 44100, .n = N,
			      .flags = HIFI_PARTY_F_CODED | HIFI_PARTY_F_MARKS};
	uint8_t buf[HIFI_PARTY_MAX];
	size_t n = hifi_party_encode(&k, &p, f, buf, sizeof(buf));
	CHECK(n > 0 && n < HIFI_PARTY_MAX * 6 / 10);
	CHECK(buf[2] == 2);
	hifi_party_pkt_t q;
	const uint8_t *pl;
	size_t plen;
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(q.flags == (HIFI_PARTY_F_CODED | HIFI_PARTY_F_MARKS) && q.n == N && plen == n - HIFI_PARTY_HDR - HIFI_PARTY_TAG);
	CHECK(hifi_party_audio_frames(&q, pl, plen, out) && memcmp(out, f, sizeof(f)) == 0);
	/* a cap too small for the code is too small for the packet */
	CHECK(hifi_party_encode(&k, &p, f, buf, n - 1) == 0);
	CHECK(hifi_party_encode(&k, &p, f, buf, n) == n);

	/* noise: falls back to PCM, the flag cleared, the v1 size */
	uint32_t nz[N];
	for (int i = 0; i < N; i++) nz[i] = rnd();
	n = hifi_party_encode(&k, &p, nz, buf, sizeof(buf));
	CHECK(n == HIFI_PARTY_MAX && buf[2] == 1);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK && q.flags == HIFI_PARTY_F_MARKS);
	CHECK(hifi_party_audio_frames(&q, pl, plen, out) && memcmp(out, nz, sizeof(nz)) == 0);

	/* uncoded, as before */
	p.flags = 0;
	n = hifi_party_encode(&k, &p, f, buf, sizeof(buf));
	CHECK(n == HIFI_PARTY_MAX);
	/* only a coded packet says version 2, so a v1 board still reads the rest */
	CHECK(buf[2] == 1);
	/* a version 1 leader's packet still decodes */
	buf[2] = 1;
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(hifi_party_audio_frames(&q, pl, plen, out) && memcmp(out, f, sizeof(f)) == 0);
	/* a version this board doesn't know: refused before the tag */
	buf[2] = 3;
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_MAGIC);
	buf[2] = 0;
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_MAGIC);

	/* a coded packet claimed by version 1: no such thing */
	p.flags = HIFI_PARTY_F_CODED;
	n = hifi_party_encode(&k, &p, f, buf, sizeof(buf));
	buf[2] = 1;
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_FORMAT);
	/* the CODED flag on a full-size PCM payload: a code is always shorter */
	p.flags = 0;
	n = hifi_party_encode(&k, &p, f, buf, sizeof(buf));
	buf[36] |= HIFI_PARTY_F_CODED;
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_FORMAT);
	/* CODED with SILENCE: refused both ways */
	p.flags = HIFI_PARTY_F_CODED | HIFI_PARTY_F_SILENCE;
	CHECK(hifi_party_encode(&k, &p, NULL, buf, sizeof(buf)) == 0);
	/* a silence packet stays one */
	p.flags = HIFI_PARTY_F_SILENCE;
	n = hifi_party_encode(&k, &p, NULL, buf, sizeof(buf));
	CHECK(n == HIFI_PARTY_HDR + HIFI_PARTY_TAG && hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(!hifi_party_audio_frames(&q, pl, plen, out));
	/* a tagged code that's wrong inside (a broken leader): decodes as a
	 * packet, refused as audio */
	p.flags = HIFI_PARTY_F_CODED;
	n = hifi_party_encode(&k, &p, f, buf, sizeof(buf));
	buf[HIFI_PARTY_HDR] ^= 0xff;
	buf[HIFI_PARTY_HDR + 1] ^= 0xff;
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(!hifi_party_audio_frames(&q, pl, plen, out) || memcmp(out, f, sizeof(f)) != 0);
}

static void music(const char *path) {
	FILE *fp = fopen(path, "rb");
	CHECK(fp != NULL);
	if (!fp) return;
	uint32_t f[N], back[N];
	uint8_t code[RAWB];
	uint64_t pcm = 0, sent = 0, pkts = 0, coded = 0;
	double secs = 0;
	while (fread(f, 4, N, fp) == N) {
		bool silent = true;
		for (int i = 0; i < N; i++) silent &= f[i] == 0;
		if (silent) continue; /* the leader sends F_SILENCE for these */
		struct timespec a, b;
		clock_gettime(CLOCK_MONOTONIC, &a);
		size_t c = hifi_party_code(f, N, code, RAWB - 1);
		clock_gettime(CLOCK_MONOTONIC, &b);
		secs += (double) (b.tv_sec - a.tv_sec) + (double) (b.tv_nsec - a.tv_nsec) * 1e-9;
		if (c) {
			bool ok = hifi_party_uncode(code, c, N, back) && memcmp(back, f, sizeof(f)) == 0;
			CHECK(ok);
			coded++;
		}
		pcm += RAWB;
		sent += c ? c : RAWB;
		pkts++;
	}
	fclose(fp);
	CHECK(pkts > 0);
	if (pkts)
		printf("music %s: %llu packets, payload %.1f%% of PCM, whole packet %.1f%%, %llu coded, %.1f us a packet to code (host, ASan)\n",
		       path, (unsigned long long) pkts, 100.0 * (double) sent / (double) pcm,
		       100.0 * (double) (sent + pkts * (HIFI_PARTY_HDR + HIFI_PARTY_TAG)) / (double) (pkts * HIFI_PARTY_MAX),
		       (unsigned long long) coded, 1e6 * secs / (double) pkts);
}

/* the lab's noise codes like music, round-trips, and never clips */
static void noise(void) {
	hifi_party_noise_t st = {0x12345678u, 0, 0};
	uint32_t f[HIFI_PARTY_FRAMES], back[HIFI_PARTY_FRAMES];
	uint8_t out[HIFI_PARTY_FRAMES * 4];
	uint64_t sent = 0, pcm = 0;
	int clips = 0, bad = 0;
	for (int p = 0; p < 1000; p++) {
		for (int i = 0; i < HIFI_PARTY_FRAMES; i++) {
			f[i] = hifi_party_noise(&st);
			int16_t l = (int16_t) (f[i] & 0xffff), r = (int16_t) (f[i] >> 16);
			clips += l == 32767 || l == -32768 || r == 32767 || r == -32768;
		}
		size_t n = hifi_party_code(f, HIFI_PARTY_FRAMES, out, sizeof(out));
		if (!n || !hifi_party_uncode(out, n, HIFI_PARTY_FRAMES, back) || memcmp(back, f, sizeof(f))) bad++;
		sent += n;
		pcm += sizeof(f);
	}
	double ratio = (double) sent / (double) pcm;
	CHECK(bad == 0);
	CHECK(clips == 0);
	CHECK(ratio > 0.55 && ratio < 0.70);
	printf("noise: payload %.1f%% of PCM\n", 100.0 * ratio);
}

int main(void) {
	round_trips();
	noise();
	hostile();
	packets();
	const char *m = getenv("HIFI_PARTY_MUSIC");
	if (m) {
		char *list = strdup(m), *save = NULL;
		for (char *t = strtok_r(list, ":", &save); t; t = strtok_r(NULL, ":", &save)) music(t);
		free(list);
	}
	printf("test_party_code: %d checks, %d failed\n", check_count, check_failures);
	return check_failures != 0;
}
