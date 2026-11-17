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
