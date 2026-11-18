/* Party mode's core: the packet and its tag, the clock fit on late-only
 * samples, the jitter buffer with its NACKs and concealment, the follower's
 * controller under crystal drift, the gain and the leader's resend history. */
#include <math.h>
#include <stdlib.h>

#include "check.h"
#include "hifi_party.h"

static const uint8_t KEY[HIFI_LINK_KEY] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint8_t KEY2[HIFI_LINK_KEY] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};

static uint64_t rng = 88172645463325252ull;
static double urand(void) { /* (0, 1) */
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return ((double) (rng >> 11) + 0.5) / 9007199254740992.0;
}

/* signs a packet changed in place, as a board with the key would (the tag
 * construction of hifi_party.c) */
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
	hifi_link_key_t k, k2, none = {0};
	hifi_link_key_init(&k, KEY);
	hifi_link_key_init(&k2, KEY2);
	uint32_t frames[HIFI_PARTY_FRAMES];
	for (int i = 0; i < HIFI_PARTY_FRAMES; i++) frames[i] = 0x10000u * (uint32_t) i + (uint32_t) (1000 - i);
	hifi_party_pkt_t p = {.type = HIFI_PARTY_AUDIO, .group = 0xabcdef01, .seq = 77, .k = 352 * 1000, .rate = 44100, .n = HIFI_PARTY_FRAMES, .flags = 0};
	uint8_t buf[HIFI_PARTY_MAX + 16];
	size_t n = hifi_party_encode(&k, &p, frames, buf, sizeof(buf));
	CHECK(n == HIFI_PARTY_MAX && n == 1454);

	hifi_party_pkt_t q;
	const uint8_t *pl;
	size_t plen;
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(q.type == p.type && q.group == p.group && q.seq == p.seq && q.k == p.k && q.rate == p.rate && q.n == p.n);
	CHECK(plen == sizeof(frames) && !memcmp(pl, frames, plen));

	/* a changed byte anywhere, a wrong key, a cut packet: the tag fails */
	for (size_t at = 0; at < n; at += 97) {
		buf[at] ^= 0x40;
		int r = hifi_party_decode(&k, buf, n, &q, &pl, &plen);
		CHECK(r == (at < 3 ? HIFI_PARTY_E_MAGIC : HIFI_PARTY_E_TAG));
		buf[at] ^= 0x40;
	}
	CHECK(hifi_party_decode(&k2, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_TAG);
	CHECK(hifi_party_decode(&k, buf, n - 1, &q, &pl, &plen) == HIFI_PARTY_E_TAG);
	CHECK(hifi_party_decode(&k, buf, HIFI_PARTY_HDR + HIFI_PARTY_TAG - 1, &q, &pl, &plen) == HIFI_PARTY_E_SHORT);
	CHECK(hifi_party_decode(&k, buf, HIFI_PARTY_MAX + 1, &q, &pl, &plen) == HIFI_PARTY_E_SHORT);
	CHECK(hifi_party_decode(&none, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_KEY);
	CHECK(hifi_party_encode(&none, &p, frames, buf, sizeof(buf)) == 0);
	CHECK(hifi_party_encode(&k, &p, frames, buf, n - 1) == 0);

	/* fields that don't fit the type, even tagged right */
	hifi_party_pkt_t bad = p;
	bad.n = 0;
	CHECK(hifi_party_encode(&k, &bad, frames, buf, sizeof(buf)) == 0);
	bad.n = HIFI_PARTY_FRAMES + 1;
	CHECK(hifi_party_encode(&k, &bad, frames, buf, sizeof(buf)) == 0);
	bad = p;
	bad.k = 100; /* not on a packet boundary */
	n = hifi_party_encode(&k, &bad, frames, buf, sizeof(buf));
	CHECK(n && hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_E_FORMAT);
	bad.type = 9;
	CHECK(hifi_party_encode(&k, &bad, frames, buf, sizeof(buf)) == 0);

	/* CLOCK carries the BSSID; NACK the k values */
	uint8_t bssid[6] = {0xa0, 1, 2, 3, 4, 5};
	hifi_party_pkt_t c = {.type = HIFI_PARTY_CLOCK, .group = 5, .seq = 1, .k = 123456, .t = 987654321012ll, .rate = 44100123};
	n = hifi_party_encode(&k, &c, bssid, buf, sizeof(buf));
	CHECK(n == HIFI_PARTY_HDR + 6 + HIFI_PARTY_TAG);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(q.t == c.t && q.k == c.k && q.rate == c.rate && plen == 6 && !memcmp(pl, bssid, 6));
	uint8_t ks[3 * 8];
	int64_t want[3] = {352, 704 * 1000000ll, 0};
	for (int i = 0; i < 3; i++)
		for (int b = 0; b < 8; b++) ks[8 * i + b] = (uint8_t) ((uint64_t) want[i] >> (8 * b));
	hifi_party_pkt_t nk = {.type = HIFI_PARTY_NACK, .group = 5, .seq = 2, .n = 3};
	n = hifi_party_encode(&k, &nk, ks, buf, sizeof(buf));
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK && q.n == 3);
	for (int i = 0; i < 3; i++) CHECK(hifi_party_nack_k(pl, i) == want[i]);
	nk.n = HIFI_PARTY_NACK_MAX + 1;
	CHECK(hifi_party_encode(&k, &nk, ks, buf, sizeof(buf)) == 0);

	/* silence: n frames, no samples sent (46 bytes, not 1454), and no
	 * payload needed; samples with the flag don't decode */
	hifi_party_pkt_t sp = p;
	sp.flags = HIFI_PARTY_F_SILENCE;
	n = hifi_party_encode(&k, &sp, NULL, buf, sizeof(buf));
	CHECK(n == HIFI_PARTY_HDR + HIFI_PARTY_TAG && n == 46);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK);
	CHECK(q.n == HIFI_PARTY_FRAMES && (q.flags & HIFI_PARTY_F_SILENCE) && plen == 0);
	CHECK(hifi_party_encode(&k, &p, NULL, buf, sizeof(buf)) == 0); /* samples promised, none given */
	n = hifi_party_encode(&k, &p, frames, buf, sizeof(buf));
	buf[36] |= HIFI_PARTY_F_SILENCE; /* the flag set on a full packet: retagged, it still doesn't fit */
	retag(&k, buf, n);
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) != HIFI_PARTY_OK);
	/* a CLOCK with no audio */
	c.flags = HIFI_PARTY_F_NOAUDIO;
	n = hifi_party_encode(&k, &c, bssid, buf, sizeof(buf));
	CHECK(hifi_party_decode(&k, buf, n, &q, &pl, &plen) == HIFI_PARTY_OK && (q.flags & HIFI_PARTY_F_NOAUDIO));
}

/* A DAC whose crystal is ppm off, its writes returning late by an
 * exponential amount (mean_us), sometimes a whole late second: the fit's
 * line predicts the boundary's play moment to within a few µs. */
static void fit_dac(double ppm, double mean_us) {
	const int BUF = 256, BLOCK = 2048;
	const double us_per_frame = 1e6 / 44100.0 * (1 + ppm * 1e-6), t0 = 3e6;
	hifi_party_fit_t f;
	hifi_party_fit_init(&f, 1000000, 32, 4, 200);
	int64_t w = 0;
	int closed = 0;
	for (int i = 0; i < 2000; i++) {
		int64_t w0 = w, w1 = w + BLOCK, x;
		w = w1;
		int64_t b = 0;
		CHECK(hifi_party_dac_sample(w0, w1, 0, 1000, BUF, 0, &x));
		b = x;
		double free_at = t0 + (double) b * us_per_frame;
		double late = -mean_us * log(urand());
		bool busy_second = ((int64_t) free_at / 1000000) == 80; /* every write of second 80 is 3 ms late */
		if (busy_second) late += 3000;
		int64_t y = (int64_t) llround(free_at + late);
		double nominal = t0 + (double) b * 1e6 / 44100.0;
		closed += hifi_party_fit_add(&f, b, y, (double) y - nominal, y);
	}
	CHECK(closed >= 40);
	CHECK(f.ok && f.used >= 30);
	CHECK(f.rejected >= 1); /* the busy second */
	double xn = (double) w + 44100;  /* a second past the last write */
	double want = t0 + xn * us_per_frame;
	CHECK_NEAR(hifi_party_fit_y(&f, xn) - want, mean_us / 21.0, 8); /* the bias of a window's best of ~21: common to both boards */
	CHECK_NEAR((f.slope / (1e6 / 44100.0) - 1) * 1e6, ppm, 0.5);
	CHECK_NEAR(hifi_party_fit_x(&f, hifi_party_fit_y(&f, xn)), xn, 1e-3);
}

static void clock_fit(void) {
	fit_dac(0, 100);
	fit_dac(30, 100);
	fit_dac(-25, 50);

	/* too few windows: no line */
	hifi_party_fit_t f;
	hifi_party_fit_init(&f, 1000, 8, 4, 0);
	for (int i = 0; i < 3; i++) hifi_party_fit_add(&f, i, 10 * i, 0, 1000 * i);
	CHECK(!f.ok);
	for (int i = 3; i < 6; i++) hifi_party_fit_add(&f, i, 10 * i, 0, 1000 * i);
	CHECK(f.ok);
	CHECK_NEAR(hifi_party_fit_y(&f, 100), 1000, 1e-6);
	/* the best (lowest q) sample of each window is kept */
	hifi_party_fit_init(&f, 1000, 8, 2, 0);
	for (int w = 0; w < 4; w++) {
		hifi_party_fit_add(&f, w, 5 * w + 50, 50, 1000 * w);
		hifi_party_fit_add(&f, w, 5 * w, 0, 1000 * w + 10);
		hifi_party_fit_add(&f, w, 5 * w + 30, 30, 1000 * w + 20);
	}
	hifi_party_fit_add(&f, 9, 0, 0, 9000);
	CHECK(f.ok && f.n == 4);
	CHECK_NEAR(hifi_party_fit_y(&f, 8), 40, 1e-9);

	/* a TSF that steps back: with both_sides the stepped windows stay out of
	 * the line, which goes on through the others */
	for (int both = 0; both < 2; both++) {
		hifi_party_fit_init(&f, 1000, 30, 4, 200);
		f.both_sides = both;
		for (int i = 0; i < 20; i++) hifi_party_fit_add(&f, 1000 * i, 1000 * i + 5000000, 0, 1000 * i);
		for (int i = 20; i < 24; i++) hifi_party_fit_add(&f, 1000 * i, 1000 * i - 4000000, 0, 1000 * i);
		bool on_line = f.ok && fabs(hifi_party_fit_y(&f, 30000) - 5030000) < 1;
		CHECK(both ? on_line && f.rejected == 3 : !on_line);
	}

	/* the write samples */
	int64_t x = -1;
	CHECK(hifi_party_dac_sample(0, 2048, 0, 100, 256, 0, &x) && x == 1792);
	CHECK(hifi_party_dac_sample(100, 300, 0, 100, 256, 0, &x) && x == 256);
	CHECK(!hifi_party_dac_sample(257, 511, 0, 100, 256, 0, &x)); /* no boundary */
	CHECK(hifi_party_dac_sample(256, 257, 0, 100, 256, 0, &x) && x == 256);
	CHECK(!hifi_party_dac_sample(0, 2048, 0, 100, 256, 500, &x));  /* found room: didn't wait */
	CHECK(hifi_party_dac_sample(0, 2048, 0, 600, 256, 500, &x));
	CHECK(!hifi_party_dac_sample(10, 10, 0, 600, 256, 0, &x));

	/* the target: k_ref plays at t_ref, 44.1 frames per ms after */
	CHECK_NEAR(hifi_party_target(1000, 5000000, 44100000, 6000000), 45100, 1e-9);
	CHECK_NEAR(hifi_party_target(1000, 5000000, 44100000, 4999000), 955.9, 1e-9);
}

/* a frame that says which stream index it is */
static uint32_t frame_of(int64_t i) {
	return (uint32_t) (i % 30000 + 1) | (uint32_t) (uint16_t) (-(int16_t) (i % 30000 + 1)) << 16;
}
static int64_t index_of(uint32_t f) {
	return (int16_t) (uint16_t) f - 1;
}

static void put_packet(hifi_party_jb_t *jb, int64_t p, int64_t now) {
	uint32_t fr[HIFI_PARTY_FRAMES];
	for (int i = 0; i < HIFI_PARTY_FRAMES; i++) fr[i] = frame_of(p * HIFI_PARTY_FRAMES + i);
	hifi_party_jb_put(jb, p * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, now);
}

static void jitter_buffer(void) {
	const int NS = 8;
	void *mem = aligned_alloc(8, (hifi_party_jb_bytes(NS) + 7) & ~(size_t) 7);
	hifi_party_jb_t jb;
	hifi_party_jb_init(&jb, mem, NS);
	uint32_t fr[HIFI_PARTY_FRAMES] = {0}, out[2 * HIFI_PARTY_FRAMES];
	int64_t ks[16];

	CHECK(hifi_party_jb_put(&jb, 100, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_BAD);
	CHECK(hifi_party_jb_put(&jb, 0, fr, 10, 0) == HIFI_PARTY_PUT_BAD);
	put_packet(&jb, 10, 0);
	CHECK(jb.floor == 10 && jb.top == 10 && jb.got == 1);
	CHECK(hifi_party_jb_put(&jb, 10 * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_DUP);
	CHECK(hifi_party_jb_put(&jb, 9 * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OLD);

	/* 11 and 12 lost: asked for at 20 ms, again at 60 ms, then no more */
	put_packet(&jb, 13, 1000);
	CHECK(jb.gaps == 2);
	CHECK(hifi_party_jb_nacks(&jb, 1000 + HIFI_PARTY_NACK1_US - 1, ks, 16) == 0);
	CHECK(hifi_party_jb_nacks(&jb, 1000 + HIFI_PARTY_NACK1_US, ks, 16) == 2);
	CHECK(ks[0] == 11 * HIFI_PARTY_FRAMES && ks[1] == 12 * HIFI_PARTY_FRAMES);
	CHECK(hifi_party_jb_nacks(&jb, 1000 + HIFI_PARTY_NACK1_US + 5000, ks, 16) == 0);
	put_packet(&jb, 11, 30000); /* the resend */
	CHECK(hifi_party_jb_nacks(&jb, 1000 + HIFI_PARTY_NACK2_US, ks, 16) == 1 && ks[0] == 12 * HIFI_PARTY_FRAMES);
	CHECK(hifi_party_jb_nacks(&jb, 1000000, ks, 16) == 0);
	CHECK(jb.nacked == 3);

	/* reading: 10 and 11 as sent */
	CHECK(hifi_party_jb_read(&jb, 10 * HIFI_PARTY_FRAMES, out, 2 * HIFI_PARTY_FRAMES) == 0);
	bool same = true;
	for (int i = 0; i < 2 * HIFI_PARTY_FRAMES; i++) same &= index_of(out[i]) == 10 * HIFI_PARTY_FRAMES + i;
	CHECK(same);
	/* 12 is missing: the last 220 frames again, fading, then silence */
	CHECK(hifi_party_jb_read(&jb, 12 * HIFI_PARTY_FRAMES, out, HIFI_PARTY_FRAMES) == HIFI_PARTY_FRAMES);
	CHECK(index_of(out[0]) == 12 * HIFI_PARTY_FRAMES - HIFI_PARTY_CONCEAL);
	int16_t l100 = (int16_t) (uint16_t) out[100], want100 = (int16_t) ((12 * HIFI_PARTY_FRAMES - HIFI_PARTY_CONCEAL + 100 + 1) * (HIFI_PARTY_CONCEAL - 100) / HIFI_PARTY_CONCEAL);
	CHECK(l100 == want100);
	CHECK(out[HIFI_PARTY_CONCEAL] == 0 && out[HIFI_PARTY_FRAMES - 1] == 0);
	CHECK(jb.missing_frames == HIFI_PARTY_FRAMES);
	/* 13 is there: it fades back in */
	CHECK(hifi_party_jb_read(&jb, 13 * HIFI_PARTY_FRAMES, out, HIFI_PARTY_FRAMES) == 0);
	int16_t l0 = (int16_t) (uint16_t) out[0];
	CHECK(l0 == (int16_t) ((13 * HIFI_PARTY_FRAMES + 1) / (HIFI_PARTY_FADE_IN + 1)));
	CHECK(index_of(out[HIFI_PARTY_FADE_IN]) == 13 * HIFI_PARTY_FRAMES + HIFI_PARTY_FADE_IN);
	/* reading let go of what is behind: 11 is old now */
	CHECK(jb.floor == 12);
	CHECK(hifi_party_jb_put(&jb, 11 * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OLD);
	/* far ahead: the window moves */
	put_packet(&jb, 40, 2000000);
	CHECK(jb.floor == 40 - NS + 1 && jb.top == 40 && jb.skipped == (uint32_t) (40 - NS + 1 - 12));
	/* the gap the jump left isn't asked for beyond the window */
	int c = hifi_party_jb_nacks(&jb, 3000000, ks, 16);
	CHECK(c == NS - 1);
	for (int i = 0; i < c; i++) CHECK(ks[i] >= jb.floor * HIFI_PARTY_FRAMES);
	/* a jump back: reading at 36 lowers the floor so 35 and 36, late or
	 * resent, are taken (not old); never below a buffer under the top */
	CHECK(hifi_party_jb_read(&jb, 36 * HIFI_PARTY_FRAMES, out, HIFI_PARTY_FRAMES) == HIFI_PARTY_FRAMES);
	CHECK(jb.floor == 35);
	CHECK(hifi_party_jb_put(&jb, 36 * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OK);
	CHECK(hifi_party_jb_put(&jb, 35 * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OK);
	CHECK(hifi_party_jb_read(&jb, 2 * HIFI_PARTY_FRAMES, out, HIFI_PARTY_FRAMES) == HIFI_PARTY_FRAMES);
	CHECK(jb.floor == 40 - NS + 1);
	CHECK(hifi_party_jb_put(&jb, 3 * HIFI_PARTY_FRAMES, fr, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OLD);

	/* a silence packet is held as zeros, where samples were before */
	put_packet(&jb, 41, 0);
	CHECK(hifi_party_jb_put(&jb, 42 * HIFI_PARTY_FRAMES, NULL, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OK);
	CHECK(hifi_party_jb_put(&jb, 42 * HIFI_PARTY_FRAMES, NULL, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_DUP);
	put_packet(&jb, 41 + NS, 0); /* reuses 41's slot */
	CHECK(hifi_party_jb_put(&jb, (41 + 2 * NS) * HIFI_PARTY_FRAMES, NULL, HIFI_PARTY_FRAMES, 0) == HIFI_PARTY_PUT_OK);
	CHECK(hifi_party_jb_read(&jb, (41 + 2 * NS) * HIFI_PARTY_FRAMES, out, HIFI_PARTY_FRAMES) == 0);
	int nz = 0;
	for (int i = 0; i < HIFI_PARTY_FRAMES; i++) nz += out[i] != 0;
	CHECK(nz == 0);
	free(mem);
}

/* The follower's crystal runs ppm off the leader's: each block of 2048
 * frames the follower writes covers 2048 * (1 + ppm) of the leader's. The
 * controller keeps it within two frames, slips at most once per block, and
 * plays the stream in order but for the slipped frame. */
static void controller(double ppm) {
	const int NS = 64, N = 2048;
	void *mem = aligned_alloc(8, (hifi_party_jb_bytes(NS) + 7) & ~(size_t) 7);
	hifi_party_jb_t jb;
	hifi_party_jb_init(&jb, mem, NS);
	hifi_party_ctl_t c;
	hifi_party_ctl_init(&c, 44100);
	uint32_t out[2048];
	double target = 44100.3;
	int64_t sent = (int64_t) (target / HIFI_PARTY_FRAMES);
	bool ok = true, inorder = true, oneslip = true;
	uint32_t slips = 0, prev = 0;
	for (int b = 0; b < 3000; b++) {
		while (sent * HIFI_PARTY_FRAMES < target + N + 13230) put_packet(&jb, sent++, b);
		uint32_t before = c.drops + c.repeats;
		hifi_party_render(&c, &jb, target, out, N, 65536, 65536);
		uint32_t s = c.drops + c.repeats - before;
		oneslip &= s <= 1;
		slips += s;
		/* the stream runs on across blocks but for the slipped frame */
		int disc = 0;
		for (int i = b == 0; i < N; i++) {
			int64_t d = ((index_of(out[i]) - index_of(i ? out[i - 1] : prev)) % 30000 + 30000) % 30000; /* frame_of wraps */
			disc += d != 1;
		}
		prev = out[N - 1];
		inorder &= disc == (int) s;
		target += N * (1 + ppm * 1e-6);
		if (b > 0) ok &= fabs((double) c.next - target) <= 2;
	}
	CHECK(ok);
	CHECK(inorder);
	CHECK(oneslip);
	CHECK(c.jumps == 0);
	double want = fabs(ppm) * 1e-6 * N * 3000;
	CHECK(fabs(slips - want) <= 3);
	CHECK(ppm >= 0 ? c.repeats == 0 : c.drops == 0);
	CHECK(jb.missing_frames == 0);

	/* the clock moves 10 ms (the leader's restarted): one jump */
	target += 441;
	hifi_party_render(&c, &jb, target, out, N, 65536, 65536);
	CHECK(c.jumps == 1 && c.next == llround(target) + N);

	/* a mute, short enough that what played before is still in the buffer:
	 * the mute block fades out from what played last, then silence, and
	 * isn't counted missing; the next render starts afresh (no jump, no
	 * crossfade from the stretch before the mute) and fades in */
	uint32_t missing = jb.missing_frames, jumps = c.jumps;
	hifi_party_mute(&c, &jb, out, N, 65536, 65536);
	CHECK(!c.started && jb.missing_frames == missing);
	bool falls = true;
	for (int i = 0; i < N; i++) falls &= abs((int16_t) (uint16_t) out[i]) <= 30000 * (HIFI_PARTY_CONCEAL - (i < HIFI_PARTY_CONCEAL ? i : HIFI_PARTY_CONCEAL)) / HIFI_PARTY_CONCEAL + 1;
	CHECK(falls);
	target += 0.3 * 44100;
	while (sent * HIFI_PARTY_FRAMES < target + N + 13230) put_packet(&jb, sent++, 0); /* the stream ran on meanwhile */
	hifi_party_render(&c, &jb, target, out, N, 65536, 65536);
	CHECK(c.jumps == jumps && c.next == llround(target) + N);
	bool rises = true;
	for (int i = 0; i < HIFI_PARTY_FADE_IN; i++) rises &= abs((int16_t) (uint16_t) out[i]) <= 30000 * (i + 1) / HIFI_PARTY_FADE_IN + 1;
	CHECK(rises);
	CHECK(index_of(out[N - 1]) == (llround(target) + N - 1) % 30000);
	free(mem);
}

static void gain_and_hist(void) {
	uint32_t f[4] = {0x7fff | 0x8000u << 16, 1000 | (uint32_t) (uint16_t) -1000 << 16, 0, 0xffff};
	hifi_party_gain(f, 4, 2 * 65536, 32768);
	CHECK((int16_t) (uint16_t) f[0] == 32767 && (int16_t) (f[0] >> 16) == -16384);
	CHECK((int16_t) (uint16_t) f[1] == 2000 && (int16_t) (f[1] >> 16) == -500);
	CHECK(f[2] == 0);
	hifi_party_gain(f, 1, 65536 * 4, 65536 * 4);
	CHECK((int16_t) (uint16_t) f[0] == 32767 && (int16_t) (f[0] >> 16) == -32768);
	uint32_t u = f[1];
	hifi_party_gain(f + 1, 1, 65536, 65536);
	CHECK(f[1] == u);

	const int NS = 4;
	void *mem = aligned_alloc(8, (hifi_party_hist_bytes(NS) + 7) & ~(size_t) 7);
	hifi_party_hist_t h;
	hifi_party_hist_init(&h, mem, NS);
	uint8_t pkt[HIFI_PARTY_MAX];
	size_t len = 0;
	CHECK(!hifi_party_hist_get(&h, 0, &len));
	for (int p = 0; p < 6; p++) {
		memset(pkt, p, sizeof(pkt));
		hifi_party_hist_put(&h, (int64_t) p * HIFI_PARTY_FRAMES, pkt, 100 + p);
	}
	CHECK(!hifi_party_hist_get(&h, 1 * HIFI_PARTY_FRAMES, &len)); /* overwritten */
	const uint8_t *g = hifi_party_hist_get(&h, 5 * HIFI_PARTY_FRAMES, &len);
	CHECK(g && len == 105 && g[0] == 5 && g[104] == 5);
	g = hifi_party_hist_get(&h, 2 * HIFI_PARTY_FRAMES, &len);
	CHECK(g && len == 102 && g[0] == 2);
	CHECK(!hifi_party_hist_get(&h, 7, &len));
	free(mem);
}

static void bursts(void) {
	hifi_party_bursts_t r = {0};
	CHECK(!hifi_party_bursts_get(&r, 0));
	hifi_party_bursts_add(&r, 1000, 1, 12);
	hifi_party_bursts_add(&r, 1100, 1, 0);
	hifi_party_bursts_add(&r, 1100 + HIFI_PARTY_BURST_GAP_MS, 2, 11); /* just within the gap: the same burst */
	const hifi_party_burst_t *b = hifi_party_bursts_get(&r, 0);
	CHECK(r.n == 1 && r.total == 1);
	CHECK(b && b->at_ms == 1000 && b->dur_ms == 100 + HIFI_PARTY_BURST_GAP_MS && b->count == 4 && b->code == 11);
	hifi_party_bursts_add(&r, 1351 + HIFI_PARTY_BURST_GAP_MS, 5, 0); /* past it: a new one */
	CHECK(r.n == 2 && r.total == 2);
	b = hifi_party_bursts_get(&r, 0);
	CHECK(b && b->at_ms == 1351 + HIFI_PARTY_BURST_GAP_MS && b->dur_ms == 0 && b->count == 5 && b->code == 0);
	b = hifi_party_bursts_get(&r, 1);
	CHECK(b && b->at_ms == 1000 && b->count == 4);
	CHECK(!hifi_party_bursts_get(&r, 2) && !hifi_party_bursts_get(&r, -1));
	/* the ring keeps the newest, newest first */
	for (uint32_t i = 0; i < 2 * HIFI_PARTY_BURSTS; i++) hifi_party_bursts_add(&r, 10000 + i * 1000, i + 1, 0);
	CHECK(r.n == HIFI_PARTY_BURSTS && r.total == 2 + 2 * HIFI_PARTY_BURSTS);
	b = hifi_party_bursts_get(&r, 0);
	CHECK(b && b->count == 2 * HIFI_PARTY_BURSTS);
	b = hifi_party_bursts_get(&r, HIFI_PARTY_BURSTS - 1);
	CHECK(b && b->count == HIFI_PARTY_BURSTS + 1);
	/* a clock that went back (a reset log) starts a new burst, not a huge one */
	hifi_party_bursts_add(&r, 500, 1, 0);
	b = hifi_party_bursts_get(&r, 0);
	CHECK(b && b->at_ms == 500 && b->dur_ms == 0 && b->count == 1);
	/* the millisecond clock wrapping inside a burst */
	hifi_party_bursts_t w = {0};
	hifi_party_bursts_add(&w, 0xFFFFFFF0u, 1, 0);
	hifi_party_bursts_add(&w, 0x10, 1, 0);
	b = hifi_party_bursts_get(&w, 0);
	CHECK(w.n == 1 && b && b->dur_ms == 0x20 && b->count == 2);
}

int main(void) {
	packets();
	clock_fit();
	jitter_buffer();
	controller(0);
	controller(30);
	controller(-30);
	controller(250);
	gain_and_hist();
	bursts();
	return check_report("party");
}
