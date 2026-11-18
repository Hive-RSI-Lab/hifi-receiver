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
 *   when it passes, or when internal RAM falls under HIFI_LAB_HEAP_STOP;
 * - a run that crashes the board is named after the restart (the board
 *   keeps the running experiment's name, or the one that stopped in the
 *   last HIFI_LAB_RECENT_MS, where a restart doesn't clear it);
 * - a board that hangs comes back by itself: the lab build's task watchdog
 *   panics (board/lite-lab/sdkconfig.extra); a board that has been off the
 *   Hive's dial-in for HIFI_LAB_OFFLINE_MS after having been on it restarts
 *   (hifi_lab_link_tick); and so does one whose lab tick has been stuck for
 *   HIFI_LAB_STUCK_MS (an experiment's start or stop hung holding the lab's
 *   lock: nothing else notices that, the idle tasks still run). Both checks
 *   run on a timer of their own, not in the task that takes the lock. A
 *   hung Lite 2 once needed a hand on its power switch: the plain image's
 *   watchdog only logs.
 *
 * This is the logic, in plain C so the host tests run it; the board gives
 * each experiment's start, stop and status (esp/hifi_lab_esp.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_LAB_EXP_MAX 12 /* 8 base and Bluetooth ones, and room: a lab build with both BT modes registers 9, the lab-radio one 10 */
#define HIFI_LAB_MAX_S 3600       /* the longest run a command may ask for */
#define HIFI_LAB_DEFAULT_S 60
/* Internal RAM. The Lite runs with ~13 KB free (Wi-Fi, Bluetooth, player), so
 * an experiment keeps its buffers and stacks in PSRAM: a start needs FLOOR
 * free, and a run is stopped when it falls under STOP. */
#define HIFI_LAB_HEAP_FLOOR 10000
#define HIFI_LAB_HEAP_STOP 7000
/* a crash this soon after a run stopped is still put down to it (a stop
 * that leaves the radio or a driver in a bad state) */
#define HIFI_LAB_RECENT_MS 60000u
/* off the dial-in this long, after having been on it: the lab restarts the
 * board. Once per outage: after that restart the watch arms again only when
 * the board has been back on the dial-in, so a long home Wi-Fi outage costs
 * one restart, and a board that hangs again before it reconnects has only
 * the task watchdog and the stuck-tick check. */
#define HIFI_LAB_OFFLINE_MS (15u * 60u * 1000u)
/* the lab's own tick hasn't run this long (its lock held by a hung start or
 * stop): the lab restarts the board */
#define HIFI_LAB_STUCK_MS (3u * 60u * 1000u)

typedef struct hifi_lab_exp {
	const char *name; /* [a-z0-9_-], at most 15 characters */
	const char *help; /* one line: what it does and its parameters */
	/* run: this run's number, to give back when it ends itself (a finish
	 * from an earlier run is dropped). body: the whole request (a flat JSON
	 * object). 0, or an HTTP status with err filled. */
	int (*start)(uint32_t run, const char *body, char *err, size_t n);
	/* false: it didn't stop in time and is still winding down (the slot
	 * says "stuck"; the experiment refuses a start until it is down) */
	bool (*stop)(void);
	/* members of the run's result, written into an open JSON object with
	 * hj_* (hifi_json.h); may be called while it runs and after it stops */
	void (*result)(void *hj);
} hifi_lab_exp_t;

typedef struct {
	const hifi_lab_exp_t *exp;
	bool running;
	uint32_t started_ms, deadline_ms, stopped_ms;
	char stop_why[32]; /* "asked", "deadline", "heap", "done", "failed", "stuck" */
	uint32_t runs;
	bool recent; /* stopped less than HIFI_LAB_RECENT_MS ago (cleared by the tick) */
} hifi_lab_slot_t;

typedef struct {
	int n;
	hifi_lab_slot_t s[HIFI_LAB_EXP_MAX];
	char crashed_in[16]; /* the experiment running when the board last went down; "" none */
	bool offline_restart; /* the last restart was the lab's own, off the dial-in too long */
	bool stuck_restart;   /* the last restart was the lab's own, its tick stuck */
} hifi_lab_t;

/* the dial-in watch: hifi_lab_link_tick once a second */
typedef struct {
	bool seen;            /* on the dial-in at least once since boot */
	bool down;
	uint32_t down_since;
} hifi_lab_link_t;

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

/* An experiment that ended by itself (a ping run that sent its count). run:
 * the number its start was given; a finish of any other run is dropped (the
 * board hands finishes over late, and a new run may have started since). */
void hifi_lab_finished(hifi_lab_t *l, const char *name, uint32_t run, uint32_t now_ms, bool ok);

/* The name of a running experiment ("" none), for the board to keep across
 * a crash; and the one it kept, after the restart. */
const char *hifi_lab_running(const hifi_lab_t *l);
void hifi_lab_set_crashed(hifi_lab_t *l, const char *name);
/* The one to keep: the running experiment, else the one that stopped in the
 * last HIFI_LAB_RECENT_MS, else "". */
const char *hifi_lab_recent(const hifi_lab_t *l, uint32_t now_ms);

/* true when the board should restart: it has been off the dial-in for
 * HIFI_LAB_OFFLINE_MS after having been on it since boot. A board never on
 * it (no network set up, the setup AP) is left alone, and a dial-in that
 * isn't configured resets the watch (set again later, it starts afresh). */
bool hifi_lab_link_tick(hifi_lab_link_t *w, uint32_t now_ms, bool configured, bool connected);

/* "lab" self-test line: passes with experiments registered; names a running
 * one and the one a crash interrupted */
bool hifi_lab_selftest(const hifi_lab_t *l, char *value, size_t n);
