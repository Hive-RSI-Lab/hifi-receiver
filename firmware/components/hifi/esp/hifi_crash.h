#pragma once
#include <stddef.h>

/* why the board last restarted: power_on, restart, panic, task_watchdog,
 * interrupt_watchdog, watchdog, brownout, ... */
const char *hifi_crash_reset_reason(void);
/* the last boot's console tail (up to 3 KB) into out, NUL-terminated; its length */
size_t hifi_crash_last_log(char *out, size_t cap);
/* esp_restart's caller, for a "restart" reset: "<note> by <task> at 0x<pc>
 * after <n> s" (hifi_restart_cause_fmt); "unrecorded" when the restart left no
 * note. For another reset (a watchdog, a panic) that cut an esp_restart short,
 * the same plus how far it got: ", then cut short going quiet" (in
 * hifi_quiet_stop), "in shutdown" (a shutdown handler, such as Wi-Fi's or
 * Bluetooth's, hadn't returned), "after shutdown" (IDF's last steps, the
 * bootloader or early start-up) or "?" (no mark: the restart was by an image
 * without one, or RTC memory held garbage). Up to HIFI_RESTART_CAUSE_MAX - 1
 * chars; NULL when no restart was under way */
const char *hifi_crash_restart_cause(void);
/* why the next esp_restart happens, kept with it (up to 23 chars) */
void hifi_restart_note(const char *why);
