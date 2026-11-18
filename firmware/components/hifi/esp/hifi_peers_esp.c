/*
 * GET /api/v1/peers on the board (core/hifi_peers.h keeps the table and
 * writes the reply). Each read collects what the last mDNS question heard
 * and, if none is running and the last began over HIFI_PEERS_ROUND_US ago,
 * asks again for _hifi._tcp: one PTR question, answered with SRV, TXT and A
 * by every speaker on the LAN. Nothing runs while nobody reads the list, the
 * question never leaves the LAN (mDNS is link-local multicast) and the
 * reply never waits on the radio.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"

#include "hifi_esp.h"
#include "hifi_peers.h"

static const char *TAG = "hifi_peers";

static EXT_RAM_ATTR hifi_peers_t table;
static mdns_search_once_t *search;
static long long round_us = -HIFI_PEERS_ROUND_US;
/* the API task and the console's "api peers" both read the list */
static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_buf;

void hifi_peers_init(void) {
	if (!lock) lock = xSemaphoreCreateMutexStatic(&lock_buf);
}

static void collect(mdns_result_t *res, long long now) {
	for (mdns_result_t *r = res; r; r = r->next) {
		if (!r->hostname) continue;
		hifi_peer_t p = {0};
		for (mdns_ip_addr_t *a = r->addr; a; a = a->next)
			if (a->addr.type == ESP_IPADDR_TYPE_V4) {
				snprintf(p.ip, sizeof(p.ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
				break;
			}
		snprintf(p.host, sizeof(p.host), "%s", r->hostname);
		for (size_t i = 0; i < r->txt_count; i++)
			hifi_peers_txt(&p, r->txt[i].key, r->txt[i].value,
				       r->txt_value_len ? r->txt_value_len[i] : (r->txt[i].value ? strlen(r->txt[i].value) : 0));
		hifi_peers_seen(&table, &p, now);
	}
}

int hifi_peers_op(char *out, size_t cap) {
	long long now = esp_timer_get_time();
	hifi_peer_t self = {0};
	hifi_info_t info = {0};
	hifi_ops.info(&info);
	hifi_net_ip(self.ip, sizeof(self.ip));
	snprintf(self.host, sizeof(self.host), "%s", info.hostname);
	snprintf(self.name, sizeof(self.name), "%s", info.name);
	snprintf(self.model, sizeof(self.model), "%s", info.model ? info.model : "");
	snprintf(self.fw, sizeof(self.fw), "%s", info.fw ? info.fw : "");
	self.seen_us = now;

	if (!lock) return snprintf(out, cap, "{\"error\":\"starting\"}"), 503;
	xSemaphoreTake(lock, portMAX_DELAY);
	hifi_net_lan(&table.net, &table.mask);
	if (search) {
		mdns_result_t *res = NULL;
		if (mdns_query_async_get_results(search, 0, &res)) {
			collect(res, now);
			if (res) mdns_query_results_free(res);
			mdns_query_async_delete(search);
			search = NULL;
		}
	}
	if (!search && self.ip[0] && now - round_us >= HIFI_PEERS_ROUND_US) {
		/* its own answer takes a result too */
		search = mdns_query_async_new(NULL, "_hifi", "_tcp", MDNS_TYPE_PTR, 3000, HIFI_PEERS_MAX + 1, NULL);
		if (search) round_us = now;
		else ESP_LOGW(TAG, "no mDNS question (mDNS not up?)");
	}
	hifi_peers_expire(&table, now);
	int rc = hifi_peers_json(&table, &self, search != NULL, now, out, cap);
	xSemaphoreGive(lock);
	return rc;
}
