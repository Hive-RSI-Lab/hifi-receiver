/*
 * The test API (docs/test-api.md) as pure logic: a request (method, path,
 * body) in, a status and a JSON reply out. The board code implements
 * hifi_ops_t; the HTTP server and the serial console both call
 * hifi_api_handle(), and the host tests call it with a mock board.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_ir.h"
#include "hifi_meta.h"
#include "hifi_capture.h"
#include "hifi_diag.h"
#include "hifi_ota.h"
#include "hifi_pd.h"
#include "hifi_power.h"
#include "hifi_tone.h"
#include "hifi_update.h"

#define HIFI_API_VERSION 1

#define HIFI_EQ_BANDS 10

/* What the tier can do, so one web app serves all three. */
typedef struct {
	bool dj_direct;
	bool line;         /* the line in through the ADC, the DSP and the governor */
	float eq_min_db, eq_max_db;
	int sub_count;
	bool sub_crossover, sub_per_sub;
	float sub_max_db;  /* the highest sub level the board sets (Lite -9: its mute FETs) */
	int zones;
	bool eth;
	float pdo_v_max;
	/* Loudness (ask k2388): the most Boost may add, in dB (0: no Boost),
	 * and the heat cap's range and default, watts a channel sustained */
	float boost_max_db;
	float heat_min_w, heat_max_w, heat_def_w;
} hifi_caps_t;

typedef struct {
	const char *model, *board, *fw, *upstream;
	char name[65];     /* the player name people see in their apps */
	char mac[18];
	char hostname[33];
	int flash_mb, psram_mb;
	hifi_caps_t caps;
} hifi_info_t;

typedef enum { HIFI_INPUT_STREAM = 0, HIFI_INPUT_DJ_DIRECT, HIFI_INPUT_LINE } hifi_input_t;

/* "stream", "dj_direct", "line"; parse: false on an unknown name */
const char *hifi_input_name(hifi_input_t in);
bool hifi_input_parse(const char *s, hifi_input_t *in);

/* Which side of the program the speaker output plays (a Lite driving one
 * speaker of a pair): "stereo", "left", "right", "mono" ((L+R)/2) */
typedef enum { HIFI_CHAN_STEREO = 0, HIFI_CHAN_LEFT, HIFI_CHAN_RIGHT, HIFI_CHAN_MONO } hifi_chan_t;
const char *hifi_chan_name(hifi_chan_t c);
bool hifi_chan_parse(const char *s, hifi_chan_t *c);

/* Everything /state reports. Floats that can't be measured are NaN. */
typedef struct {
	uint32_t uptime_s;
	/* power */
	hifi_pwr_state_t pwr_state;
	float vbus_v, pvdd_v, vbus_i_a;
	bool pg, pvdd_on;
	hifi_contract_t contract;
	/* amp */
	bool amp_enabled, amp_fault, amp_latched;
	int amp_faults;
	float amp_temp_c;
	/* governor */
	float gov_budget_w, gov_in_w, gov_out_w, gov_gain_db;
	bool gov_limiting;
	uint32_t gov_events;
	/* loudness (set_loudness): reported only when `loudness` is true */
	bool loudness;
	const char *gov_why;      /* hifi_gov_why: "" when free */
	float heat_cap_w, heat_w; /* the cap and the sustained power now, a channel */
	bool amp_hot;             /* the amp's over-temperature warning is held */
	float boost_db, boost_limit_db; /* set, and the limiter's reduction now (<= 0) */
	bool boost_limiting;
	uint32_t boost_events;
	/* audio */
	const char *source; /* idle lms airplay spotify bt test */
	hifi_input_t input;
	bool tone_active;
	hifi_tone_params_t tone;
	float level_dbfs;
	/* DJ direct's thermal guard (boards whose ADC hears the line): the 60 s
	 * input power it sees, its limit, and why it last moved DJ direct to
	 * the governed line in (NULL: it hasn't) */
	bool dj_guard;
	float dj_guard_w, dj_guard_limit_w;
	const char *dj_guard_tripped;
	bool sub_on;
	float sub_db;
	hifi_chan_t channel;
	float eq[HIFI_EQ_BANDS];
	int volume, max_volume;   /* hifi_vol.h */
	bool mute;
	int zones;                /* 2: zone B below is reported */
	bool zone_b_on;           /* zone B plays zone A's program at its volume */
	hifi_meta_t playing;      /* elapsed_ms as of now; state NONE: null */
	/* remote */
	int ir_learning;          /* an action, or -1 */
	bool ir_learned[HIFI_IR_N];
	bool ir_have_last;        /* any code seen since boot */
	uint32_t ir_last_code;
	/* the controls, for bring-up: the I/O expander's and the front board's
	 * input ports as last read, the last button, knob step or remote
	 * action and how long ago, the backlight's mode. Unset (false, "",
	 * NULL): not on this board, and state leaves it out */
	bool exp_present;
	uint16_t exp_in;
	bool front_present;
	uint8_t front_in;
	char last_control[24];    /* "button toggle", "knob +2", "ir mute"... */
	uint32_t last_control_ago_s;
	const char *backlight;    /* "auto" "on" "off" */
	/* net, sys */
	const char *net_mode; /* "sta" or "ap" */
	bool eth_present, eth_up; /* a board with Ethernet; its link has an address */
	char ssid[33];
	char hostname[33];
	char ip[16];        /* empty when not connected */
	int rssi;           /* 0 when unknown */
	uint32_t heap_free, heap_min, psram_free; /* internal now, internal lowest since boot, PSRAM */
	uint32_t heap_boot; /* internal once settled after boot (hifi_boot.h); 0: not yet */
	const char *reset_reason; /* why it last restarted (power_on, panic...); NULL: unknown */
	const char *restart_cause; /* for a restart: who called it (hifi_diag.h hifi_restart_cause_fmt); NULL: none */
	hifi_update_status_t update; /* the app's updates; idle: not reported */
} hifi_snapshot_t;

#define HIFI_SELFTEST_MAX 16

typedef struct {
	char name[24];
	bool pass;
	char value[64];
} hifi_check_t;

typedef struct {
	int n;
	hifi_check_t c[HIFI_SELFTEST_MAX];
} hifi_selftest_t;

/* The relay link (docs/remote.md) */
typedef struct {
	bool configured, connected;
	char url[128], user[48], prefix[24];
	char dev[13];
	bool has_key;               /* a wss:// link has its key from the Hive (never shown) */
	int keys;                   /* trusted signing keys in this build */
	uint32_t accepted, rejected; /* commands since boot */
} hifi_remote_status_t;

/* A Wi-Fi network a scan found */
typedef struct {
	char ssid[33];
	int rssi;
	bool secure;
} hifi_wifi_ap_t;

#define HIFI_WIFI_SCAN_MAX 20

/* Which image runs, and what the last rollback was */
typedef struct {
	const char *slot;  /* "ota_0", "ota_1" or "recovery" */
	const char *state; /* "valid", "pending_verify" (on trial) or "unknown" */
	char last_from[32], last_reason[96]; /* empty: no rollback recorded */
} hifi_ota_status_t;

typedef struct {
	void (*info)(hifi_info_t *out);
	void (*snapshot)(hifi_snapshot_t *out);
	/* 0 on success, else an HTTP status with *err set */
	int (*tone_start)(const hifi_tone_params_t *p, bool force, const char **err);
	void (*tone_stop)(void);
	void (*amp_enable)(bool on);
	void (*amp_reset)(void);
	int (*set_input)(hifi_input_t in);
	/* on: -1 leaves it; db: NaN leaves it, else set as near as the
	 * hardware can. Returns the level it set. */
	float (*set_sub)(int on, float db);
	/* NULL: the board has no channel mode. 0 ok */
	int (*set_channel)(hifi_chan_t c);
	int (*set_eq)(const float gains_db[HIFI_EQ_BANDS]);
	/* Boost (dB) and the heat cap (W a channel); NaN leaves one. Checked
	 * against caps first. NULL: the board has neither (404) */
	int (*set_loudness)(float boost_db, float heat_cap_w);
	int (*set_name)(const char *name);
	void (*reboot)(void);        /* after the reply has gone */
	void (*factory_reset)(void); /* likewise */
	int (*control)(const char *action); /* 0 ok, -1 unknown action */
	/* -1 leaves a field; 0 ok, -1 out of range (hifi_vol_set) */
	int (*set_volume)(int volume, int mute, int max_volume);
	void (*ir_learn)(int action);  /* hifi_ir_action_t */
	void (*ir_forget)(int action); /* -1: all of them */
	void (*selftest)(hifi_selftest_t *out);

	/* Optional: NULL where the build or board can't (the call answers 404,
	 * or 409 for capture). */
	/* fills frames of interleaved stereo; 0, or an HTTP status with *err */
	int (*capture)(hifi_cap_src_t src, int16_t *pcm, int frames, hifi_cap_info_t *info, const char **err);
	int (*set_wifi)(const char *ssid, const char *password); /* 0: joining */
	void (*remote_status)(hifi_remote_status_t *out);
	/* key: the board's own key for the Hive's wss:// endpoint (64 hex), or "" for MQTT */
	int (*remote_set)(const char *url, const char *user, const char *password, const char *prefix, const char *key);
	hifi_ota_t *ota;   /* the update session, with the board's flash writes */
	void (*ota_status)(hifi_ota_status_t *out);
	/* the networks in range: how many it wrote, or -1 while the radio is busy */
	int (*wifi_scan)(hifi_wifi_ap_t *aps, int max);
	int (*wifi_forget)(void); /* back to the receiver's own network; 0 ok */
	/* zone B's amp on or off (Ultra; core has no level of its own for it) */
	void (*set_zone)(bool on);
	/* the update manifest's offer: 0, or an HTTP status with *err set */
	int (*update_check)(hifi_update_offer_t *out, const char **err);
	/* downloads and installs an image in the background (state's "update");
	 * sha256 NULL: the one the last check offered for that url, if any.
	 * need_sig: the image must carry the Hive's signature (a request from
	 * the LAN; hifi_ota.h). 0, or an HTTP status with *err set */
	int (*update_start)(const char *url, const char *sha256, bool need_sig, const char **err);
	/* bring-up. The LED test: "test" lights the board's LEDs solid (Ultra's
	 * front LED red, green, blue and white in turn) for 30 s or until
	 * "auto" hands them back. The backlight (Ultra): "on", "off" or
	 * "auto". 0 ok, -1 an unknown mode; NULL: the board has none (404) */
	int (*set_led)(const char *mode);
	int (*set_backlight)(const char *mode);
	/* why the board last restarted, and the console's last lines before it
	 * (printable text, at most cap - 1 bytes) for GET crash; NULL: 404 */
	const char *(*reset_reason)(void);
	size_t (*last_log)(char *out, size_t cap);
	/* bring-up: for the next boot only, play to the Bluetooth speaker called
	 * sink (another board) as a Bluetooth source, so pairing can be tested
	 * with no phone; the boot after that is back to normal. Restarts the
	 * board. 0, or -1 with *err set; NULL: 404 */
	int (*bt_test)(const char *sink, const char **err);
	/* The LAN's factory reset (L6): arm it (0, else -1); the erase happens
	 * only if the board next starts from power-on (unplugged and plugged
	 * back in) within HIFI_RESET_ARM_US. NULL: the LAN can't reset. */
	int (*factory_arm)(void);
	/* the lab build's experiments (hifi_lab.h): GET lists them, POST starts,
	 * stops or reads one. Returns the HTTP status with the reply in out;
	 * NULL outside a lab build: 404 */
	int (*lab)(bool post, const char *body, char *out, size_t cap);
	/* party mode (esp/hifi_party_esp.c): sub is "" for party, "stats" for
	 * party/stats; lan says the request came from port 80 (it may switch
	 * party on and off and pick the leader, not set the key). Returns the
	 * HTTP status with the reply in out; NULL: 404 */
	int (*party)(const char *sub, bool post, bool lan, const char *body, char *out, size_t cap);
	/* on-board diagnostics (hifi_diag.h); NULL: diag answers 404 */
	const hifi_diag_ops_t *diag;
} hifi_ops_t;

/* Where a request came from (security.md). The LAN (port 80) may play and
 * adjust; it may not change the relay, and firmware it sends must carry the
 * Hive's signature. The relay's signed commands and the USB console are
 * trusted. */
typedef enum { HIFI_SRC_TRUSTED = 0, HIFI_SRC_LAN } hifi_src_t;

/* Returns the HTTP status; the reply (always a JSON object) is in out.
 * hifi_api_handle is a trusted caller's (the relay, the console). */
int hifi_api_handle(const hifi_ops_t *ops, const char *method, const char *path,
		    const char *body, char *out, size_t cap);
int hifi_api_handle_from(const hifi_ops_t *ops, hifi_src_t src, const char *method, const char *path,
			 const char *body, char *out, size_t cap);

/* The reply buffer a request needs: 4 KB, or more for a capture. */
size_t hifi_api_reply_cap(const char *path, const char *body);

/* Shared by /state and the console */
void hifi_api_write_state(const hifi_snapshot_t *s, char *out, size_t cap);

/* /state/<part>: one top-level section (power, amp, governor, audio, net,
 * sys, ir, controls, update), or the whole state when part is NULL. A short
 * answer for a serial line: the whole state runs near 1000 characters. */
void hifi_api_write_state_part(const hifi_snapshot_t *s, const char *part, char *out, size_t cap);
bool hifi_api_state_part_known(const char *part);

/* Adds a check to a self-test result. */
void hifi_selftest_add(hifi_selftest_t *t, const char *name, bool pass, const char *fmt, ...);

/*
 * The serial console: turns `hifi <args...>` (argv without "hifi") into an
 * API request. Returns false with a usage line in `out` when the args don't
 * parse. body must hold at least 256 bytes.
 */
bool hifi_cli_request(int argc, const char *const *argv, const char **method,
		      const char **path, char *body, size_t body_cap, char *out, size_t cap);

/*
 * The console's `hifi i2c <addr> <reg> [val]` (argv from "i2c"): reads or
 * writes one register on the shared bus. Console only, never the LAN API.
 * A write is refused (409) while busy is set (what plays, or the amps on):
 * a DAC, ADC or amp register changed under live audio can pop or latch a
 * fault. rd and wr return 0 on an ack. The reply, a JSON object, is in out;
 * returns the HTTP-like status.
 */
typedef int (*hifi_i2c_rd_t)(uint8_t addr, uint8_t reg, uint8_t *val);
typedef int (*hifi_i2c_wr_t)(uint8_t addr, uint8_t reg, uint8_t val);
int hifi_i2c_cli(int argc, const char *const *argv, const char *busy, hifi_i2c_rd_t rd,
		 hifi_i2c_wr_t wr, char *out, size_t cap);
