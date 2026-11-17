#pragma once
#include <stddef.h>

/* why the board last restarted: power_on, restart, panic, task_watchdog,
 * interrupt_watchdog, watchdog, brownout, ... */
const char *hifi_crash_reset_reason(void);
/* the last boot's console tail (up to 3 KB) into out, NUL-terminated; its length */
size_t hifi_crash_last_log(char *out, size_t cap);
