// libmad as the board builds it (firmware/third_party/libmad/src, patch 0029)
// on crafted MP3 frames and on a real file. The Makefile's `mad` target
// builds this twice with AddressSanitizer: against that source, and against
// a copy with Debian's md_size and length-check patches taken back out
// (stock 0.15.1b with the board's inline buffers), and checks that the
// crafted frames overflow the stock one and not the fixed one.
//
//   test_mad                    every case below; exit 1 on a failure
//   test_mad crafted MDB...     decode one crafted frame per MDB (see below)
//   test_mad pcm FILE           decode FILE; print frames, errors and a hash
//                               of the PCM (the Makefile compares the two
//                               builds' hashes: the fix changes no samples)
//   test_mad frame MDB > F      write one crafted frame (for the board's lab
//                               experiment "crafted", codec mp3)
//
// The crafted frame: MPEG-1 Layer III, 32 kHz, mono, free bitrate, 2871
// bytes (FRAME), all zeros after the header but for main_data_begin. A free
// bitrate frame can be as long as its sender likes, and 2871 bytes of main
// data plus the 511 a frame may take from the one before is more than the
// 2567-byte main_data buffer (MAD_BUFFER_MDLEN): stock libmad copies it in
// unchecked. Two such frames with main_data_begin 511 write past the end of
// struct mad_stream, which is on the heap on the board (codecs/mad.c
// mallocs it); the fixed libmad refuses the frame (MAD_ERROR_BADFRAMELEN or
// BADDATAPTR, both recoverable) and goes on.
// On the board only the lab's crafted experiment (lab_crafted.c, which
// decodes a whole buffer at once) can hand libmad a frame this big:
// squeezelite's mad.c reads at most 2048 bytes at a time (READBUF_SIZE). On
// the playback path the risk is the over-reads Debian's length-check fixes,
// which the fuzzer below covers.
//
// Built with -DMAD_FUZZ it is instead a libFuzzer target: any bytes, as an
// MP3 stream, through the decoder and the synth.
#include "global.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bit.h"
#include "fixed.h"
#include "frame.h"
#include "stream.h"
#include "synth.h"
#include "timer.h"

#define FRAME 2871
#define MAX_FRAMES 64

struct result {
	int ok, errors, unrecoverable;
	uint32_t hash;  // FNV-1a over the synthesized PCM
	enum mad_error last;
};

// decode len bytes as one stream, as codecs/mad.c does on the board: the
// stream, frame and synth on the heap, MAD_BUFFER_GUARD zeros after the data
static struct result decode(const unsigned char *data, size_t len) {
	struct result r = {0, 0, 0, 2166136261u, MAD_ERROR_NONE};
	unsigned char *buf = calloc(1, len + MAD_BUFFER_GUARD);
	struct mad_stream *s = malloc(sizeof(*s));
	struct mad_frame *f = malloc(sizeof(*f));
	struct mad_synth *sy = malloc(sizeof(*sy));
	if (!buf || !s || !f || !sy) abort();
	if (len) memcpy(buf, data, len);
	mad_stream_init(s);
	mad_frame_init(f);
	mad_synth_init(sy);
	mad_stream_buffer(s, buf, len + MAD_BUFFER_GUARD);
	for (int n = 0; n < 100000; n++) {
		if (mad_frame_decode(f, s) == 0) {
			r.ok++;
			mad_synth_frame(sy, f);
			for (unsigned c = 0; c < sy->pcm.channels; c++)
				for (unsigned i = 0; i < sy->pcm.length; i++) {
					uint32_t v = (uint32_t)sy->pcm.samples[c][i];
					for (int b = 0; b < 4; b++) r.hash = (r.hash ^ ((v >> (8 * b)) & 0xff)) * 16777619u;
				}
			continue;
		}
		// the end of the data: the guard's zeros are no frame
		if (s->error == MAD_ERROR_BUFLEN || s->this_frame >= buf + len) break;
		r.errors++;
		r.last = s->error;
		if (!MAD_RECOVERABLE(s->error)) {
			r.unrecoverable++;
			break;
		}
	}
	mad_synth_finish(sy);
	mad_frame_finish(f);
	mad_stream_finish(s);
	free(sy);
	free(f);
	free(s);
	free(buf);
	return r;
}

static void crafted_frame(unsigned char *f, unsigned mdb) {
	memset(f, 0, FRAME);
	f[0] = 0xFF, f[1] = 0xFB, f[2] = 0x08, f[3] = 0xC0;  // MPEG-1 L3, no CRC, free bitrate, 32 kHz, mono
	f[4] = (unsigned char)(mdb >> 1);  // main_data_begin: 9 bits
	f[5] = (unsigned char)((mdb & 1) << 7);
}

static struct result crafted(const unsigned *mdb, int n) {
	static unsigned char buf[MAX_FRAMES * FRAME];
	for (int i = 0; i < n; i++) crafted_frame(buf + (size_t)i * FRAME, mdb[i]);
	return decode(buf, (size_t)n * FRAME);
}

#ifdef MAD_FUZZ
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
	decode(data, len);
	return 0;
}
#else

static int fails;

static void check_crafted(const char *what, const unsigned *mdb, int n) {
	struct result r = crafted(mdb, n);
	// every crafted frame that takes main data from the one before is
	// refused, recoverably (a frame with main_data_begin 0 asks for nothing
	// it doesn't have, and may decode)
	int zero = 0;
	for (int i = 0; i < n; i++) zero += mdb[i] == 0;
	if (r.unrecoverable || r.ok > zero || r.ok + r.errors < n - 1) {
		fprintf(stderr, "test_mad: %s: %d decoded, %d errors (%d unrecoverable), last %s\n", what, r.ok,
			r.errors, r.unrecoverable, r.errors ? "set" : "none");
		fails++;
	}
}

static long read_file(const char *path, unsigned char **out) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	rewind(fp);
	*out = malloc(n > 0 ? (size_t)n : 1);
	if (!*out || fread(*out, 1, (size_t)n, fp) != (size_t)n) n = -1;
	fclose(fp);
	return n;
}

int main(int argc, char **argv) {
	if (argc > 1 && !strcmp(argv[1], "frame") && argc == 3) {
		unsigned char f[FRAME];
		crafted_frame(f, (unsigned)atoi(argv[2]) & 511);
		return fwrite(f, 1, FRAME, stdout) == FRAME ? 0 : 1;
	}
	if (argc > 2 && !strcmp(argv[1], "crafted") && argc - 2 <= MAX_FRAMES) {
		unsigned mdb[MAX_FRAMES];
		for (int i = 2; i < argc; i++) mdb[i - 2] = (unsigned)atoi(argv[i]) & 511;
		struct result r = crafted(mdb, argc - 2);
		printf("crafted: %d decoded, %d errors, %d unrecoverable\n", r.ok, r.errors, r.unrecoverable);
		return r.unrecoverable ? 1 : 0;
	}
	if (argc == 3 && !strcmp(argv[1], "pcm")) {
		unsigned char *data;
		long n = read_file(argv[2], &data);
		if (n < 0) {
			fprintf(stderr, "test_mad: can't read %s\n", argv[2]);
			return 1;
		}
		struct result r = decode(data, (size_t)n);
		free(data);
		printf("frames %d errors %d pcm %08x\n", r.ok, r.errors, r.hash);
		return r.unrecoverable ? 1 : 0;
	}
	if (argc != 1) {
		fprintf(stderr, "test_mad [crafted MDB... | pcm FILE | frame MDB]\n");
		return 64;
	}
	// two frames that each take the most a frame may from the one before:
	// the second overflows main_data in stock libmad
	check_crafted("two frames, main_data_begin 511", (const unsigned[]){511, 511}, 2);
	// a frame that takes 511 bytes from a frame that had main data of its own
	check_crafted("511 after 0", (const unsigned[]){0, 511, 0, 0, 0, 0}, 6);
	// 64 of them, as the board's lab experiment sends with repeat 64
	unsigned many[MAX_FRAMES];
	for (int i = 0; i < MAX_FRAMES; i++) many[i] = 511;
	check_crafted("64 frames, main_data_begin 511", many, MAX_FRAMES);
	// and every main_data_begin, two frames each
	for (unsigned m = 0; m < 512; m++) {
		struct result r = crafted((const unsigned[]){m, m}, 2);
		if (r.unrecoverable || r.ok) {
			fprintf(stderr, "test_mad: main_data_begin %u: %d decoded, %d unrecoverable\n", m, r.ok,
				r.unrecoverable);
			fails++;
			break;
		}
	}
	// truncated and empty input
	unsigned char f[FRAME];
	crafted_frame(f, 511);
	for (size_t n = 0; n <= 64; n++) decode(f, n);
	printf("mad          %s\n", fails ? "FAILED" : "crafted frames refused, no overflow");
	return fails ? 1 : 0;
}
#endif
