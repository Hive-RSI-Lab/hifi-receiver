/*
 * Channel mode: which side of the program the speaker output plays, so one
 * Lite can drive one speaker of a stereo pair. The ES8388's cross-mix
 * registers (0x28 RD2LO, 0x29 LD2RO) do nothing on the chip (Lite 2,
 * 2026-11-17: the right leg read -40 dB in left mode), so a stream or line
 * gets its channel in the DSP (hifi_chan_mix, after the governor) and the
 * codec's mixers stay stereo. DJ direct is analog and can't cross sides:
 * left or right plays that side on its own output only, mono plays stereo.
 * The mixers feed LOUT1/ROUT1 (amp) and LOUT2/ROUT2 (sub out) alike, so
 * the sub out follows the channel too.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_api.h"

/* ES8388 registers 0x27..0x2A (LD2LO/LI2LO, RD2LO/RI2LO, LD2RO/LI2RO, RD2RO/RI2RO),
 * the DAC's digital volume (0x1A, 0x1B; both the same) */
typedef struct {
	uint8_t mix[4];
	uint8_t dac_vol;
} hifi_chan_regs_t;

#define HIFI_CHAN_MIX_FIRST 0x27
#define HIFI_CHAN_DACVOL_L 0x1A
#define HIFI_CHAN_DACVOL_R 0x1B

/* direct: DJ direct, the analog inputs in the mix and the DAC out of it.
 * direct_lin_code: that path's input gain code (bits 5:3; 3 = -3 dB);
 * live: false leaves the source out of the mix (DJ direct muted). */
void hifi_chan_regs(hifi_chan_t c, bool direct, int direct_lin_code, bool live, hifi_chan_regs_t *r);

/* The channel on an output block of interleaved stereo frames, in place:
 * 4 bytes a frame (16-bit) or 8 (32-bit). Left or right copies that side to
 * both; mono plays (L + R) / 2 on both; stereo leaves it. */
void hifi_chan_mix(hifi_chan_t c, void *buf, size_t frames, int bytes_per_frame);

/* A stream's gain from one side of the program (side 0 left, 1 right) to
 * each output, g[0] left and g[1] right: 1, 0, or 0.5 in mono. */
void hifi_chan_gain(hifi_chan_t c, int side, float g[2]);

/*
 * The self-test's loopback check (docs/test-api.md, "loopback"): a tone on
 * the left side, then the right, read back on both taps, got[side][leg] in
 * RMS full scale. A leg the channel mode routes the side to reads within
 * tol_db of want_fs times its gain (hifi_chan_gain). The self-test measures
 * with the EQ skipped and stereo routing, so it passes HIFI_CHAN_STEREO and
 * HIFI_LOOP_TOL_DB. One it doesn't route the side to reads
 * HIFI_LOOP_OFF_DB under want_fs or more. Writes the value, each reading in
 * dB against want_fs, and returns whether all four pass.
 */
#define HIFI_LOOP_TOL_DB 3.0f
#define HIFI_LOOP_OFF_DB 20.0f
bool hifi_loop_judge(hifi_chan_t c, const float got[2][2], float want_fs, float tol_db, char *out, size_t n);
