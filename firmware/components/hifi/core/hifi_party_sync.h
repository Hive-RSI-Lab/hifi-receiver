/*
 * Party mode: one board's clocks, without the radio or the I2S driver. This is
 * the part of esp/hifi_party_esp.c that turns timestamps into "which stream frame
 * should this block start with", moved here so the host simulation
 * (test/sim_party.c) runs the same code the board does.
 *
 * Two fits (hifi_party.h) chain write position to TSF:
 *   dac: write position w -> local µs (esp_timer) at which w's buffer freed
 *   tsf: local µs -> Wi-Fi TSF µs
 * so tsf_at(w) is the TSF at which write position w reaches the DAC, up to a
 * constant that is the same on every Lite running this code.
 *
 * Each fit starts again when it stops describing its clock:
 *   - dac: a write that took less than its block (where the rest goes is
 *     unknown), an I2S restart, or the newest window more than the reject
 *     limit above the line three windows running (the DMA ran dry);
 *   - tsf: the newest window more than the reject limit off the line, either
 *     side, three windows running (the AP restarted or the TSF re-locked); a
 *     window that jumps from the one before by more than the reject limit
 *     when the line has no room to keep it out (the fit isn't ready, or the
 *     line took the step in); a jump back onto the line ends a step instead;
 *     a new BSSID (a roam: another AP's TSF). A reset also forgets the clock,
 *     which was in the old TSF.
 *
 * The leader turns its fits into a CLOCK (k_ref plays at TSF t_ref, rate
 * frames per 1000 TSF s); a follower keeps the last CLOCK and, per block,
 * works out the stream index the leader plays when that block reaches the
 * follower's DAC. The caller holds whatever lock guards the state.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hifi_party.h"

#define HIFI_PARTY_SYNC_RATE 44100
#define HIFI_PARTY_SYNC_STALE_US 2000000 /* a CLOCK older than this mutes the follower */
#define HIFI_PARTY_SYNC_BAD 3            /* windows off the line, running, that reset a fit */
#define HIFI_PARTY_SYNC_PPM 100          /* the TSF against esp_timer, at most (crystals ±30 ppm, the AP's too) */

typedef struct {
	hifi_party_fit_t dac, tsf;
	int dac_bad, tsf_bad;
	/* the TSF fit's last closed window, for the jump test (a step the young
	 * line would take in) */
	bool tsf_have_prev, tsf_step_open;
	int64_t tsf_prev_x, tsf_prev_y;
	uint8_t bssid[6];
	bool have_bssid;
	int buf_frames;     /* the DMA buffer length, from the last I2S restart */
	/* the last CLOCK (a follower's) */
	bool clk_ok;
	int64_t clk_k, clk_t, clk_at;
	uint32_t clk_rate, clk_seq;
	uint8_t clk_bssid[6];
	/* counters */
	uint32_t dac_resets, tsf_resets, short_writes, tsf_zero, bssid_changes, old_clocks, clocks, bssid_wrong, stale;
} hifi_party_sync_t;

void hifi_party_sync_init(hifi_party_sync_t *s, int buf_frames);
/* An I2S restart (start, or a new rate): the write position's alignment to
 * the DMA buffers is new, so the DAC fit starts again. */
void hifi_party_sync_restart(hifi_party_sync_t *s, int buf_frames);
/* A TSF read bracketed by two local reads a_us <= b_us, on the AP bssid. A
 * zero TSF (not associated: ESP-IDF drops the AP after 6 s with no beacon)
 * is counted and dropped, and the BSSID and the TSF fit with it: the next
 * association, to whichever AP, starts afresh. */
void hifi_party_sync_tsf(hifi_party_sync_t *s, int64_t a_us, int64_t b_us, int64_t tsf, const uint8_t bssid[6]);
/* An I2S write of frames [w0, w1) that began at local t0 and returned at t1;
 * last_block is the block the output was about to write (0: unknown). */
void hifi_party_sync_wrote(hifi_party_sync_t *s, int64_t w0, int64_t w1, int64_t t0, int64_t t1, int last_block);

bool hifi_party_sync_ok(const hifi_party_sync_t *s);
/* the TSF at which write position w reaches the DAC (meaningful when ok) */
double hifi_party_sync_tsf_at(const hifi_party_sync_t *s, int64_t w);

/* Leader: the CLOCK for write position w (k_ref is the stream index the
 * caller writes there): t_ref, the rate and the BSSID the TSF fit is in.
 * False while the fits aren't ready, or the rate is more than 1000 ppm off. */
bool hifi_party_sync_leader_clock(const hifi_party_sync_t *s, int64_t w, int64_t *t_ref, uint32_t *rate, uint8_t bssid[6]);

/* Follower: a CLOCK received at local now. Returns false (and keeps the old
 * one) when seq is not newer than the last (a replay, or late). */
bool hifi_party_sync_clock(hifi_party_sync_t *s, int64_t k_ref, int64_t t_ref, uint32_t rate, uint32_t seq, const uint8_t bssid[6], int64_t now);
/* Follower: forget the CLOCK and its sequence (a new leader, or a reset), so
 * the next CLOCK is taken whatever its seq. */
void hifi_party_sync_forget_clock(hifi_party_sync_t *s);
/* Follower: the stream index the leader plays when write position w reaches
 * this board's DAC. False (mute) unless both fits are ready, the CLOCK is
 * fresh and it is on the AP this board is on. */
bool hifi_party_sync_target(hifi_party_sync_t *s, int64_t w, int64_t now, double *target);
