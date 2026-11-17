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
	char reset_hist[HIFI_DIAG_HIST_CAP];
	uint32_t boots; /* since the counter was first written */
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
} hifi_diag_net_t;

typedef struct {
	bool known;            /* false: no audio stats in this build */
	uint32_t underruns;    /* times the output ran dry while playing */
	uint32_t rate;         /* Hz now at the output */
	uint32_t buffer_ms;    /* decoded audio waiting in the output buffer */
	uint32_t dma_ms;       /* in the I2S DMA: the pipeline's fixed latency */
	uint32_t frames_ms;    /* the output's total played, ms since boot */
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

typedef struct {
	void (*sys)(hifi_diag_sys_t *out);
	/* fresh: take a new CPU sample (blocks about 1 s), else the last one.
	 * 0, or -1 when there are no task stats */
	int (*tasks)(hifi_diag_tasks_t *out, bool fresh);
	void (*net)(hifi_diag_net_t *out);
	void (*audio)(hifi_diag_audio_t *out);
	void (*ota)(hifi_diag_ota_t *out);
	/* sends bytes of filler up the remote link and times it: 0, or an HTTP
	 * status with *err set. NULL: 404 */
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
