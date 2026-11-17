/*
 * A new firmware image boots on trial (docs/remote.md, "Rollback"). This
 * judges the trial from what the board sees; the board marks the image good
 * (esp_ota_mark_app_valid_cancel_rollback) or bad and restarts into the
 * previous one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_TRIAL_LIMIT_S 120
/* internal RAM still free for a board to count as healthy (self-test and
 * trial). A live TLS link to the bench holds about 4 KB of it, so on a Lite
 * with Bluetooth on 20 KB failed a healthy board. */
#define HIFI_HEAP_MIN 12000

typedef struct {
	bool component_ok;    /* our init ran */
	bool wifi_configured, wifi_ok;
	bool relay_configured, relay_ok;
	bool i2c_ok;          /* the codec and the PD sink answer */
	bool heap_ok;
	/* the listener scan (hifi_listen.h): listen_wait while the two scans
	 * after the network came up aren't in; listen_open names what is open
	 * outside the allow list ("1 open: tcp/23"), NULL when nothing is */
	bool listen_wait;
	const char *listen_open;
	bool lab_on, lab_ok;   /* a lab build (CONFIG_HIFI_LAB), and its frame is up */
} hifi_trial_t;

typedef enum { HIFI_TRIAL_WAIT = 0, HIFI_TRIAL_GOOD, HIFI_TRIAL_BAD } hifi_trial_verdict_t;

/* why gets the first thing still missing (or why it failed) */
hifi_trial_verdict_t hifi_trial_judge(const hifi_trial_t *t, uint32_t elapsed_s, char *why, size_t n);

/* The boot heap: internal RAM free once the board has settled, the figure
 * that must stay well over HIFI_HEAP_MIN (16000 B is the aim, so one more
 * feature can't roll a board back). Settled means the network and the
 * bench link (when set up) are up and 30 s have passed, or 90 s in any
 * case. */
#define HIFI_HEAP_BOOT_AFTER_S 30
#define HIFI_HEAP_BOOT_LATEST_S 90
bool hifi_heap_boot_due(uint32_t up_s, bool net_up, bool relay_configured, bool relay_up);

/* the self-test's heap line into out (64 B is enough): free now, at boot
 * and the boot figure's margin over the floor. boot 0: not measured yet.
 * Returns the check's verdict, which is free now over the floor. */
bool hifi_heap_line(uint32_t now_free, uint32_t boot_free, char *out, size_t n);

/* A restart that something asks for while the image is on trial (upstream's
 * reboot on an IP change, patch 0030) waits until the trial is over. With
 * rollback on, any restart before the image is marked good puts the old one
 * back, so a router handing out a new address during the first two minutes
 * rolled every update back. Held, it runs once the image is marked good; a
 * bad image rolls back anyway, which is a restart too.
 * on_trial is kept in RAM, not read from otadata: marking the image good
 * erases and rewrites otadata, and a read in between says "not on trial".
 * The caller serialises both functions (hifi_relay.c: a critical section). */
typedef struct {
	bool on_trial; /* set at boot for an image waiting to be marked good */
	bool held;
	const char *why;
} hifi_restart_hold_t;

/* true: on trial, so the restart is held and the caller must not restart;
 * false: restart now. */
bool hifi_restart_hold(hifi_restart_hold_t *h, const char *why);
/* once the image has been marked good (and only then): the trial is over,
 * and true (with *why) when a held restart should run now. Once. */
bool hifi_restart_release(hifi_restart_hold_t *h, const char **why);
