// Regression test for patch 0021 (ALAC bounds): upstream's ALAC decoder as
// the board builds it (cspot/bell/external/alac with our fixes, plus
// codecs/alac_wrapper.cpp), under AddressSanitizer.
//
// - Crafted AirPlay packets whose partial-frame numSamples is larger than the
//   cookie's frameLength (the heap overflow in security.md): each must come
//   back as one packet of silence, with no write past the decoder's buffers.
// - Cookies that would size the buffers wrong are refused.
// - Real audio still decodes bit-exact: Apple's own encoder (same tree, not
//   patched) encodes a test signal, and the patched decoder must give it back.
//   Each packet sits in a buffer of exactly its length, and the wrapper
//   poisons its own copy past the packet and its zero pad under ASan, so a
//   read past the packet is caught too.
//
// Built with -DALAC_FUZZ it is a libFuzzer harness instead: the first byte
// picks a cookie (or the next 24 bytes are the cookie), the rest is a packet.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ALACEncoder.h"
#include "ALACAudioTypes.h"
#include "alac_wrapper.h"

// ALACSpecificConfig as rtp.c builds it from the RTSP fmtp line, big-endian
struct cookie_s {
	uint32_t frameLength;
	uint8_t compatibleVersion, bitDepth, pb, mb, kb, numChannels;
	uint16_t maxRun;
	uint32_t maxFrameBytes, avgBitRate, sampleRate;
} __attribute__((packed));

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void make_cookie(uint8_t c[24], uint32_t frames, uint8_t bits, uint8_t ch) {
	memset(c, 0, 24);
	be32(c, frames);
	c[5] = bits; c[6] = 40; c[7] = 10; c[8] = 14; c[9] = ch;	// pb mb kb as AirPlay sends
	c[10] = 0; c[11] = 255;
	be32(c + 20, 44100);
}

#ifndef ALAC_FUZZ

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// MSB-first bit writer for crafting packets
struct bw { uint8_t *b; size_t cap, bit; };
static void put(struct bw *w, uint32_t v, int n) {
	for (int i = n - 1; i >= 0; i--, w->bit++) {
		if (w->bit / 8 >= w->cap) return;
		if ((v >> i) & 1) w->b[w->bit / 8] |= 0x80 >> (w->bit % 8);
	}
}

static bool all_zero(const uint8_t *p, size_t n) { while (n--) if (*p++) return false; return true; }

// decode one packet held in a heap buffer of exactly len bytes
static bool decode(struct alac_codec_s *d, const uint8_t *pkt, size_t len, uint8_t *out, unsigned outlen,
				   unsigned *frames, char ch = 2) {
	uint8_t *exact = (uint8_t *) malloc(len ? len : 1);
	memcpy(exact, pkt, len);
	memset(out, 0x5a, outlen);
	bool ok = alac_to_pcm_len(d, exact, (unsigned) len, out, ch, frames);
	free(exact);
	return ok;
}

// the AirPlay decoder: 352 frames, 16 bits, 2 channels (rtp.c's fmtp)
static struct alac_codec_s *airplay(unsigned *block) {
	uint8_t c[24]; unsigned char size, chans; unsigned rate;
	make_cookie(c, 352, 16, 2);
	return alac_create_decoder(24, c, &size, &rate, &chans, block);
}

static void test_crafted_partial_frames(void) {
	unsigned block = 0, frames = 0;
	struct alac_codec_s *d = airplay(&block);
	CHECK(d && block == 352 * 4);
	if (!d) return;
	uint8_t *out = (uint8_t *) malloc(block);

	// 1. stereo pair, uncompressed (escape), partial frame of 65536 samples:
	//    upstream writes the samples straight into 352-sample mix buffers
	uint8_t pkt[1600] = { 0 };	// room for a full 352-frame escape packet (1416 bytes)
	struct bw w = { pkt, sizeof pkt, 0 };
	put(&w, 1, 3); put(&w, 0, 4); put(&w, 0, 12); put(&w, 0x9, 4);	// CPE, tag 0, partial + escape
	put(&w, 0x10000, 32);
	for (int i = 0; w.bit / 8 < sizeof pkt - 4; i++) put(&w, 0x4141, 16);
	CHECK(!decode(d, pkt, sizeof pkt, out, block, &frames));
	CHECK(frames == 352 && all_zero(out, block));

	// 2. the same, compressed: dyn_decomp would run 4096 residuals into the predictor
	memset(pkt, 0, sizeof pkt); w = (struct bw){ pkt, sizeof pkt, 0 };
	put(&w, 1, 3); put(&w, 0, 4); put(&w, 0, 12); put(&w, 0x8, 4);	// CPE, partial, compressed
	put(&w, 4096, 32);
	put(&w, 0, 8); put(&w, 0, 8);			// mixBits, mixRes
	put(&w, 0x00, 8); put(&w, 0x80, 8);		// U: mode 0 denshift 0, pbFactor 4, no coefs
	put(&w, 0x00, 8); put(&w, 0x80, 8);		// V
	CHECK(!decode(d, pkt, sizeof pkt, out, block, &frames));
	CHECK(frames == 352 && all_zero(out, block));

	// 3. a mono element with a partial frame of 353, one past frameLength
	memset(pkt, 0, sizeof pkt); w = (struct bw){ pkt, sizeof pkt, 0 };
	put(&w, 0, 3); put(&w, 0, 4); put(&w, 0, 12); put(&w, 0x9, 4);	// SCE, partial + escape
	put(&w, 353, 32);
	CHECK(!decode(d, pkt, sizeof pkt, out, block, &frames));
	CHECK(frames == 352 && all_zero(out, block));

	// 4. exactly frameLength is still a valid partial frame (escape, a ramp)
	memset(pkt, 0, sizeof pkt); w = (struct bw){ pkt, sizeof pkt, 0 };
	put(&w, 1, 3); put(&w, 0, 4); put(&w, 0, 12); put(&w, 0x9, 4);
	put(&w, 352, 32);
	for (int i = 0; i < 352; i++) { put(&w, (uint16_t) (i * 90), 16); put(&w, (uint16_t) -(i * 90), 16); }
	put(&w, 7, 3);							// ID_END
	size_t len = (w.bit + 7) / 8;
	CHECK(decode(d, pkt, len, out, block, &frames));
	CHECK(frames == 352);
	int16_t *s = (int16_t *) out;
	CHECK(s[0] == 0 && s[2] == 90 && s[3] == -90 && s[702] == (int16_t) (351 * 90));

	// 5. the same packet cut short reads zeros past its end, not the heap
	for (size_t cut = 0; cut < len; cut += 37) decode(d, pkt, cut, out, block, &frames);

	// 6. more output channels than the buffer holds: silence, not an overflow
	CHECK(!decode(d, pkt, len, out, block, &frames, 3));
	CHECK(all_zero(out, block));

	// 7. no codec, no crash
	CHECK(!alac_to_pcm_len(NULL, pkt, (unsigned) len, out, 2, &frames) && frames == 0);

	free(out);
	alac_delete_decoder(d);
}

static void test_cookies(void) {
	uint8_t c[24]; unsigned char size, chans; unsigned rate, block;
	struct { uint32_t frames; uint8_t bits, ch; bool ok; } cases[] = {
		{ 352, 16, 2, true }, { 4096, 16, 2, true }, { 4096, 24, 2, true }, { 4096, 16, 1, true },
		{ 0, 16, 2, false }, { 31, 16, 2, false }, { 16385, 16, 2, false }, { 0x80000000u, 16, 2, false },
		{ 352, 0, 2, false }, { 352, 17, 2, false }, { 352, 64, 2, false },
		{ 352, 16, 0, false }, { 352, 16, 9, false },
	};
	for (auto &t : cases) {
		make_cookie(c, t.frames, t.bits, t.ch);
		struct alac_codec_s *d = alac_create_decoder(24, c, &size, &rate, &chans, &block);
		if (!d != !t.ok) printf("FAIL cookie frames %u bits %u ch %u: %s\n", t.frames, t.bits, t.ch,
								d ? "accepted" : "refused");
		failures += !d != !t.ok;
		alac_delete_decoder(d);
	}
	make_cookie(c, 352, 16, 2);
	CHECK(alac_create_decoder(23, c, &size, &rate, &chans, &block) == NULL);
	CHECK(alac_create_decoder(24, NULL, &size, &rate, &chans, &block) == NULL);
}

// Apple's encoder makes real packets; the patched decoder must return the input exactly.
// noise: full-scale random samples, which the encoder can't compress, so every
// packet takes the escape (uncompressed) path.
static void roundtrip(uint32_t frames, int bits, int ch, int packets, bool noise = false) {
	ALACEncoder enc;
	enc.SetFrameSize(frames);
	AudioFormatDescription in = {}, out = {};
	int bps = bits / 8;
	in.mSampleRate = 44100; in.mFormatID = kALACFormatLinearPCM;
	in.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked;
	in.mFramesPerPacket = 1; in.mBytesPerFrame = in.mBytesPerPacket = bps * ch;
	in.mChannelsPerFrame = ch; in.mBitsPerChannel = bits;
	out = in; out.mFormatID = kALACFormatAppleLossless; out.mFramesPerPacket = frames;
	out.mFormatFlags = bits == 16 ? 1 : bits == 24 ? 3 : 4; out.mBytesPerFrame = out.mBytesPerPacket = 0;
	CHECK(enc.InitializeEncoder(out) == 0);

	uint8_t cookie[64]; uint32_t csize = sizeof cookie;
	enc.GetMagicCookie(cookie, &csize);
	unsigned char size, chans; unsigned rate, block;
	struct alac_codec_s *d = alac_create_decoder((int) csize, cookie, &size, &rate, &chans, &block);
	CHECK(d && size == bits && chans == ch && rate == 44100);
	if (!d) return;

	size_t pcm_len = (size_t) frames * bps * ch;
	uint8_t *pcm = (uint8_t *) malloc(pcm_len), *pkt = (uint8_t *) malloc(pcm_len + 64), *dec = (uint8_t *) malloc(block);
	uint32_t seed = 1;
	for (int p = 0; p < packets; p++) {
		for (uint32_t f = 0; f < frames; f++)
			for (int c = 0; c < ch; c++) {
				seed = seed * 1103515245u + 12345u;
				double t = (p * frames + f) / 44100.0;
				// a tone, a quieter one, noise, and a silent stretch for the run-length path
				int32_t v = p == 2 ? 0 : (int32_t) (0.4 * sin(2 * M_PI * (440 + 220 * c) * t) * ldexp(1.0, bits - 1))
					+ (int32_t) ((seed >> 16) & 0xff) - 128;
				if (noise) v = (int32_t) (seed ^ (seed << 13) ^ (seed >> 7));
				for (int b = 0; b < bps; b++) pcm[(f * ch + c) * bps + b] = (uint8_t) (v >> (8 * b));
			}
		int32_t n = (int32_t) pcm_len;
		CHECK(enc.Encode(in, out, pcm, pkt, &n) == 0);
		unsigned got = 0;
		bool ok = decode(d, pkt, (size_t) n, dec, block, &got, (char) (ch < 2 ? 2 : ch));
		CHECK(ok && got == frames);
		// the decoder writes 2-channel frames even for mono (squeezelite asks for 2)
		bool same = true;
		for (uint32_t f = 0; ok && f < frames && same; f++)
			for (int c = 0; c < (ch < 2 ? 2 : ch); c++)
				same &= !memcmp(dec + (f * (ch < 2 ? 2 : ch) + c) * bps, pcm + (f * ch + (ch < 2 ? 0 : c)) * bps, bps);
		if (ch >= 2) CHECK(same);
		if (!ok || got != frames || (ch >= 2 && !same))
			printf("  roundtrip %u frames %d bits %d ch%s, packet %d (%d bytes)\n", frames, bits, ch,
				   noise ? " noise" : "", p, n);
	}
	free(pcm); free(pkt); free(dec);
	alac_delete_decoder(d);
}

int main(void) {
	test_crafted_partial_frames();
	test_cookies();
	roundtrip(352, 16, 2, 6);		// AirPlay
	roundtrip(4096, 16, 2, 4);		// an ALAC file through squeezelite
	roundtrip(4096, 24, 2, 4);
	roundtrip(4096, 32, 2, 4);		// 32-bit stereo: chanBits 33 when nothing is shifted
	roundtrip(4096, 16, 1, 4);		// mono
	roundtrip(352, 16, 2, 3, true);	// escape packets
	roundtrip(4096, 24, 2, 3, true);
	roundtrip(4096, 32, 2, 3, true);
	printf("%s: test_up_alac (%d failures)\n", failures ? "FAIL" : "ok", failures);
	return failures != 0;
}

#else	// ALAC_FUZZ

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	static const struct { uint32_t frames; uint8_t bits, ch; } cfg[] = {
		{ 352, 16, 2 }, { 4096, 16, 2 }, { 4096, 24, 2 }, { 4096, 16, 1 }, { 4096, 20, 2 }, { 1024, 32, 2 }, { 4096, 16, 6 },
	};
	if (size < 1) return 0;
	uint8_t c[24];
	if ((data[0] & 0x80) && size >= 25) {
		memcpy(c, data + 1, 24);
		data += 25; size -= 25;
	} else {
		unsigned k = data[0] % (sizeof cfg / sizeof cfg[0]);
		make_cookie(c, cfg[k].frames, cfg[k].bits, cfg[k].ch);
		data++; size--;
	}
	unsigned char bits, ch; unsigned rate, block;
	struct alac_codec_s *d = alac_create_decoder(24, c, &bits, &rate, &ch, &block);
	if (!d) return 0;
	uint8_t *out = (uint8_t *) malloc(block);
	uint8_t *in = (uint8_t *) malloc(size ? size : 1);	// exactly the packet, so overreads show
	memcpy(in, data, size);
	unsigned frames;
	alac_to_pcm_len(d, in, (unsigned) size, out, 2, &frames);
	if (ch > 2) alac_to_pcm_len(d, in, (unsigned) size, out, (char) ch, &frames);
	free(in); free(out);
	alac_delete_decoder(d);
	return 0;
}

#endif
