/*
 * The URL update's download with two buffers (hifi_net.c's download()): a
 * reader task fills one 4 KB buffer from the HTTP(S) stream while the update
 * task writes the other to flash, so the TLS read of the next chunk overlaps
 * the flash write of this one. It costs one more 4 KB buffer and the reader's
 * stack, during an update only.
 *
 * Weak in hifi_esp.h and pulled into the app only by patch 0036's -u:
 * recovery keeps hifi_net.c's one-buffer loop and doesn't grow.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hifi_esp.h"
#include "hifi_ota.h"

static const char *TAG = "hifi_dl";

#define DL_READER_STACK 5120 /* esp_http_client_read through mbedTLS */

typedef struct {
	int idx; /* which buffer */
	int len; /* bytes in it; short of what was asked: the stream ended or failed */
} dl_msg_t;

typedef struct {
	esp_http_client_handle_t h;
	size_t left;          /* bytes the reader still has to read */
	uint8_t *buf[2];
	QueueHandle_t empty;  /* buffer indexes, writer to reader */
	QueueHandle_t full;   /* dl_msg_t, reader to writer */
	SemaphoreHandle_t gone; /* given when the reader has ended */
	volatile bool stop;   /* the writer has given up: the reader ends */
} dl_t;

/* Only two buffers exist, so neither queue (depth 2) ever blocks a send. */
static void reader(void *arg) {
	dl_t *d = arg;
	int idx;
	while (d->left && xQueueReceive(d->empty, &idx, portMAX_DELAY) == pdTRUE && !d->stop) {
		int want = d->left < HIFI_OTA_CHUNK_MAX ? (int) d->left : HIFI_OTA_CHUNK_MAX;
		int got = 0, r;
		while (got < want && (r = esp_http_client_read(d->h, (char *) d->buf[idx] + got, want - got)) > 0) got += r;
		dl_msg_t m = {idx, got};
		xQueueSend(d->full, &m, portMAX_DELAY);
		if (got < want) break;
		d->left -= got;
	}
	xSemaphoreGive(d->gone);
	vTaskDelete(NULL);
}

int hifi_dl_pump(hifi_ota_t *o, esp_http_client_handle_t h, uint8_t *buf, int *progress, const char **err) {
	dl_t d = {.h = h, .left = o->size - o->written, .buf = {buf, malloc(HIFI_OTA_CHUNK_MAX)}};
	d.empty = xQueueCreate(2, sizeof(int));
	d.full = xQueueCreate(2, sizeof(dl_msg_t));
	d.gone = xSemaphoreCreateBinary();
	int st = -1; /* not started: the caller reads with one buffer */
	if (d.buf[1] && d.empty && d.full && d.gone) {
		for (int i = 0; i < 2; i++) xQueueSend(d.empty, &i, 0);
		if (xTaskCreate(reader, "hifi_dl", DL_READER_STACK, &d, ESP_TASK_PRIO_MIN + 2, NULL) == pdPASS) st = 0;
	}
	if (st) {
		ESP_LOGW(TAG, "no memory for the second buffer: reading with one");
	} else {
		int64_t t0 = esp_timer_get_time(), wait_us = 0, write_us = 0;
		size_t first = o->written;
		while (o->written < o->size) {
			int want = o->size - o->written < HIFI_OTA_CHUNK_MAX ? (int) (o->size - o->written) : HIFI_OTA_CHUNK_MAX;
			dl_msg_t m;
			int64_t t = esp_timer_get_time();
			xQueueReceive(d.full, &m, portMAX_DELAY); /* the reader always sends before it ends */
			wait_us += esp_timer_get_time() - t;
			if (m.len < want) {
				st = 502, *err = "the download stopped part way";
				break;
			}
			t = esp_timer_get_time();
			st = hifi_ota_chunk(o, o->written, d.buf[m.idx], m.len, err);
			write_us += esp_timer_get_time() - t;
			if (st != 200) break;
			st = 0;
			*progress = (int) (o->written * 100 / o->size);
			xQueueSend(d.empty, &m.idx, 0);
		}
		/* wake the reader if it waits for a buffer, then wait until it has
		 * ended: it uses the client and the buffers the caller frees */
		d.stop = true;
		int none = 0;
		xQueueSend(d.empty, &none, 0);
		xSemaphoreTake(d.gone, portMAX_DELAY);
		if (st) hifi_ota_abort(o);
		ESP_LOGI(TAG, "%u B in %lld ms: %lld ms writing (flash and hash), %lld ms waiting for the network",
			 (unsigned) (o->written - first), (esp_timer_get_time() - t0) / 1000, write_us / 1000,
			 wait_us / 1000);
	}
	if (d.gone) vSemaphoreDelete(d.gone);
	if (d.full) vQueueDelete(d.full);
	if (d.empty) vQueueDelete(d.empty);
	free(d.buf[1]);
	return st;
}
