/*
 * What the board printed just before it last restarted, kept across the
 * restart so it can be read over Wi-Fi (GET /api/v1/crash) with no USB cable:
 * the console's last few KB, panic and backtrace included.
 *
 * Every byte the console UART sends passes uart_hal_write_txfifo (the UART
 * driver's ISR and the panic handler both write through it), so the link
 * wraps it (-Wl,--wrap, CMakeLists.txt) and copies each byte into a ring in
 * RTC slow memory, which a software restart, a panic or a watchdog reset
 * leaves alone. Two rings: at boot the one being written becomes "the last
 * boot's" and the other is cleared for this boot.
 *
 * Who restarted it: every software restart goes through esp_restart, which
 * the link wraps too (__wrap_esp_restart) to note in RTC memory the caller's
 * address, its task, the uptime and, when the path set one first
 * (hifi_restart_note), why. The next boot reads it back
 * (hifi_crash_restart_cause), so a clean restart names its own cause.
 * A restart that ends in some other reset (the RTC watchdog, a panic) still
 * names its caller, and how far the way down got: the wrapper marks the
 * start and the end of going quiet, and a wrap of esp_restart_noos (the step
 * after every shutdown handler has returned) marks the rest.
 */
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_private/system_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_hal.h"
#include "soc/uart_struct.h"
#include "hifi_crash.h"
#include "hifi_diag.h"
#include "hifi_esp.h"

#define RING 3072
#define MAGIC 0x48694672u /* "HiFr" */

typedef struct {
	uint32_t magic;
	uint32_t head; /* next byte written */
	uint32_t len;  /* bytes held, up to RING */
	char buf[RING];
} ring_t;

static RTC_NOINIT_ATTR ring_t rings[2];
static RTC_NOINIT_ATTR uint32_t cur_magic, cur; /* which ring this boot writes */

void __real_uart_hal_write_txfifo(uart_hal_context_t *hal, const uint8_t *buf, uint32_t data_size,
				  uint32_t *write_size);

void IRAM_ATTR __wrap_uart_hal_write_txfifo(uart_hal_context_t *hal, const uint8_t *buf,
					    uint32_t data_size, uint32_t *write_size) {
	__real_uart_hal_write_txfifo(hal, buf, data_size, write_size);
	if (hal->dev != &UART0 || cur_magic != MAGIC || cur > 1) return;
	ring_t *r = &rings[cur];
	if (r->magic != MAGIC) return;
	uint32_t n = *write_size, h = r->head % RING;
	for (uint32_t i = 0; i < n; i++) {
		r->buf[h] = (char) buf[i];
		h = h + 1 == RING ? 0 : h + 1;
	}
	r->head = h;
	r->len = r->len + n > RING ? RING : r->len + n;
}

/* esp_restart's caller, kept across the restart it asks for */
#define RESTART_MAGIC 0x52737443u /* "RstC" */
typedef struct {
	uint32_t magic;
	uint32_t pc;   /* the call's return address */
	uint32_t up_s; /* uptime then */
	char task[16];
	char note[24];
} restart_rec_t;

static RTC_NOINIT_ATTR restart_rec_t restart_rec;
static restart_rec_t restart_last; /* the last boot's, moved out at boot */
static char restart_note[24];

/* how far the last esp_restart got: DOWN_CALLED as the wrapper starts going
 * quiet, DOWN_QUIET once it has and the shutdown handlers (Wi-Fi, Bluetooth,
 * esp_timer, ...) run, DOWN_HANDLERS once they have all returned */
#define DOWN_CALLED 0x44776e31u   /* "Dwn1" */
#define DOWN_QUIET 0x44776e32u    /* "Dwn2" */
#define DOWN_HANDLERS 0x44776e33u /* "Dwn3" */
static RTC_NOINIT_ATTR uint32_t down_mark;
static uint32_t down_last;

void hifi_restart_note(const char *why) {
	snprintf(restart_note, sizeof(restart_note), "%s", why ? why : "");
}

void __real_esp_restart(void);

void __wrap_esp_restart(void) {
	uint32_t ra = (uint32_t) __builtin_return_address(0);
	restart_rec_t r = {.magic = RESTART_MAGIC};
	/* windowed Xtensa keeps the window size in the top two bits; the call is
	 * 3 bytes before the return address */
	r.pc = ((ra & 0x3fffffffu) | 0x40000000u) - 3;
	r.up_s = (uint32_t) (esp_timer_get_time() / 1000000);
	const char *t = xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED ? NULL : pcTaskGetTaskName(NULL);
	snprintf(r.task, sizeof(r.task), "%s", t ? t : "?");
	memcpy(r.note, restart_note, sizeof(r.note));
	r.note[sizeof(r.note) - 1] = '\0';
	restart_rec = r;
	down_mark = DOWN_CALLED;
	/* every planned restart, updates and rollbacks included, goes quiet
	 * first: no pop or burst reaches a speaker (hifi_quiet.h) */
	hifi_quiet_stop(r.note[0] ? r.note : "restart", true);
	down_mark = DOWN_QUIET;
	__real_esp_restart();
}

/* esp_restart calls this once its shutdown handlers have returned; a panic,
 * a watchdog's panic and the brownout handler call it too, with a reset hint
 * set first, so only a call with none moves the mark on (IRAM: the panic
 * path runs with the flash cache off) */
void __real_esp_restart_noos(void) __attribute__((noreturn));

void IRAM_ATTR __wrap_esp_restart_noos(void) {
	if (down_mark == DOWN_QUIET && esp_reset_reason_get_hint() == ESP_RST_UNKNOWN) down_mark = DOWN_HANDLERS;
	__real_esp_restart_noos();
}

/* before app_main and before most of the boot log: swap the rings */
static void __attribute__((constructor)) crash_boot(void) {
	if (cur_magic != MAGIC || cur > 1) {
		/* power-on: nothing kept */
		memset(rings, 0, sizeof(rings));
		cur = 0;
	} else {
		cur ^= 1;
	}
	rings[cur].head = rings[cur].len = 0;
	rings[cur].magic = MAGIC;
	cur_magic = MAGIC;
	/* the last restart's note, if it left one; then clear it so a later
	 * reset that isn't esp_restart (a panic, a watchdog) can't inherit it */
	if (restart_rec.magic == RESTART_MAGIC) {
		restart_last = restart_rec;
		/* RTC garbage that happens to match the magic stays bounded */
		restart_last.task[sizeof(restart_last.task) - 1] = '\0';
		restart_last.note[sizeof(restart_last.note) - 1] = '\0';
	}
	memset(&restart_rec, 0, sizeof(restart_rec));
	down_last = down_mark;
	down_mark = 0;
}

static const char *reason_name(esp_reset_reason_t r) {
	switch (r) {
	case ESP_RST_POWERON: return "power_on";
	case ESP_RST_EXT: return "external";
	case ESP_RST_SW: return "restart";
	case ESP_RST_PANIC: return "panic";
	case ESP_RST_INT_WDT: return "interrupt_watchdog";
	case ESP_RST_TASK_WDT: return "task_watchdog";
	case ESP_RST_WDT: return "watchdog";
	case ESP_RST_DEEPSLEEP: return "deep_sleep";
	case ESP_RST_BROWNOUT: return "brownout";
	case ESP_RST_SDIO: return "sdio";
	default: return "unknown";
	}
}

const char *hifi_crash_reset_reason(void) {
	return reason_name(esp_reset_reason());
}

size_t hifi_crash_last_log(char *out, size_t cap) {
	if (!cap) return 0;
	out[0] = '\0';
	if (cur_magic != MAGIC || cur > 1) return 0;
	const ring_t *r = &rings[cur ^ 1];
	if (r->magic != MAGIC || !r->len) return 0;
	size_t n = r->len < cap - 1 ? r->len : cap - 1;
	size_t start = (r->head + RING - n) % RING;
	for (size_t i = 0; i < n; i++) out[i] = r->buf[(start + i) % RING];
	out[n] = '\0';
	return n;
}

const char *hifi_crash_restart_cause(void) {
	static char out[HIFI_RESTART_CAUSE_MAX];
	esp_reset_reason_t why = esp_reset_reason();
	const restart_rec_t *r = &restart_last;
	if (why == ESP_RST_SW) {
		if (r->magic != RESTART_MAGIC) return "unrecorded";
		hifi_restart_cause_fmt(out, sizeof(out), r->note, r->task, r->pc, r->up_s);
		return out;
	}
	/* another reset cut a restart short: say where it was on the way down */
	if (why == ESP_RST_POWERON || r->magic != RESTART_MAGIC) return NULL;
	hifi_restart_cause_fmt(out, sizeof(out), r->note, r->task, r->pc, r->up_s);
	size_t n = strlen(out);
	snprintf(out + n, sizeof(out) - n, ", then %s",
		 down_last == DOWN_CALLED     ? "cut short going quiet"
		 : down_last == DOWN_QUIET    ? "cut short in shutdown"
		 : down_last == DOWN_HANDLERS ? "cut short after shutdown"
					      : "?");
	return out;
}
