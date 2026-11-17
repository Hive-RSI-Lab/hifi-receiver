/*
 * "Cannot open the home network" on the board: reads lwIP's TCP listeners and
 * bound UDP sockets (on the tcpip thread, where they may be read) and judges
 * them with core/hifi_listen.c. The relay task ticks it every 2 s; it scans
 * every 10 s. The OTA trial holds the image until two scans agree and rolls
 * it back on a stray; the self-test shows the newest verdict. In normal
 * running each new stray set is logged and counted in /api/diag's net page
 * (L4), so a listener that opens after the trial still shows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include "hifi_esp.h"
#include "hifi_listen.h"

static const char *TAG = "hifi_listen";

#define SCAN_EVERY_US (10 * 1000000LL)

typedef struct {
	struct tcpip_api_call_data call;
	hifi_sock_t *out;
	int max, n;
} walk_t;

static err_t walk(struct tcpip_api_call_data *c) {
	walk_t *w = (walk_t *) c;
	w->n = 0;
	for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p; p = p->next) {
		if (w->n >= w->max) goto full;
		w->out[w->n++] = (hifi_sock_t){.tcp = true, .port = p->local_port, .loopback = ip_addr_isloopback(&p->local_ip)};
	}
	for (struct udp_pcb *p = udp_pcbs; p; p = p->next) {
		if (!p->local_port) continue; /* not bound: can't receive */
		if (w->n >= w->max) goto full;
		w->out[w->n++] = (hifi_sock_t){.tcp = false, .port = p->local_port, .loopback = ip_addr_isloopback(&p->local_ip)};
	}
	return ERR_OK;
full:
	w->n = -1; /* more than we can hold: fail closed */
	return ERR_OK;
}

static int scan(hifi_sock_t *out, int max) {
	walk_t w = {.out = out, .max = max};
	if (tcpip_api_call(walk, &w.call) != ERR_OK) return -1;
	return w.n;
}

static bool ap_on(void) {
	wifi_mode_t m;
	return esp_wifi_get_mode(&m) == ESP_OK && (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
}

static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_buf;
/* in PSRAM (only tasks touch them): internal RAM is the lab image's limit */
EXT_RAM_ATTR static hifi_sock_t prev[HIFI_LISTEN_MAX], cur[HIFI_LISTEN_MAX];
static int nprev = -2; /* -2: no scan yet */
static bool prev_up;
static int verdict = -1; /* strays in the newest pair of scans; -1: none yet */
static bool verdict_up;  /* both scans of that pair had the network up */
EXT_RAM_ATTR static char verdict_why[64];
static int64_t last_scan;
static hifi_listen_report_t report = {.strays = -1};

void hifi_listen_tick(bool up) {
	int64_t now = esp_timer_get_time();
	if (!lock) lock = xSemaphoreCreateMutexStatic(&lock_buf);
	if (nprev != -2 && now - last_scan < SCAN_EVERY_US) return;
	last_scan = now;
	int n = scan(cur, HIFI_LISTEN_MAX);
	bool ap = ap_on();
	xSemaphoreTake(lock, portMAX_DELAY);
	if (nprev != -2 && prev_up == up) {
		char why[sizeof(verdict_why)];
		int v = hifi_listen_check(prev, nprev, cur, n, ap, why, sizeof(why));
		if (hifi_listen_report(&report, v, why))
			ESP_LOGE(TAG, "listening outside the allow list (stray set %u): %s", (unsigned) report.events, why);
		verdict = v;
		verdict_up = up;
		strcpy(verdict_why, why);
	}
	if (n >= 0) memcpy(prev, cur, n * sizeof(cur[0]));
	nprev = n;
	prev_up = up;
	xSemaphoreGive(lock);
}

void hifi_listen_trial(hifi_trial_t *t) {
	static char open[sizeof(verdict_why)];
	if (!lock) {
		t->listen_wait = true;
		return;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	t->listen_wait = verdict < 0 || (t->wifi_configured && !verdict_up);
	strcpy(open, verdict_why);
	t->listen_open = verdict > 0 ? open : NULL;
	xSemaphoreGive(lock);
}

int hifi_listen_now(char *why, size_t n) {
	hifi_sock_t *now = calloc(HIFI_LISTEN_MAX, sizeof(*now));
	if (!now) {
		snprintf(why, n, "no memory to scan");
		return 1;
	}
	int k = scan(now, HIFI_LISTEN_MAX);
	int v;
	if (lock) xSemaphoreTake(lock, portMAX_DELAY);
	/* against the last periodic scan, so a DNS query in flight doesn't count */
	if (lock && nprev != -2) v = hifi_listen_check(prev, nprev, now, k, ap_on(), why, n);
	else v = hifi_listen_check(NULL, 0, now, k, ap_on(), why, n);
	if (lock) xSemaphoreGive(lock);
	free(now);
	return v;
}

void hifi_listen_diag(hifi_diag_net_t *n) {
	if (!lock) {
		n->listen_strays = -1;
		return;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	n->listen_strays = report.strays;
	n->listen_events = report.events;
	snprintf(n->listen_open, sizeof(n->listen_open), "%s", report.open);
	xSemaphoreGive(lock);
}
