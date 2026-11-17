/*
 * On-board diagnostics (docs/test-api.md, "Diagnostics"): what the board
 * knows about its own health and performance, as the test API's `diag`
 * calls. The board fills the structs below through hifi_diag_ops_t; this
 * file holds the pure parts (CPU load from two task samples, the reset
 * history, paging) and the JSON writers, so the host tests cover them.
 *
 * A remote bench caps a reply line at 1000 characters, so every reply here
 * stays under HIFI_DIAG_PAGE characters, and the task list pages with
 * {"from":N} and "next" like crash does.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_DIAG_TASKS_MAX 48
#define HIFI_DIAG_RESETS 8     /* reasons kept in the history */
#define HIFI_DIAG_HIST_CAP 112 /* "panic,sw,power_on,..." */
#define HIFI_DIAG_PAGE 900     /* the most any diag reply writes */

typedef struct {
	char name[17];
	uint32_t num;        /* the scheduler's task number, to match samples */
	uint32_t runtime;    /* run-time counter, in the counter's units (wraps) */
	uint32_t stack_free; /* stack high-water mark: least ever free, bytes */
	uint32_t stack_size; /* the whole stack, bytes; 0: unknown */
	char stack_ram;      /* where the stack is: 'i' internal RAM, 'p' PSRAM, 0 unknown */
	int core;            /* -1: either core */
	int prio;
	float cpu_pct;       /* % of one core over the sample; NaN: unknown */
} hifi_diag_task_t;

typedef struct {
	int n;
	uint32_t sample_ms; /* how long the CPU sample ran; 0: no CPU figures */
	float idle_pct[2];  /* each core's idle task, % of that core; NaN: unknown */
	hifi_diag_task_t t[HIFI_DIAG_TASKS_MAX];
} hifi_diag_tasks_t;

/*
 * CPU load: b's cpu_pct from the runtime each task gained since sample a
 * (matched by num; a task that started in between counts from 0), over
 * elapsed counter units per core. Fills idle_pct from the tasks named
 * IDLE*, by their core. Then sorts b busiest first.
 */
void hifi_diag_cpu(const hifi_diag_task_t *a, int na, hifi_diag_tasks_t *b, uint32_t elapsed);

/* The reset history, newest first, comma-separated, at most HIFI_DIAG_RESETS
 * reasons: adds reason at the front of hist (cap bytes). */
void hifi_diag_hist_push(char *hist, size_t cap, const char *reason);
/* A restart that looks like the power went: reason power_on or brownout on a
 * board whose counter says it had booted before (a first boot is a plug-in). */
bool hifi_diag_is_power_cut(const char *reason, uint32_t boots_before);
/* What esp_restart's wrapper kept (esp/hifi_crash.c) in words:
 * "<note> by <task> at 0x<pc> after <n> s"; with no note, "esp_restart by ...".
 * The address names the caller (addr2line on the build's ELF). */
#define HIFI_RESTART_CAUSE_CAP 80
void hifi_restart_cause_fmt(char *out, size_t cap, const char *note, const char *task, uint32_t pc,
			    uint32_t up_s);

/*
 * The output's underrun count, one step per output call (patch 0009, in
 * squeezelite's output.c). running: the output is playing; frames: what it
 * has to play now; live: a stream is still coming in; flushed: the buffer
 * was emptied on purpose since the last step (a seek). *state holds the
 * HIFI_UNDERRUN_ bits between steps, 0 at boot. Returns 1 when this step
 * starts a dry spell to count: the output is running, dry, the stream live,
 * and it has played since it last started or was flushed, so a start that
 * is still filling (Spotify's play comes before its data) or a seek isn't
 * one. One count per dry spell, not per call.
 */
#define HIFI_UNDERRUN_ARMED 1u
#define HIFI_UNDERRUN_DRY 2u
int hifi_diag_underrun_step(uint8_t *state, bool running, uint32_t frames, bool live, bool flushed);

typedef struct {
	uint32_t free, min, largest, total; /* bytes; total 0: none fitted */
} hifi_diag_heap_t;

typedef struct {
	const char *name; /* NULL: unused */
	uint32_t used, free, total, namespaces; /* entries */
} hifi_diag_nvs_t;

typedef struct {
	bool known;            /* false: no snapshot */
	const char *state;     /* the power state machine's */
	float vbus_v, vbus_i_a, pvdd_v; /* NaN: not measured */
	const char *proto;     /* the PD sink's contract */
	float contract_v, contract_a;
	bool pg;
	bool amp_on, amp_fault;
	int amp_faults;        /* since boot */
	float amp_temp_c;      /* NaN: no sensor wired */
} hifi_diag_power_t;

typedef struct {
	uint32_t uptime_s;
	/* ms from power-up: the hifi code started (hifi_init), the first IP, the
	 * remote link first up; 0: not yet */
	uint32_t boot_app_ms, boot_ip_ms, boot_link_ms;
	hifi_diag_heap_t internal, psram;
	const char *reset_reason;
	const char *restart_cause; /* who called esp_restart (hifi_crash_restart_cause); NULL: not a restart */
	char reset_hist[HIFI_DIAG_HIST_CAP];
	uint32_t boots; /* since the counter was first written */
	uint32_t power_cuts; /* restarts that looked like a power cut (hifi_diag_is_power_cut), kept in NVS */
	hifi_diag_nvs_t nvs[2];
	hifi_diag_power_t power; /* the API fills it from the snapshot */
} hifi_diag_sys_t;

typedef struct {
	int rssi;              /* dBm, 0: not connected */
	int channel;
	char bssid[18];
	const char *phy;       /* "11n", "11g", "11b" or NULL */
	uint32_t connects, disconnects;
	int last_reason;       /* the last disconnect's reason code; 0: none */
	uint32_t up_s;         /* since the last IP */
	/* the remote link (wss:// or MQTT) */
	bool link_up;
	uint32_t link_drops, link_up_s;
	uint64_t tx_bytes, rx_bytes;
	/* the last drop (wss:// only): a hifi_ws_drop_t (core/hifi_ws_proto.h;
	 * 0: none since boot) and its code, whether Wi-Fi was down then, how long
	 * ago and how long that link had been up */
	int link_last_drop, link_last_code;
	bool link_last_wifi_down;
	uint32_t link_last_ago_s, link_last_lasted_s;
	/* L4: the listener scan in normal running (core/hifi_listen.h) */
	int listen_strays;       /* -1: no verdict yet */
	uint32_t listen_events;  /* stray sets seen since boot */
	char listen_open[64];    /* "N open: tcp/23 ..."; "": none */
} hifi_diag_net_t;

typedef struct {
	bool known;            /* false: no audio stats in this build */
	uint32_t underruns;    /* times the output ran dry while playing */
	uint32_t rate;         /* Hz now at the output */
	uint32_t buffer_ms;    /* decoded audio waiting in the output buffer */
	uint32_t dma_ms;       /* in the I2S DMA: the pipeline's fixed latency */
	uint32_t played_s;     /* the output's total played, s since boot */
} hifi_diag_audio_t;

typedef struct {
	bool known;            /* false: no update since the counter was set */
	uint32_t bytes, total_ms, write_ms; /* write_ms: in flash writes */
	char version[41];
	uint32_t boot;         /* the boot count when it was written */
} hifi_diag_ota_t;

typedef struct {
	uint32_t bytes, ms;
} hifi_diag_echo_t;

#define HIFI_DIAG_ECHO_MAX (1024u * 1024u)
#define HIFI_DIAG_ECHO_DEFAULT (128u * 1024u)
#define HIFI_DIAG_ECHO_BUDGET_S 20

typedef struct {
	void (*sys)(hifi_diag_sys_t *out);
	/* fresh: take a new CPU sample (blocks about 1 s), else the last one.
	 * 0; -1 when there are no task stats; -2 when a sample failed (out of
	 * memory, or the second read): try again; the task count when there are
	 * more than HIFI_DIAG_TASKS_MAX */
	int (*tasks)(hifi_diag_tasks_t *out, bool fresh);
	void (*net)(hifi_diag_net_t *out);
	void (*audio)(hifi_diag_audio_t *out);
	void (*ota)(hifi_diag_ota_t *out);
	/* sends bytes of filler up the remote link and times it: 0, or an HTTP
	 * status with *err set. Stops after HIFI_DIAG_ECHO_BUDGET_S, with out
	 * holding what was sent by then. NULL: 404 */
	int (*echo)(uint32_t bytes, hifi_diag_echo_t *out, const char **err);
} hifi_diag_ops_t;

/* The writers: each returns 200 and a JSON object of at most HIFI_DIAG_PAGE
 * characters in out (cap >= HIFI_DIAG_PAGE + 1). */
void hifi_diag_write_sys(const hifi_diag_sys_t *s, char *out, size_t cap);
/* rows from `from` on, as many as fit; "next" is where the next page starts
 * (n when this one reaches the end) */
void hifi_diag_write_tasks(const hifi_diag_tasks_t *t, int from, char *out, size_t cap);
void hifi_diag_write_net(const hifi_diag_net_t *n, char *out, size_t cap);
void hifi_diag_write_audio(const hifi_diag_audio_t *a, char *out, size_t cap);
void hifi_diag_write_ota(const hifi_diag_ota_t *o, char *out, size_t cap);
void hifi_diag_write_echo(const hifi_diag_echo_t *e, char *out, size_t cap);
