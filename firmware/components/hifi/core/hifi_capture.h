/*
 * The board records its own outputs (docs/remote.md, "The board measures its
 * own output"): the codec's ADC on the loopback taps or the RCA input, as
 * 16-bit stereo. The board code fills the samples; this turns them into the
 * API's reply.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HIFI_CAPTURE_FRAMES_MAX 16384
#define HIFI_CAPTURE_FRAMES_DEFAULT 4096

typedef enum { HIFI_CAP_LOOPBACK = 0, HIFI_CAP_LINE } hifi_cap_src_t;

typedef struct {
	int rate;
	const char *ch[2];  /* "spk_l+", "spk_r+" or "line_l", "line_r" */
	float v_per_fs[2];  /* peak volts at the tap's source for an ADC full scale; NaN if unknown */
} hifi_cap_info_t;

typedef struct {
	float rms_fs, peak_fs, dc_fs; /* relative to full scale */
	int clipped;                  /* samples at the ADC's limits */
} hifi_cap_stats_t;

/* Stats of one channel (0 or 1) of interleaved stereo. */
void hifi_capture_stats(const int16_t *pcm, int frames, int ch, hifi_cap_stats_t *out);
/* The RMS of one sine in one channel (Goertzel), relative to full scale,
 * with the channel's DC and straight-line drift taken off first; exact when
 * the frames hold whole cycles of it. */
float hifi_capture_tone_rms(const int16_t *pcm, int frames, int ch, float freq_hz, int rate);


/* ------------------------------------------------------------ chime capture */

/*
 * An armed capture (docs/test-api.md, "Capturing a chime"): a chime is
 * 330 to 440 ms and the ADC takes about 370 ms to come up, so a capture
 * asked for after the trigger misses it. Armed, the next chime plays after a
 * silent lead-in while the ADC comes up; the board records it and finds its
 * notes, and GET capture/chime gives them: a few numbers, where the samples
 * would not fit through the remote path.
 */
#define HIFI_CAP_NOTES_MAX 6
#define HIFI_CAP_BLOCK_MS 10

typedef struct {
	float hz;
	int on_ms, len_ms;  /* from the recording's start, in whole blocks */
	float rms_fs[2];    /* the loudest block's tone RMS on each channel */
} hifi_cap_note_t;

typedef enum {
	HIFI_CAPCH_IDLE = 0,
	HIFI_CAPCH_ARMED,     /* waiting for a chime */
	HIFI_CAPCH_RECORDING,
	HIFI_CAPCH_DONE,
	HIFI_CAPCH_FAILED,    /* err says why: a timeout, the ADC, memory */
} hifi_capch_state_t;

typedef struct {
	hifi_capch_state_t state;
	const char *kind;   /* the chime recorded: "pair", "connect", "disconnect" */
	const char *err;
	hifi_cap_info_t info;
	int frames;
	int32_t start_ms;   /* the recording's first sample, after the chime was asked for */
	int32_t armed_ms;   /* the chime's request, after the arming */
	float floor_fs[2];  /* the quietest block's RMS: the noise the notes stand over */
	int n;
	hifi_cap_note_t notes[HIFI_CAP_NOTES_MAX];
} hifi_cap_chime_t;

const char *hifi_capch_state_name(hifi_capch_state_t s);

/*
 * Finds the notes in a recording (interleaved stereo): in each 10 ms block
 * the loudest of `hz` (Goertzel, the louder channel) counts when its tone RMS
 * is over min_fs; a run of at least min_blocks blocks on one frequency is a
 * note. Writes up to `max` notes in order; returns how many. Also fills
 * floor_fs[2] (may be NULL): the quietest block's RMS on each channel.
 */
int hifi_capture_find_notes(const int16_t *pcm, int frames, int rate, const float *hz, int nhz,
			    float min_fs, int min_blocks, hifi_cap_note_t *out, int max, float floor_fs[2]);
