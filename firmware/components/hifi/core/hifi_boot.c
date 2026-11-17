#include <stdio.h>

#include "hifi_boot.h"

hifi_trial_verdict_t hifi_trial_judge(const hifi_trial_t *t, uint32_t elapsed_s, char *why, size_t n) {
	const char *missing = NULL;
	char open[64];
	if (!t->component_ok) {
		snprintf(why, n, "the hifi component didn't start");
		return HIFI_TRIAL_BAD; /* it won't start later either */
	}
	if (!t->i2c_ok) missing = "codec or PD sink not answering on I2C";
	else if (!t->heap_ok) missing = "heap below 12 KB";
	else if (t->lab_on && !t->lab_ok) missing = "the lab frame isn't up";
	else if (t->wifi_configured && !t->wifi_ok) missing = "Wi-Fi not joined";
	else if (t->relay_configured && !t->relay_ok) missing = "relay not reached";
	else if (t->listen_open) {
		snprintf(open, sizeof(open), "listening outside the allow list (%s)", t->listen_open);
		missing = open;
	} else if (t->listen_wait) missing = "listener scan not done";
	if (!missing) {
		snprintf(why, n, "all checks passed in %u s", (unsigned) elapsed_s);
		return HIFI_TRIAL_GOOD;
	}
	if (elapsed_s >= HIFI_TRIAL_LIMIT_S) {
		snprintf(why, n, "%s after %u s", missing, (unsigned) elapsed_s);
		return HIFI_TRIAL_BAD;
	}
	snprintf(why, n, "%s", missing);
	return HIFI_TRIAL_WAIT;
}

bool hifi_heap_boot_due(uint32_t up_s, bool net_up, bool relay_configured, bool relay_up) {
	if (up_s >= HIFI_HEAP_BOOT_LATEST_S) return true;
	return up_s >= HIFI_HEAP_BOOT_AFTER_S && net_up && (!relay_configured || relay_up);
}

bool hifi_heap_line(uint32_t now_free, uint32_t boot_free, char *out, size_t n) {
	if (boot_free)
		snprintf(out, n, "%u B free, %u B at boot, %u %s the %u floor", (unsigned) now_free, (unsigned) boot_free,
			 (unsigned) (boot_free >= HIFI_HEAP_MIN ? boot_free - HIFI_HEAP_MIN : HIFI_HEAP_MIN - boot_free),
			 boot_free >= HIFI_HEAP_MIN ? "over" : "under", (unsigned) HIFI_HEAP_MIN);
	else
		snprintf(out, n, "%u B free, boot not measured yet (floor %u)", (unsigned) now_free,
			 (unsigned) HIFI_HEAP_MIN);
	return now_free > HIFI_HEAP_MIN;
}

bool hifi_restart_hold(hifi_restart_hold_t *h, const char *why) {
	if (!h->on_trial) return false;
	h->why = why ? why : "held";
	h->held = true;
	return true;
}

bool hifi_restart_release(hifi_restart_hold_t *h, const char **why) {
	h->on_trial = false;
	if (!h->held) return false;
	h->held = false;
	if (why) *why = h->why;
	return true;
}
