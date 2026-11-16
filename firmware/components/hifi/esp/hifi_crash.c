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
 */
#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "hal/uart_hal.h"
#include "soc/uart_struct.h"
#include "hifi_crash.h"

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
