/*
 * A stand-in for the party leader's worker (board/lite-party-standin, a
 * measurement image; never in a product image).
 *
 * It has the worker's shape as a7fcd's branch builds it (lite-party-mode,
 * 2026-11-17): a 6144 B stack in PSRAM, core 1, priority MIN+5, one UDP
 * socket, and AUDIO packets of 352 frames (8 ms, 1408 B of samples plus the
 * header and tag) sent in bursts of 8 every 64 ms, about 125 a second, with a
 * small clock packet every 200 ms. It sends to one follower
 * (CONFIG_HIFI_PARTY_STANDIN_PEER), so the leader's internal RAM pays what
 * the worker's sends cost: the TCB, the socket, lwIP's pbufs and Wi-Fi's TX
 * buffers. The payload is zeros; nothing on the follower reads it.
 *
 * It opens no listening port: the socket is connect()ed to the follower and
 * bound to an ephemeral port, so it takes datagrams only from that address.
 * It starts CONFIG_HIFI_PARTY_STANDIN_DELAY_S (120 s) after the station has an
 * address, so a Bluetooth source can connect first, and runs until the board
 * restarts. After 250 failed sends in a row (2 s of them) it stops, so
 * a stand-in missing from `api diag tasks` means its sends failed.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define STACK 6144
#define AUDIO_BYTES (1408 + 44) /* 352 frames of 16-bit stereo, the header and the tag */
#define CLOCK_BYTES 64
#define BURST 8                 /* AUDIO packets per pass, as the worker sends them */
#define PASS_MS 64              /* 8 packets of 8 ms */
#define CLOCK_EVERY 3           /* passes per clock packet: about every 200 ms */
#define PORT 4951
#define FAIL_STOP 250

static const char *TAG = "party_standin";

static bool have_ip(void) {
	esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip;
	return n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void standin_main(void *arg) {
	(void) arg;
	while (!have_ip()) vTaskDelay(pdMS_TO_TICKS(1000));
	vTaskDelay(pdMS_TO_TICKS(CONFIG_HIFI_PARTY_STANDIN_DELAY_S * 1000));
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(PORT)};
	int so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (so < 0 || inet_aton(CONFIG_HIFI_PARTY_STANDIN_PEER, &to.sin_addr) == 0 ||
	    connect(so, (struct sockaddr *) &to, sizeof(to))) {
		ESP_LOGE(TAG, "no socket to %s", CONFIG_HIFI_PARTY_STANDIN_PEER);
		if (so >= 0) close(so);
		vTaskDelete(NULL);
		return;
	}
	/* the packet in PSRAM, as the worker's buffers are */
	uint8_t *pkt = heap_caps_calloc(1, AUDIO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!pkt) {
		close(so);
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG, "sending %d B x %d every %d ms to %s:%d", AUDIO_BYTES, BURST, PASS_MS,
		 CONFIG_HIFI_PARTY_STANDIN_PEER, PORT);
	uint32_t seq = 0, sent = 0, failed_in_row = 0, passes = 0;
	TickType_t wake = xTaskGetTickCount();
	for (;;) {
		for (int i = 0; i < BURST; i++) {
			memcpy(pkt, &seq, sizeof(seq));
			seq++;
			if (send(so, pkt, AUDIO_BYTES, 0) == AUDIO_BYTES) {
				sent++;
				failed_in_row = 0;
			} else if (++failed_in_row >= FAIL_STOP) {
				ESP_LOGE(TAG, "%u sends failed in a row after %u sent: stopping", (unsigned) failed_in_row,
					 (unsigned) sent);
				free(pkt);
				close(so);
				vTaskDelete(NULL);
				return;
			}
		}
		if (++passes % CLOCK_EVERY == 0) send(so, pkt, CLOCK_BYTES, 0);
		if (passes % (60000 / PASS_MS) == 0) ESP_LOGI(TAG, "%u sent", (unsigned) sent);
		vTaskDelayUntil(&wake, pdMS_TO_TICKS(PASS_MS));
	}
}

void hifi_party_standin_start(void) {
	static StaticTask_t tcb; /* internal, as the worker's static TCB is */
	StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!stack || !xTaskCreateStaticPinnedToCore(standin_main, "party_standin", STACK, NULL, ESP_TASK_PRIO_MIN + 5,
						     stack, &tcb, 1))
		ESP_LOGE(TAG, "no task");
}
