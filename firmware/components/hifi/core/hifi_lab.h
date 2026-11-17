/*
 * The lab frame: experiments a lab build (board/lite-lab, CONFIG_HIFI_LAB)
 * carries beside the receiver, started and stopped by a signed command
 * (`api lab "{...}"` over the Hive's link, POST /api/v1/lab).
 *
 * What keeps a lab image safe on a board that is only reachable over Wi-Fi:
 * - nothing starts at boot: an image boots, joins Wi-Fi, reaches the Hive,
 *   passes its trial and self-test exactly as the plain image does, so a
 *   crash in an experiment brings the board back with nothing running;
 * - every run has a deadline (max_s, at most HIFI_LAB_MAX_S) and is stopped
 *   when it passes, or when internal RAM falls under HIFI_LAB_HEAP_FLOOR;
 * - a run that crashes the board is named after the restart (the board
 *   keeps the running experiment's name where a restart doesn't clear it).
 *
 * This is the logic, in plain C so the host tests run it; the board gives
 * each experiment's start, stop and status (esp/hifi_lab_esp.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_LAB_EXP_MAX 8
#define HIFI_LAB_MAX_S 3600       /* the longest run a command may ask for */
#define HIFI_LAB_DEFAULT_S 60
#define HIFI_LAB_HEAP_FLOOR 20000 /* internal RAM a run must leave free */

typedef struct hifi_lab_exp {
	const char *name; /* [a-z0-9_-], at most 15 characters */
	const char *help; /* one line: what it does and its parameters */
	/* body: the whole request (a flat JSON object). 0, or an HTTP status
	 * with err filled. A run that ends by itself calls hifi_lab_finished. */
	int (*start)(const char *body, char *err, size_t n);
	void (*stop)(void);
	/* members of the run's result, written into an open JSON object with
	 * hj_* (hifi_json.h); may be called while it runs and after it stops */
	void (*result)(void *hj);
} hifi_lab_exp_t;

typedef struct {
	const hifi_lab_exp_t *exp;
	bool running;
	uint32_t started_ms, deadline_ms, stopped_ms;
	char stop_why[32]; /* "asked", "deadline", "heap", "done", "failed" */
	uint32_t runs;
} hifi_lab_slot_t;

typedef struct {
	int n;
	hifi_lab_slot_t s[HIFI_LAB_EXP_MAX];
	char crashed_in[16]; /* the experiment running when the board last went down; "" none */
} hifi_lab_t;

void hifi_lab_init(hifi_lab_t *l);
/* false when full, the name is bad or taken */
bool hifi_lab_register(hifi_lab_t *l, const hifi_lab_exp_t *e);

/*
 * The API: GET lists the experiments, what runs and each one's last result;
 * POST takes {"op":"start","exp":NAME,"max_s":N,...the experiment's own...},
 * {"op":"stop","exp":NAME|"all"} or {"op":"result","exp":NAME}.
 * heap_free: internal RAM now (a start is refused under the floor).
 * Returns the HTTP status; the reply is in out.
 */
int hifi_lab_request(hifi_lab_t *l, bool post, const char *body, uint32_t now_ms, uint32_t heap_free,
		     char *out, size_t cap);

/* Call about once a second: stops runs past their deadline, and every run
 * when heap_free is under the floor. Returns how many it stopped. */
int hifi_lab_tick(hifi_lab_t *l, uint32_t now_ms, uint32_t heap_free);

/* An experiment that ended by itself (a ping run that sent its count) */
void hifi_lab_finished(hifi_lab_t *l, const char *name, uint32_t now_ms, bool ok);

/* The name of a running experiment ("" none), for the board to keep across
 * a crash; and the one it kept, after the restart. */
const char *hifi_lab_running(const hifi_lab_t *l);
void hifi_lab_set_crashed(hifi_lab_t *l, const char *name);

/* "lab" self-test line: passes with experiments registered; names a running
 * one and the one a crash interrupted */
bool hifi_lab_selftest(const hifi_lab_t *l, char *value, size_t n);
