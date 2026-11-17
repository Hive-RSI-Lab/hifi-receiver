// The structs the codecs share with libmad, laid out as the codecs see them
// (components/codecs/inc/mad/mad.h, -DPUBLIC) or as libmad itself is built
// (third_party/libmad/src's headers, as patched). The upstream target builds
// both and requires the same output: codecs/mad.c mallocs these structs by
// mad.h's sizes and reads their fields by mad.h's offsets, and libmad writes
// them by its own, so any difference corrupts the heap on the board.
#include <stddef.h>
#include <stdio.h>
#ifdef PUBLIC
#include "mad.h"
#else
#include "global.h"
#include "bit.h"
#include "fixed.h"
#include "frame.h"
#include "stream.h"
#include "synth.h"
#include "timer.h"
#endif

#define S(s) printf("sizeof %s %zu\n", #s, sizeof(struct s));
#define F(s, m) printf("%s.%s %zu %zu\n", #s, #m, offsetof(struct s, m), sizeof(((struct s *)0)->m));

int main(void) {
	printf("sizeof mad_fixed_t %zu\nsizeof mad_timer_t %zu\nMAD_BUFFER_MDLEN %d\nMAD_BUFFER_GUARD %d\n",
	       sizeof(mad_fixed_t), sizeof(mad_timer_t), (int)MAD_BUFFER_MDLEN, (int)MAD_BUFFER_GUARD);
	S(mad_bitptr) F(mad_bitptr, byte) F(mad_bitptr, cache) F(mad_bitptr, left)
	S(mad_stream) F(mad_stream, buffer) F(mad_stream, bufend) F(mad_stream, skiplen) F(mad_stream, sync)
	F(mad_stream, freerate) F(mad_stream, this_frame) F(mad_stream, next_frame) F(mad_stream, ptr)
	F(mad_stream, anc_ptr) F(mad_stream, anc_bitlen) F(mad_stream, main_data) F(mad_stream, md_len)
	F(mad_stream, options) F(mad_stream, error)
	S(mad_header) F(mad_header, layer) F(mad_header, mode) F(mad_header, mode_extension) F(mad_header, emphasis)
	F(mad_header, bitrate) F(mad_header, samplerate) F(mad_header, crc_check) F(mad_header, crc_target)
	F(mad_header, flags) F(mad_header, private_bits) F(mad_header, duration)
	S(mad_frame) F(mad_frame, header) F(mad_frame, options) F(mad_frame, sbsample) F(mad_frame, overlap)
	F(mad_frame, xr) F(mad_frame, tmp)
	S(mad_pcm) F(mad_pcm, samplerate) F(mad_pcm, channels) F(mad_pcm, length) F(mad_pcm, samples)
	S(mad_synth) F(mad_synth, filter) F(mad_synth, phase) F(mad_synth, pcm)
	printf("MAD_ERROR_BADFRAMELEN 0x%x MAD_ERROR_BADDATAPTR 0x%x MAD_ERROR_LOSTSYNC 0x%x MAD_FLAG_INCOMPLETE 0x%x\n",
	       MAD_ERROR_BADFRAMELEN, MAD_ERROR_BADDATAPTR, MAD_ERROR_LOSTSYNC, MAD_FLAG_INCOMPLETE);
	return 0;
}
