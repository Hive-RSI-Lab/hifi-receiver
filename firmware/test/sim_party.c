/*
 * Party mode sync, simulated on the host: a leader and a follower, each
 * running the board's own clock code (hifi_party_sync.c) and the follower
 * the board's own controller and jitter buffer (hifi_party.c), under clocks
 * modelled from what the Lite has:
 *
 *   - true time t in µs. Each board has one crystal, ppm off: its esp_timer
 *     reads t * (1 + ppm) + an offset, and its I2S plays 44100 * (1 + ppm)
 *     frames a true second.
 *   - the DMA: a ring of NBUF buffers of 256 frames. Write position w plays at
 *     dma0 + (w + NBUF * 256) / fs; the buffer that starts at boundary b
 *     frees when the one a ring earlier finished. squeezelite writes blocks
 *     of 2048 frames; a write returns when the buffer of its last boundary
 *     freed, plus a wake-up latency (and now and then a pre-empting task), and
 *     the hook runs again just after that.
 *   - the Wi-Fi TSF: the AP's counter (its own ppm), which each station sets
 *     at every beacon (102.4 ms) and runs on its own crystal in between; a
 *     station that misses beacons drifts, and a re-lock can step it. An AP
 *     restart steps it for everyone, and a roam moves a board to another AP's
 *     TSF and BSSID.
 *   - a TSF read takes about 48 µs, bracketed between two esp_timer reads,
 *     now and then pre-empted; the TSF is taken somewhere inside.
 *   - the leader's CLOCK every 200 ms and its AUDIO, over Wi-Fi with a
 *     delivery delay, lost singly or in bursts.
 *
 * Every frame carries its stream index in its sample value, so the frames a
 * follower plays say which stream frame went out when; the sync error is the
 * true time the follower's DAC plays stream frame k minus the leader's.
 *
 * A case fails if, over the time the follower plays, the error's p99 is over
 * 100 µs, or over 1% of any 10 s of it is (so a few bad seconds can't hide
 * in a long run), or a step is audible: a jump (5 ms crossfade) other than the first
 * one after the follower was muted, or two neighbouring frames more than one
 * frame apart outside a jump; or a mute that cuts the sound rather than
 * fading it, or a resume that doesn't fade in; or the follower is muted
 * longer than the case allows. The BSSID is polled once a second, so the
 * TSF of a new AP can arrive under the old one's BSSID for up to a second.
 * SEED=n draws every case anew. Then the base case runs with two deliberately
 * broken servos (the correction's sign flipped; the slips left out), and
 * each must fail. The
 * program exits non-zero when a good case fails or a broken one passes.
 *
 *   build/sim_party            all cases, then the broken servos
 *   build/sim_party -v         and the counters per case
 *   build/sim_party CASE...    only those cases (no broken-servo run)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hifi_party.h"
#include "hifi_party_sync.h"

#define RATE 44100
#define BUF 256           /* dma_buf_len on the Lite */
#define NBUF 8            /* DMA buffers in the ring */
#define BLOCK 2048        /* frames per squeezelite write */
#define DELAY 13230       /* the leader plays its stream 300 ms late */
#define JB_SLOTS 128
#define CLOCK_EVERY_US 200000
#define TSF_EVERY_US 50000
#define BEACON_US 102400.0
#define LIMIT_US 100.0    /* p99 pass limit */
#define WINDOW_S 10       /* and over every window this long */
#define SIM_MAX_S 600
#define DECIM 4           /* errors kept: one frame in DECIM */

/* ------------------------------------------------------------ random */

static uint64_t rng_s;
static uint64_t rnd64(void) {
	uint64_t z = (rng_s += 0x9e3779b97f4a7c15ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}
static double uni(void) { return (double) (rnd64() >> 11) / 9007199254740992.0; }
static double urange(double a, double b) { return a + (b - a) * uni(); }
static double expo(double mean) { return -mean * log(1.0 - uni()); }
/* a fixed noise per (board, beacon): the same beacon reads the same */
static double hash_noise(uint64_t a, uint64_t b) {
	uint64_t z = a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	z ^= z >> 31;
	return (double) (z >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

/* ------------------------------------------------------------ a case */

typedef struct {
	const char *name, *what;
	double sim_s;
	double ppm_leader, ppm_follower, ppm_ap;
	double follower_start_s;
	/* write wake-up: base + exponential, now and then a pre-empting task, and
	 * busy seconds where every write is late */
	double wake_mean_us, spike_p, spike_max_us, busy_p;
	/* the TSF read: the bracket, sometimes pre-empted; the station's error at a beacon */
	double tsf_wide_p, beacon_noise_us;
	/* CLOCK delivery: delay, single losses, bursts [start, end) */
	double clk_delay_mean_us, clk_tail_p, clk_loss_p;
	double burst_at_s[4], burst_len_s[4];
	/* AP restart: TSF steps for every station */
	double ap_step_at_s, ap_step_us;
	/* the follower misses beacons over [miss_at, miss_at + miss_len) with its
	 * TSF stepped by miss_step_us, and re-locks at the end */
	double miss_at_s, miss_len_s, miss_step_us;
	/* roams to AP 2 (another TSF and BSSID) */
	double roam_leader_s, roam_follower_s;
	double max_mute_s; /* the follower may be muted this long after it first plays */
} scase_t;

static const scase_t CASES[] = {
	{.name = "base", .what = "measured crystals: leader -2.9 ppm, follower +23.6", .sim_s = 120, .ppm_leader = -2.9, .ppm_follower = 23.6,
	 .ppm_ap = 4},
	{.name = "drift+30", .what = "leader -30 ppm, follower +30 ppm", .sim_s = 120, .ppm_leader = -30, .ppm_follower = 30, .ppm_ap = -20},
	{.name = "drift-30", .what = "leader +30 ppm, follower -30 ppm", .sim_s = 120, .ppm_leader = 30, .ppm_follower = -30, .ppm_ap = 20},
	{.name = "clock-loss", .what = "10% of CLOCKs lost, one at a time", .sim_s = 120, .ppm_leader = -2.9, .ppm_follower = 23.6, .ppm_ap = 4,
	 .clk_loss_p = 0.10},
	{.name = "clock-burst", .what = "CLOCKs lost for 1.5 s, then 4 s (past the 2 s stale limit)", .sim_s = 120, .ppm_leader = -30,
	 .ppm_follower = 30, .ppm_ap = 0, .burst_at_s = {40, 80}, .burst_len_s = {1.5, 4}, .max_mute_s = 3},
	{.name = "ap-restart", .what = "the AP restarts: TSF steps back to ~1 s for everyone", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .ap_step_at_s = 50, .ap_step_us = -1, .max_mute_s = 12},
	{.name = "ap-step-fwd", .what = "the AP's TSF steps forward 3 s", .sim_s = 120, .ppm_leader = -2.9, .ppm_follower = 23.6, .ppm_ap = 4,
	 .ap_step_at_s = 50, .ap_step_us = 3e6, .max_mute_s = 12},
	{.name = "roam", .what = "both roam to another AP, the follower 2 s after the leader", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .roam_leader_s = 50, .roam_follower_s = 52, .max_mute_s = 12},
	{.name = "relock", .what = "follower misses beacons 3 s with its TSF 1.5 ms off, then re-locks", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .miss_at_s = 50, .miss_len_s = 3, .miss_step_us = 1500, .max_mute_s = 12},
	{.name = "relock-small", .what = "follower misses beacons 5 s (drifts ~220 µs), no step", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = -20, .miss_at_s = 50, .miss_len_s = 5, .max_mute_s = 12},
	{.name = "relock-60ppm", .what = "follower misses beacons 5 s at +30 ppm, the AP at -30 (drifts 300 µs), no step", .sim_s = 120,
	 .ppm_leader = -2.9, .ppm_follower = 30, .ppm_ap = -30, .miss_at_s = 50, .miss_len_s = 5, .max_mute_s = 12},
	{.name = "relock-60ppm-3s", .what = "the same, beacons missed 3 s (drifts 180 µs)", .sim_s = 120,
	 .ppm_leader = -2.9, .ppm_follower = 30, .ppm_ap = -30, .miss_at_s = 50, .miss_len_s = 3, .max_mute_s = 12},
	{.name = "relock-step150", .what = "follower misses beacons 1 s with its TSF 150 µs off", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .miss_at_s = 50, .miss_len_s = 1, .miss_step_us = 150, .max_mute_s = 12},
	{.name = "relock-early", .what = "relock, at 6 s while the TSF fit is young", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .miss_at_s = 6, .miss_len_s = 3, .miss_step_us = 1500, .max_mute_s = 12},
	{.name = "relock-5s", .what = "relock, beacons missed 5 s", .sim_s = 120, .ppm_leader = -2.9, .ppm_follower = 23.6, .ppm_ap = 4,
	 .miss_at_s = 50, .miss_len_s = 5, .miss_step_us = 1500, .max_mute_s = 12},
	{.name = "relock-8s", .what = "relock, beacons missed 8 s: disconnected after 6", .sim_s = 120, .ppm_leader = -2.9, .ppm_follower = 23.6, .ppm_ap = 4,
	 .miss_at_s = 50, .miss_len_s = 8, .miss_step_us = 1500, .max_mute_s = 12},
	{.name = "short-mute", .what = "CLOCKs lost 2.3 s: muted ~0.3 s, the old audio still buffered", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .burst_at_s = {40}, .burst_len_s = {2.3}, .max_mute_s = 3},
	{.name = "wifi-jitter", .what = "CLOCK delay 2-60 ms, 2% at 0.3-1 s; TSF reads pre-empted 10%", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .clk_delay_mean_us = 15000, .clk_tail_p = 0.02, .tsf_wide_p = 0.10, .beacon_noise_us = 3},
	{.name = "dac-noise", .what = "write wake-ups mean 150 µs, 5% late 0.5-8 ms, 3% busy seconds", .sim_s = 120, .ppm_leader = -2.9,
	 .ppm_follower = 23.6, .ppm_ap = 4, .wake_mean_us = 150, .spike_p = 0.05, .spike_max_us = 8000, .busy_p = 0.03},
	{.name = "late-join", .what = "the follower starts 40 s after the leader", .sim_s = 120, .ppm_leader = -2.9, .ppm_follower = 23.6,
	 .ppm_ap = 4, .follower_start_s = 40},
	{.name = "all", .what = "+-30 ppm, losses, jitter, DAC noise and a late join together", .sim_s = 150, .ppm_leader = 30,
	 .ppm_follower = -30, .ppm_ap = 10, .follower_start_s = 10, .wake_mean_us = 120, .spike_p = 0.03, .spike_max_us = 5000,
	 .busy_p = 0.02, .tsf_wide_p = 0.08, .beacon_noise_us = 3, .clk_delay_mean_us = 10000, .clk_tail_p = 0.01, .clk_loss_p = 0.10,
	 .burst_at_s = {70}, .burst_len_s = {1.5}, .max_mute_s = 1}, /* a lost CLOCK each side of the burst leaves one over 2 s old: a short, deliberate mute */
};
#define NCASES ((int) (sizeof(CASES) / sizeof(CASES[0])))

typedef enum { SERVO_OK, SERVO_SIGN, SERVO_NOSLIP } servo_t;
static const char *SERVO_NAME[] = {"", "sign flipped", "no slips"};

/* ------------------------------------------------------------ a board */

static const uint8_t BSSID1[6] = {0x02, 0xaa, 0, 0, 0, 1}, BSSID2[6] = {0x02, 0xbb, 0, 0, 0, 2};

typedef struct {
	int id;
	bool follower;
	double ppm, off_us, start;
	double fs;           /* frames per true µs */
	double dma0;         /* true µs at which the DMA started */
	int64_t w;           /* frames written */
	double next_hook;    /* true µs of the next hook and write */
	double next_tsf;
	double roam_at;
	uint8_t bssid[6];    /* the AP as last polled, once a second */
	double next_poll;
	double busy_win;     /* the busy second this board is in, or -1 */
	hifi_party_sync_t sync;
	/* leader */
	double last_clock;
	uint32_t seq;
	int64_t sent_k;      /* stream frames packed so far */
	/* follower */
	hifi_party_ctl_t ctl;
	hifi_party_jb_t jb;
	void *jbmem;
	uint32_t underruns;
} board_t;

static const scase_t *C;
static board_t L, F;

static int64_t local_us(const board_t *b, double t) { return llround(t * (1 + b->ppm * 1e-6) + b->off_us); }
static double play_at(const board_t *b, int64_t w) { return b->dma0 + (double) (w + NBUF * BUF) / b->fs; }
static double free_at(const board_t *b, int64_t bound) { return b->dma0 + (double) (bound + BUF) / b->fs; }

static bool on_ap2(const board_t *b, double t) { return b->roam_at > 0 && t >= b->roam_at; }

/* the AP's TSF at true time t */
static double ap_tsf(int ap, double t) {
	if (ap == 2) return 7.77e9 + t * (1 + (C->ppm_ap + 11) * 1e-6);
	double v = 1.234e12 + t * (1 + C->ppm_ap * 1e-6);
	if (C->ap_step_at_s > 0 && t >= C->ap_step_at_s * 1e6) {
		if (C->ap_step_us < 0)
			v = 1e6 + (t - C->ap_step_at_s * 1e6) * (1 + C->ppm_ap * 1e-6); /* restarted from ~1 s */
		else
			v += C->ap_step_us;
	}
	return v;
}

/* the board's TSF at true time t: set at the last beacon it heard, then run
 * on its own crystal */
#define MISS_DISCONNECT_US 6e6
static int64_t board_tsf(const board_t *b, double t) {
	int ap = on_ap2(b, t) ? 2 : 1;
	double tb = floor(t / BEACON_US) * BEACON_US, step = 0;
	if (b->follower && C->miss_len_s > 0) {
		double m0 = C->miss_at_s * 1e6, m1 = m0 + C->miss_len_s * 1e6;
		if (t >= m0 && t < m1) {
			if (t >= m0 + MISS_DISCONNECT_US) return 0; /* ESP-IDF disconnects after 6 s of no beacon: no TSF until it reassociates */
			tb = floor(m0 / BEACON_US) * BEACON_US;
			step = C->miss_step_us;
		}
	}
	if (b->roam_at > 0 && t >= b->roam_at && tb < b->roam_at) tb = b->roam_at; /* the first beacon of the new AP */
	double noise = C->beacon_noise_us * hash_noise((uint64_t) b->id, (uint64_t) (tb / BEACON_US));
	double v = ap_tsf(ap, tb) + noise + (t - tb) * (1 + b->ppm * 1e-6) + step;
	return llround(v);
}

/* ------------------------------------------------------------ messages */

typedef struct {
	double at;
	int64_t k, t;
	uint32_t rate, seq;
	uint8_t bssid[6];
} clock_msg_t;

#define QCLK 64
static clock_msg_t clk_q[QCLK];
static int clk_n;

#define QAUD 512
static struct {
	double at;
	int64_t k;
} aud_q[QAUD];
static int aud_head, aud_n;
static double aud_last;

static bool in_burst(double t) {
	for (int i = 0; i < 4; i++)
		if (C->burst_len_s[i] > 0 && t >= C->burst_at_s[i] * 1e6 && t < (C->burst_at_s[i] + C->burst_len_s[i]) * 1e6) return true;
	return false;
}

static void send_clock(double t, int64_t k, int64_t tref, uint32_t rate, uint32_t seq, const uint8_t *bssid) {
	if (in_burst(t) || uni() < C->clk_loss_p) return;
	double d = 1500 + (C->clk_delay_mean_us > 0 ? expo(C->clk_delay_mean_us) : expo(1500));
	if (uni() < C->clk_tail_p) d = urange(300000, 1000000);
	if (clk_n == QCLK) return; /* the queue is full: lost */
	clk_q[clk_n++] = (clock_msg_t){.at = t + d, .k = k, .t = tref, .rate = rate, .seq = seq};
	memcpy(clk_q[clk_n - 1].bssid, bssid, 6);
}

static void send_audio(double t, int64_t k) {
	if (aud_n == QAUD) {
		fprintf(stderr, "audio queue full\n");
		exit(2);
	}
	double at = t + 1000 + expo(C->clk_delay_mean_us > 0 ? C->clk_delay_mean_us / 2 : 1000);
	if (at < aud_last) at = aud_last; /* in order, as the follower NACKs gaps anyway */
	aud_last = at;
	int i = (aud_head + aud_n++) % QAUD;
	aud_q[i].at = at;
	aud_q[i].k = k;
}

/* deliver what has arrived by true time t */
static void deliver(double t) {
	bool fol = t >= F.start;
	for (int i = 0; i < clk_n;) {
		if (clk_q[i].at <= t) {
			clock_msg_t m = clk_q[i];
			clk_q[i] = clk_q[--clk_n];
			if (fol) hifi_party_sync_clock(&F.sync, m.k, m.t, m.rate, m.seq, m.bssid, local_us(&F, m.at));
		} else
			i++;
	}
	static uint32_t pkt[HIFI_PARTY_FRAMES];
	while (aud_n && aud_q[aud_head].at <= t) {
		int64_t k = aud_q[aud_head].k;
		double at = aud_q[aud_head].at;
		aud_head = (aud_head + 1) % QAUD;
		aud_n--;
		if (!fol) continue;
		for (int i = 0; i < HIFI_PARTY_FRAMES; i++) pkt[i] = (uint32_t) (k + i);
		hifi_party_jb_put(&F.jb, k, pkt, HIFI_PARTY_FRAMES, local_us(&F, at));
	}
}

/* ------------------------------------------------------------ measuring */

typedef struct {
	float *err;
	size_t n, cap;
	double first_play, mute_us, muted_since;
	bool playing, have_prev;
	int64_t prev_k;
	uint32_t jumps_ok, jumps_bad, steps_bad, concealed_blocks, blocks, resume_bad, mute_bad;
	int skip; /* frames to leave out after a jump: the crossfade */
	/* per second of the run: samples, and those over LIMIT_US */
	uint32_t sec_n[SIM_MAX_S], sec_over[SIM_MAX_S];
} meas_t;

static meas_t M;

static void keep(double t, double e) {
	int sec = (int) (t / 1e6);
	if (sec >= 0 && sec < SIM_MAX_S) {
		M.sec_n[sec]++;
		M.sec_over[sec] += fabs(e) > LIMIT_US;
	}
	if (M.n == M.cap) {
		M.cap = M.cap ? M.cap * 2 : 1 << 16;
		M.err = realloc(M.err, M.cap * sizeof(float));
		if (!M.err) exit(2);
	}
	M.err[M.n++] = (float) e;
}

/* ------------------------------------------------------------ one write */

static double wake(board_t *b, double t) {
	double d = 25 + expo(C->wake_mean_us > 0 ? C->wake_mean_us : 30);
	if (C->spike_p > 0 && uni() < C->spike_p) d += urange(500, C->spike_max_us);
	if (C->busy_p > 0) {
		double win = floor(t / 1e6);
		if (win != b->busy_win && uni() < C->busy_p / 10) b->busy_win = win; /* about busy_p of seconds */
		if (win == b->busy_win) d += urange(1000, 3000);
	}
	return d;
}

static void board_hook(board_t *b, double t, servo_t servo) {
	int64_t now = local_us(b, t);
	static uint32_t out[BLOCK];
	if (!b->follower) {
		/* the leader taps stream frames [w, w + BLOCK) and plays w - DELAY */
		int64_t s1 = b->w + BLOCK;
		while (b->sent_k + HIFI_PARTY_FRAMES <= s1) {
			send_audio(t, b->sent_k);
			b->sent_k += HIFI_PARTY_FRAMES;
		}
		int64_t tref;
		uint32_t rate;
		uint8_t bssid[6];
		if (t - b->last_clock >= CLOCK_EVERY_US / (1 + b->ppm * 1e-6) && hifi_party_sync_leader_clock(&b->sync, b->w, &tref, &rate, bssid)) {
			b->seq++;
			send_clock(t, b->w - DELAY, tref, rate, b->seq, bssid);
			b->last_clock = t;
		}
	} else {
		double target;
		bool on = hifi_party_sync_target(&b->sync, b->w, now, &target);
		if (on) {
			if (servo == SERVO_SIGN && b->ctl.started) target = 2.0 * (double) b->ctl.next - target;
			if (servo == SERVO_NOSLIP && b->ctl.started && fabs((double) b->ctl.next - target) < b->ctl.jump_frames) target = (double) b->ctl.next;
			uint32_t jumps0 = b->ctl.jumps, miss0 = b->jb.missing_frames;
			bool resumed = !M.playing;
			hifi_party_render(&b->ctl, &b->jb, target, out, BLOCK, 65536, 65536);
			M.blocks++;
			if (resumed) {
				if (M.first_play < 0) M.first_play = t;
				else M.mute_us += t - M.muted_since;
				M.playing = true;
				M.have_prev = false;
			}
			bool jumped = b->ctl.jumps != jumps0;
			if (jumped) {
				if (resumed || M.blocks == 1) M.jumps_ok++;
				else M.jumps_bad++;
				if (getenv("SIM_TRACE"))
					printf("      t %.3f s jump err %.1f frames%s, F tsf resets %u, clk seq %u age %.0f ms\n", t / 1e6, b->ctl.last_err,
					       resumed ? " (resume)" : "", b->sync.tsf_resets, b->sync.clk_seq, (now - b->sync.clk_at) / 1e3);
				M.have_prev = false;
				M.skip = HIFI_PARTY_CONCEAL;
			}
			if (resumed) {
				/* back from silence: the block fades in, so the frames don't say
				 * their index; it must rise no faster than the fade-in */
				for (int i = 0; i < BLOCK; i++) {
					int lim = i < HIFI_PARTY_FADE_IN ? 32768 * (i + 1) / HIFI_PARTY_FADE_IN + 1 : 32768;
					if (abs((int16_t) (uint16_t) out[i]) > lim || abs((int16_t) (uint16_t) (out[i] >> 16)) > lim) {
						M.resume_bad++;
						break;
					}
				}
				M.have_prev = false;
			} else if (b->jb.missing_frames != miss0 || b->jb.fade_in > 0) {
				M.concealed_blocks++; /* no audio is lost in these cases: a gap is a fault of the model */
				M.have_prev = false;
			} else {
				for (int i = 0; i < BLOCK; i++) {
					int64_t k = (int64_t) out[i];
					if (M.skip > 0) {
						M.skip--;
						continue;
					}
					if (M.have_prev) {
						int64_t d = k - M.prev_k;
						if (d < 0 || d > 2) M.steps_bad++;
					}
					M.prev_k = k;
					M.have_prev = true;
					if (i % DECIM == 0) keep(t, play_at(b, b->w + i) - play_at(&L, k + DELAY));
				}
			}
		} else {
			hifi_party_mute(&b->ctl, &b->jb, out, BLOCK, 65536, 65536);
			/* the first mute block fades out what played last, no faster
			 * than the concealment does; then silence */
			for (int i = 0; i < BLOCK; i++) {
				int lim = M.playing && i < HIFI_PARTY_CONCEAL ? 32768 * (HIFI_PARTY_CONCEAL - i) / HIFI_PARTY_CONCEAL + 1 : 0;
				if (abs((int16_t) (uint16_t) out[i]) > lim || abs((int16_t) (uint16_t) (out[i] >> 16)) > lim) {
					M.mute_bad++;
					break;
				}
			}
			if (M.playing) {
				M.playing = false;
				M.muted_since = t;
			}
		}
	}
	/* the write: each boundary waits for its buffer; it returns after the last one's */
	double t0 = t + 300;
	int64_t last_b = b->w + BLOCK - BUF;
	if (play_at(b, b->w) < t0) b->underruns++;
	double t1 = fmax(t0 + 100, free_at(b, last_b)) + wake(b, t);
	hifi_party_sync_wrote(&b->sync, b->w, b->w + BLOCK, now + 300, local_us(b, t1), BLOCK);
	b->w += BLOCK;
	b->next_hook = t1 + 20;
}

static void board_tsf_read(board_t *b, double t) {
	double width = 48 + expo(4);
	if (C->tsf_wide_p > 0 && uni() < C->tsf_wide_p) width += urange(100, 3000);
	else if (uni() < 0.02) width += urange(100, 1000);
	double at = t + urange(5, width - 5);
	if (t >= b->next_poll) {
		memcpy(b->bssid, on_ap2(b, t) ? BSSID2 : BSSID1, 6);
		b->next_poll = t + 1e6;
	}
	uint32_t resets0 = b->sync.tsf_resets;
	bool ok0 = b->sync.tsf.ok;
	hifi_party_sync_tsf(&b->sync, local_us(b, t), local_us(b, t + width), board_tsf(b, at), b->bssid);
	if (getenv("SIM_TRACE") && (b->sync.tsf_resets != resets0 || b->sync.tsf.ok != ok0))
		printf("      t %.3f s %s TSF fit %s%s\n", t / 1e6, b->follower ? "F" : "L", b->sync.tsf_resets != resets0 ? "reset" : "",
		       b->sync.tsf.ok ? " ok" : " not ok");
	b->next_tsf = t + TSF_EVERY_US / (1 + b->ppm * 1e-6);
}

static void board_start(board_t *b, int id, bool follower, double ppm, double start, double roam_s) {
	void *mem = b->jbmem;
	memset(b, 0, sizeof(*b));
	b->id = id;
	b->follower = follower;
	b->ppm = ppm;
	b->off_us = urange(1e6, 1e9);
	b->start = start;
	b->fs = RATE * (1 + ppm * 1e-6) / 1e6;
	b->dma0 = start;
	b->next_hook = start;
	b->next_tsf = start + urange(0, TSF_EVERY_US);
	b->roam_at = roam_s > 0 ? roam_s * 1e6 : 0;
	b->busy_win = -1;
	b->last_clock = -1e18;
	hifi_party_sync_init(&b->sync, BUF);
	if (follower) {
		hifi_party_ctl_init(&b->ctl, RATE);
		if (!mem) mem = malloc(hifi_party_jb_bytes(JB_SLOTS));
		b->jbmem = mem;
		hifi_party_jb_init(&b->jb, mem, JB_SLOTS);
	} else
		b->jbmem = mem;
}

/* ------------------------------------------------------------ a run */

typedef struct {
	double mean, p50, p99, max, mute_s, play_s;
	double worst_frac; /* the most of any WINDOW_S that is over LIMIT_US */
	int worst_at;
	uint32_t jumps_ok, jumps_bad, steps_bad, concealed;
	bool pass;
	char why[96];
} result_t;

static int cmpf(const void *a, const void *b) {
	float x = *(const float *) a, y = *(const float *) b;
	return (x > y) - (x < y);
}

static bool verbose;

static result_t run(const scase_t *c, servo_t servo, uint64_t seed) {
	C = c;
	rng_s = seed;
	clk_n = aud_n = aud_head = 0;
	aud_last = 0;
	free(M.err);
	memset(&M, 0, sizeof(M));
	M.first_play = -1;
	board_start(&L, 1, false, c->ppm_leader, 0, c->roam_leader_s);
	board_start(&F, 2, true, c->ppm_follower, c->follower_start_s * 1e6 + urange(0, 1e5), c->roam_follower_s);
	double end = c->sim_s * 1e6;
	for (;;) {
		/* the next event of four */
		double t = L.next_hook;
		int ev = 0;
		if (L.next_tsf < t) t = L.next_tsf, ev = 1;
		if (F.next_hook < t) t = F.next_hook, ev = 2;
		if (F.next_tsf < t) t = F.next_tsf, ev = 3;
		if (t >= end) break;
		deliver(t);
		if (ev == 0) board_hook(&L, t, servo);
		else if (ev == 1) board_tsf_read(&L, t);
		else if (ev == 2) board_hook(&F, t, servo);
		else board_tsf_read(&F, t);
	}
	if (!M.playing && M.first_play >= 0) M.mute_us += end - M.muted_since;
	result_t r = {0};
	r.jumps_ok = M.jumps_ok;
	r.jumps_bad = M.jumps_bad;
	r.steps_bad = M.steps_bad;
	r.concealed = M.concealed_blocks;
	r.mute_s = M.mute_us / 1e6;
	r.play_s = M.first_play >= 0 ? (end - M.first_play) / 1e6 - r.mute_s : 0;
	double sum = 0;
	for (size_t i = 0; i < M.n; i++) sum += M.err[i], M.err[i] = fabsf(M.err[i]);
	r.mean = M.n ? sum / (double) M.n : 0;
	if (M.n) {
		qsort(M.err, M.n, sizeof(float), cmpf);
		r.p50 = M.err[M.n / 2];
		r.p99 = M.err[(size_t) ((double) (M.n - 1) * 0.99)];
		r.max = M.err[M.n - 1];
	}
	for (int s0 = 0; s0 + WINDOW_S <= SIM_MAX_S; s0++) {
		uint32_t n = 0, over = 0;
		for (int i = s0; i < s0 + WINDOW_S; i++) n += M.sec_n[i], over += M.sec_over[i];
		if (n >= 1000 && (double) over / n > r.worst_frac) r.worst_frac = (double) over / n, r.worst_at = s0;
	}
	r.pass = true;
	if (M.n == 0 || r.play_s < (c->sim_s - c->follower_start_s) * 0.5)
		r.pass = false, snprintf(r.why, sizeof(r.why), "played only %.1f s", r.play_s);
	else if (r.p99 > LIMIT_US)
		r.pass = false, snprintf(r.why, sizeof(r.why), "p99 %.1f µs > %.0f", r.p99, LIMIT_US);
	else if (r.worst_frac > 0.01)
		r.pass = false, snprintf(r.why, sizeof(r.why), "%.1f%% over %.0f µs in the %d s from %d s", 100 * r.worst_frac, LIMIT_US, WINDOW_S,
		                         r.worst_at);
	else if (M.jumps_bad || M.steps_bad || M.resume_bad || M.mute_bad)
		r.pass = false, snprintf(r.why, sizeof(r.why), "audible: %u jumps, %u steps, %u loud resumes, %u hard mutes", M.jumps_bad, M.steps_bad,
		                         M.resume_bad, M.mute_bad);
	else if (r.mute_s > c->max_mute_s + 0.25)
		r.pass = false, snprintf(r.why, sizeof(r.why), "muted %.1f s > %.1f", r.mute_s, c->max_mute_s);
	else if (r.concealed)
		r.pass = false, snprintf(r.why, sizeof(r.why), "%u blocks concealed", r.concealed);
	if (L.underruns || F.underruns) r.pass = false, snprintf(r.why, sizeof(r.why), "the model's DMA ran dry");
	if (verbose)
		printf("    %s: L dac/tsf resets %u/%u, F dac/tsf resets %u/%u, roams %u, clocks %u (old %u, stale %u), slips %u+%u, "
		       "jumps %u, first play %.1f s\n",
		       c->name, L.sync.dac_resets, L.sync.tsf_resets, F.sync.dac_resets, F.sync.tsf_resets, F.sync.bssid_changes,
		       F.sync.clocks, F.sync.old_clocks, F.sync.stale, F.ctl.drops, F.ctl.repeats, F.ctl.jumps, M.first_play / 1e6);
	return r;
}

static void print_head(void) {
	printf("  %-13s %8s %8s %8s %8s %7s %7s  %s\n", "case", "mean µs", "p50 µs", "p99 µs", "max µs", "mute s", "jumps", "result");
}

static void print_row(const scase_t *c, const result_t *r) {
	printf("  %-13s %+8.1f %8.1f %8.1f %8.1f %7.1f %7u  %s%s%s\n", c->name, r->mean, r->p50, r->p99, r->max, r->mute_s, r->jumps_ok + r->jumps_bad,
	       r->pass ? "pass" : "FAIL", r->pass ? "" : ": ", r->why);
}

int main(int argc, char **argv) {
	int sel[NCASES], nsel = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			verbose = true;
			continue;
		}
		int j = 0;
		while (j < NCASES && strcmp(CASES[j].name, argv[i])) j++;
		if (j == NCASES) {
			fprintf(stderr, "no case %s; the cases:", argv[i]);
			for (j = 0; j < NCASES; j++) fprintf(stderr, " %s", CASES[j].name);
			fprintf(stderr, "\n");
			return 2;
		}
		sel[nsel++] = j;
	}
	bool all = nsel == 0;
	if (all)
		for (int j = 0; j < NCASES; j++) sel[nsel++] = j;

	/* SEED=n runs every case on other random draws */
	uint64_t seed = getenv("SEED") ? strtoull(getenv("SEED"), NULL, 10) : 1000;
	int fails = 0;
	printf("sim_party: follower sync error against the leader, |error| over the time the follower plays\n");
	print_head();
	for (int i = 0; i < nsel; i++) {
		result_t r = run(&CASES[sel[i]], SERVO_OK, seed + (uint64_t) sel[i]);
		print_row(&CASES[sel[i]], &r);
		if (!r.pass) fails++;
	}
	if (all) {
		/* a servo that is wrong must be caught: every one fails the base case */
		for (int s = SERVO_SIGN; s <= SERVO_NOSLIP; s++) {
			printf("broken servo (%s), which must fail:\n", SERVO_NAME[s]);
			result_t r = run(&CASES[0], (servo_t) s, seed);
			print_row(&CASES[0], &r);
			if (r.pass) {
				printf("  ...but it passed: the simulation does not catch a broken servo\n");
				fails++;
			}
		}
	}
	free(M.err);
	free(F.jbmem);
	printf("%s\n", fails ? "sim_party: FAILED" : "sim_party: all cases pass, and the broken servos fail");
	return fails ? 1 : 0;
}
