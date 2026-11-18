/*
 * A stand-in for the party leader's worker (board/lite-party-standin, a
 * measurement image; never in a product image).
 *
 * It has the worker's shape as a7fcd's branch builds it (lite-party-mode,
 * 2026-11-17): a 6144 B stack in PSRAM, core 1, priority MIN+5, one UDP
 * socket, and AUDIO packets of 352 frames (8 ms, 1408 B of samples plus the
 * header and tag, 1454 B) sent in bursts of 8 every 64 ms, 125 a second, with
 * a 52 B clock packet every 192 ms. It sends to one follower
 * (CONFIG_HIFI_PARTY_STANDIN_PEER), so the leader's internal RAM pays what
 * the worker's sends cost: the TCB, the socket, lwIP's pbufs and Wi-Fi's TX
 * buffers. The payload is zeros; nothing on the follower reads it.
 *
 * It opens no listening port: the socket is connect()ed to the follower and
 * bound to an ephemeral port, so it takes datagrams only from that address.
 * It sends only when the follower is on the station's own subnet, and the
 * self-test carries a "party_standin" line saying it is a measurement image.
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
#include "esp_timer.h"
#include "lwip/sockets.h"

#if !CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
#error "the party stand-in's stack is in PSRAM: set CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY"
#endif

#define STACK 6144
#define AUDIO_BYTES (1408 + 46) /* 352 frames of 16-bit stereo, the header and the tag */
#define CLOCK_BYTES 52
#define BURST 8                 /* AUDIO packets per pass, as the worker sends them */
#define PASS_MS 64              /* 8 packets of 8 ms */
#define CLOCK_EVERY 3           /* passes per clock packet: every 192 ms */
#define PORT 4951
#define FAIL_STOP 250

static const char *TAG = "party_standin";

static bool have_ip(esp_netif_ip_info_t *ip) {
	esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	return n && esp_netif_get_ip_info(n, ip) == ESP_OK && ip->ip.addr != 0;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void standin_main(void *arg) {
	(void) arg;
	esp_netif_ip_info_t ip;
	while (!have_ip(&ip)) vTaskDelay(pdMS_TO_TICKS(1000));
	vTaskDelay(pdMS_TO_TICKS(CONFIG_HIFI_PARTY_STANDIN_DELAY_S * 1000));
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(PORT)};
	if (inet_aton(CONFIG_HIFI_PARTY_STANDIN_PEER, &to.sin_addr) == 0 || !have_ip(&ip) ||
	    (to.sin_addr.s_addr & ip.netmask.addr) != (ip.ip.addr & ip.netmask.addr)) {
		ESP_LOGE(TAG, "%s is not on this board's subnet: not sending", CONFIG_HIFI_PARTY_STANDIN_PEER);
		vTaskDelete(NULL);
		return;
	}
	int so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (so < 0 || connect(so, (struct sockaddr *) &to, sizeof(to))) {
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
	uint32_t seq = 0, sent = 0, failed = 0, failed_in_row = 0, passes = 0;
	/* paced in milliseconds, not ticks: at 100 Hz pdMS_TO_TICKS(64) is 60 ms */
	int64_t next = now_ms();
	for (;;) {
		for (int i = 0; i < BURST; i++) {
			memcpy(pkt, &seq, sizeof(seq));
			seq++;
			if (send(so, pkt, AUDIO_BYTES, 0) == AUDIO_BYTES) {
				sent++;
				failed_in_row = 0;
			} else {
				failed++;
				if (++failed_in_row >= FAIL_STOP) {
					ESP_LOGE(TAG, "%u sends failed in a row after %u sent: stopping",
						 (unsigned) failed_in_row, (unsigned) sent);
					free(pkt);
					close(so);
					vTaskDelete(NULL);
					return;
				}
			}
		}
		if (++passes % CLOCK_EVERY == 0) send(so, pkt, CLOCK_BYTES, 0);
		if (passes % (60000 / PASS_MS) == 0)
			ESP_LOGI(TAG, "%u sent, %u failed", (unsigned) sent, (unsigned) failed);
		next += PASS_MS;
		int64_t wait = next - now_ms();
		if (wait > 0) vTaskDelay((TickType_t) ((wait + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS));
		else if (wait < -1000) next = now_ms(); /* far behind (Wi-Fi stalled): don't burst to catch up */
	}
}

void hifi_party_standin_start(void) {
	static StaticTask_t tcb; /* internal, as the worker's static TCB is */
	StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!stack || !xTaskCreateStaticPinnedToCore(standin_main, "party_standin", STACK, NULL, ESP_TASK_PRIO_MIN + 5,
						     stack, &tcb, 1)) {
		ESP_LOGE(TAG, "no task");
		free(stack);
	}
}
