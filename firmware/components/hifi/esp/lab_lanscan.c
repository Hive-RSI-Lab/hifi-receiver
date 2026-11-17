/*
 * Lab experiment "lanscan": this board scans another of our boards' ports
 * from inside the home network (the tower can't reach the boards' LAN; they
 * only dial out). What a stranger on the home Wi-Fi would see, checked from
 * the network side instead of the target's own listener list.
 *
 *   api lab "{\"op\":\"start\",\"exp\":\"lanscan\",\"target\":\"192.168.2.222\",\"peer\":\"3076f52d73e0\",\"max_s\":600}"
 *   api lab "{\"op\":\"result\",\"exp\":\"lanscan\"}"
 *
 * target: the other board's IPv4 address; peer: its MAC (device id). The
 * scan refuses a target off this board's own /24 (or narrower) subnet, this
 * board, the gateway and the network's broadcast, and touches nothing until
 * ARP says the target's MAC is the peer's: an address that has moved to
 * someone else's device on a DHCP renewal is never scanned. Both checks run
 * again before every pass and every 10 s during one; a change ends the scan.
 * A run whose ports can't all be probed inside its max_s is refused.
 *
 * Optional: tcp (ports, default "1-65535"), udp (ports, default the common
 * ones below), rate (probes a second, 20-1000, default 300), listen_s
 * (0-300, default 20: how long to listen for its announcements at the end).
 *
 * TCP: a SYN to each port from a raw socket; a SYN-ACK is open, a RST
 * closed (this board's own TCP resets the SYN-ACK, so no connection is
 * made). UDP: a datagram to each port (a real query on the ports that have
 * one); an ICMP port unreachable is closed, a reply answered, nothing at
 * all "silent" (lwIP answers a closed port every time, so silent is open or
 * lost). Each port with no answer is tried once more. Then it listens:
 * every UDP datagram the target sends (to us, or to the mDNS, SSDP and
 * LLMNR groups, which it joins) is counted by its source port and where it
 * went. The result lists ports, not contents.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include "hifi_json.h"
#include "hifi_lab_esp.h"
#include "hifi_link.h"
#include "hifi_scan.h"

static const char *TAG = "lab_lanscan";

#define STACK 4096
#define UDP_DEFAULT "1-1024,1194,1434,1900,3283,3478,3483,3702,4500,5000-5010,5350,5351,5353,5355,6000-6010,7000-7011,9000,10000,11211,17000,49152-49170"

/* made once and kept (PSRAM): the worker, its stack, the port maps */
static TaskHandle_t worker;
static StaticTask_t worker_tcb;
static SemaphoreHandle_t arp_done;
static uint8_t *tcp_todo, *tcp_open, *tcp_closed, *udp_todo, *udp_closed, *udp_answered;

static volatile bool run, alive;
static uint32_t run_no;
static uint32_t self_ip, target_ip, rate, listen_s, tcp_n, udp_n;
static esp_netif_t *sta;
static struct netif *sta_nif;
static volatile bool moved;  /* the target or this board's address changed mid-scan */
static bool have_result; /* the maps hold the last run's ports and answers */
static uint8_t peer[6], seen_mac[6];
static bool mac_ok;
static uint16_t tcp_sport, udp_sport;
static uint32_t tcp_seq;
static int64_t t_start, t_end;
static const char *volatile phase = "idle";
EXT_RAM_ATTR static char error[48];
static volatile uint32_t sent, send_err, heap_min;
EXT_RAM_ATTR static hifi_scan_heard_t heard[HIFI_SCAN_HEARD_MAX];
static int heard_n;
static uint32_t heard_lost;
static portMUX_TYPE res_mux = portMUX_INITIALIZER_UNLOCKED; /* heard[] against result() */

static uint32_t heap_now(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

/* ------------------------------------------------------------ ARP */

EXT_RAM_ATTR static struct {
	struct netif *nif;
	ip4_addr_t ip;
	bool ask, found;
	uint8_t mac[6];
} arp;

static void arp_cb(void *a) { /* on the TCP/IP task */
	if (arp.ask) etharp_request(arp.nif, &arp.ip);
	struct eth_addr *e = NULL;
	const ip4_addr_t *ip = NULL;
	arp.found = etharp_find_addr(arp.nif, &arp.ip, &e, &ip) >= 0 && e;
	if (arp.found) memcpy(arp.mac, e->addr, 6);
	xSemaphoreGive(arp_done);
}

/* the target's MAC from ARP (asking for it: an ARP request touches no port) */
static bool arp_lookup(struct netif *nif) {
	arp.nif = nif;
	arp.ip.addr = htonl(target_ip);
	for (int i = 0; i < 30 && run; i++) { /* 3 s */
		arp.ask = i % 10 == 0;
		xSemaphoreTake(arp_done, 0);
		if (tcpip_callback(arp_cb, NULL) != ERR_OK || xSemaphoreTake(arp_done, pdMS_TO_TICKS(500)) != pdTRUE)
			return false;
		if (arp.found) {
			memcpy(seen_mac, arp.mac, 6);
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
	return false;
}

/* ------------------------------------------------------------ probes */

/* a DNS-shaped query for name (labels separated by '.') of type qtype */
static size_t dns_query(uint8_t *b, size_t cap, const char *name, uint16_t qtype) {
	if (cap < 12 + strlen(name) + 6) return 0;
	memset(b, 0, 12);
	b[0] = 0x4c;
	b[1] = 0x41; /* id */
	b[5] = 1;    /* one question */
	size_t n = 12;
	const char *s = name;
	while (*s) {
		const char *dot = strchr(s, '.');
		size_t l = dot ? (size_t) (dot - s) : strlen(s);
		b[n++] = (uint8_t) l;
		memcpy(b + n, s, l);
		n += l;
		s += l + (dot ? 1 : 0);
	}
	b[n++] = 0;
	b[n++] = (uint8_t) (qtype >> 8);
	b[n++] = (uint8_t) qtype;
	b[n++] = 0;
	b[n++] = 1; /* IN */
	return n;
}

/* what to send a UDP port: a real request where there is a well-known one */
static size_t udp_payload(uint16_t port, uint8_t *b, size_t cap) {
	switch (port) {
	case 53:
		return dns_query(b, cap, "hifi", 1);
	case 5353: /* legacy unicast (our port isn't 5353): answered to us */
		return dns_query(b, cap, "_services._dns-sd._udp.local", 12);
	case 5355:
		return dns_query(b, cap, "hifi", 1);
	case 123:
		if (cap < 48) return 0;
		memset(b, 0, 48);
		b[0] = 0x1b; /* NTP v3, client */
		return 48;
	case 1900: {
		static const char ms[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
					 "MX: 1\r\nST: ssdp:all\r\n\r\n";
		if (cap < sizeof(ms) - 1) return 0;
		memcpy(b, ms, sizeof(ms) - 1);
		return sizeof(ms) - 1;
	}
	default:
		return 0;
	}
}

/* ------------------------------------------------------------ the run */

typedef struct {
	int tcp, icmp, udp_raw, udp;
} socks_t;

static void heard_add(uint16_t sp, uint32_t dst, uint16_t dp) {
	portENTER_CRITICAL(&res_mux);
	if (!hifi_scan_heard_add(heard, &heard_n, sp, dst, dp)) heard_lost++;
	portEXIT_CRITICAL(&res_mux);
}

/* everything waiting on the raw sockets */
static void drain(const socks_t *s) {
	uint8_t b[96]; /* headers are all that's read: a longer datagram is cut */
	int r;
	uint16_t port;
	while ((r = recv(s->tcp, b, sizeof(b), MSG_DONTWAIT)) > 0) {
		int st = hifi_scan_tcp_reply(b, (size_t) r, target_ip, tcp_sport, tcp_seq, &port);
		if (st == HIFI_SCAN_OPEN) hifi_scan_set(tcp_open, port);
		else if (st == HIFI_SCAN_CLOSED) hifi_scan_set(tcp_closed, port);
	}
	while ((r = recv(s->icmp, b, sizeof(b), MSG_DONTWAIT)) > 0)
		if (hifi_scan_icmp_unreach(b, (size_t) r, target_ip, udp_sport, &port)) hifi_scan_set(udp_closed, port);
	while ((r = recv(s->udp_raw, b, sizeof(b), MSG_DONTWAIT)) > 0) {
		uint16_t sp, dp, len;
		uint32_t dst;
		if (!hifi_scan_udp_seen(b, (size_t) r, target_ip, &sp, &dst, &dp, &len)) continue;
		if (sp && dst == self_ip && dp == udp_sport) hifi_scan_set(udp_answered, sp);
		heard_add(sp, dst, dp);
	}
	/* the plain UDP socket's queue (the answers land there too): emptied */
	while (recv(s->udp, b, sizeof(b), MSG_DONTWAIT) > 0)
		;
}

/* wait up to ms, reading replies as they come */
static void wait_drain(const socks_t *s, uint32_t ms) {
	int64_t until = esp_timer_get_time() + (int64_t) ms * 1000;
	while (run && esp_timer_get_time() < until) {
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s->tcp, &rd);
		FD_SET(s->icmp, &rd);
		FD_SET(s->udp_raw, &rd);
		FD_SET(s->udp, &rd);
		int mx = s->tcp;
		if (s->icmp > mx) mx = s->icmp;
		if (s->udp_raw > mx) mx = s->udp_raw;
		if (s->udp > mx) mx = s->udp;
		struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
		select(mx + 1, &rd, NULL, NULL, &tv);
		drain(s);
		uint32_t h = heap_now();
		if (h < heap_min) heap_min = h;
	}
}

static bool fail(const char *why);

/* still the same target: this board's address and subnet as at the start,
 * the target still allowed on it, and ARP still giving the peer's MAC */
static bool same_target(void) {
	esp_netif_ip_info_t ip;
	char e[64];
	if (esp_netif_get_ip_info(sta, &ip) != ESP_OK || ntohl(ip.ip.addr) != self_ip ||
	    !hifi_scan_target_ok(self_ip, ntohl(ip.netmask.addr), ntohl(ip.gw.addr), target_ip, e, sizeof(e))) {
		moved = true;
		return fail("this board's address or subnet changed");
	}
	if (!arp_lookup(sta_nif)) {
		if (run) moved = true;
		return run ? fail("the target stopped answering ARP") : false;
	}
	if (memcmp(seen_mac, peer, 6)) {
		mac_ok = false;
		moved = true;
		return fail("the target's MAC changed");
	}
	return true;
}

#define RECHECK_US 10000000
#define SEND_ERR_MAX 20 /* failed sends in a row: the link is gone */

/* one pass over the ports in todo that have no answer yet in a or b;
 * false when the scan has to end */
static bool pass(const socks_t *s, bool tcp, const uint8_t *todo, const uint8_t *a, const uint8_t *b) {
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(target_ip)};
	int64_t t0 = esp_timer_get_time();
	uint32_t n = 0;
	uint8_t pkt[200];
	uint32_t errs = 0;
	int64_t checked = t0;
	uint16_t p = hifi_scan_next(todo, 1);
	while (run && p) {
		if (esp_timer_get_time() - checked > RECHECK_US) {
			if (!same_target()) return false;
			checked = esp_timer_get_time();
		}
		if (hifi_scan_bit(a, p) || hifi_scan_bit(b, p)) {
			p = p == 65535 ? 0 : hifi_scan_next(todo, (uint32_t) p + 1);
			continue;
		}
		/* paced: n probes are due rate * elapsed */
		int64_t due = (esp_timer_get_time() - t0) * rate / 1000000;
		if ((int64_t) n >= due) {
			wait_drain(s, 10);
			continue;
		}
		int r;
		if (tcp) {
			hifi_scan_syn(pkt, self_ip, target_ip, tcp_sport, p, tcp_seq);
			r = send(s->tcp, pkt, HIFI_SCAN_SYN_LEN, 0);
		} else {
			to.sin_port = htons(p);
			size_t len = udp_payload(p, pkt, sizeof(pkt));
			r = sendto(s->udp, pkt, len, 0, (struct sockaddr *) &to, sizeof(to));
		}
		if (r < 0) { /* out of buffers: back off and send this one again */
			send_err++;
			if (++errs >= SEND_ERR_MAX) return fail("sends keep failing (Wi-Fi down?)");
			wait_drain(s, 20);
			continue;
		}
		errs = 0;
		sent++;
		n++;
		p = p == 65535 ? 0 : hifi_scan_next(todo, (uint32_t) p + 1);
	}
	wait_drain(s, 1500); /* the last replies */
	return run;
}

/* Wi-Fi loses a few % of probes or replies at a time, so the silent ports
 * are tried again, up to PASSES_MAX times in all, until a pass gets no new
 * answer: a port that never answers costs only one more probe for each pass */
#define PASSES_MAX 4
static bool passes(const socks_t *s, bool tcp, const uint8_t *todo, const uint8_t *a, const uint8_t *b) {
	uint32_t got = hifi_scan_count(a) + hifi_scan_count(b);
	for (int i = 0; i < PASSES_MAX && run; i++) {
		if (!same_target() || !pass(s, tcp, todo, a, b)) return false;
		uint32_t now = hifi_scan_count(a) + hifi_scan_count(b);
		if (i && now == got) break;
		got = now;
	}
	return run;
}

static void join(int fd, uint32_t group) {
	struct ip_mreq m = {.imr_multiaddr.s_addr = htonl(group), .imr_interface.s_addr = htonl(self_ip)};
	setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
}

static bool fail(const char *why) {
	strlcpy(error, why, sizeof(error));
	return false;
}

static bool scan(void) {
	phase = "arp";
	if (!arp_lookup(sta_nif)) return fail(run ? "no ARP answer from the target" : "stopped");
	mac_ok = !memcmp(seen_mac, peer, 6);
	if (!mac_ok) return fail("the target's MAC isn't the peer's");

	socks_t s = {-1, -1, -1, -1};
	struct sockaddr_in t = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(target_ip)};
	s.tcp = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
	s.icmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	s.udp_raw = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
	s.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	bool ok = s.tcp >= 0 && s.icmp >= 0 && s.udp_raw >= 0 && s.udp >= 0;
	/* the raw sockets connected: they only see the target's packets */
	ok = ok && !connect(s.tcp, (struct sockaddr *) &t, sizeof(t)) &&
	     !connect(s.icmp, (struct sockaddr *) &t, sizeof(t)) && !connect(s.udp_raw, (struct sockaddr *) &t, sizeof(t));
	struct sockaddr_in me = {.sin_family = AF_INET}; /* an ephemeral port, inside the listener allow-list */
	socklen_t ml = sizeof(me);
	ok = ok && !bind(s.udp, (struct sockaddr *) &me, sizeof(me)) && !getsockname(s.udp, (struct sockaddr *) &me, &ml);
	if (!ok) {
		fail("no sockets (lwIP's are all in use?)");
	} else {
		udp_sport = ntohs(me.sin_port);
		join(s.udp, 0xe00000fbu); /* 224.0.0.251 mDNS */
		join(s.udp, 0xe00000fcu); /* 224.0.0.252 LLMNR */
		join(s.udp, 0xeffffffau); /* 239.255.255.250 SSDP */
		tcp_sport = (uint16_t) (40000 + esp_random() % 8000); /* under lwIP's own ephemeral range */
		tcp_seq = esp_random();
		phase = "tcp";
		ok = passes(&s, true, tcp_todo, tcp_open, tcp_closed);
		phase = "udp";
		ok = ok && passes(&s, false, udp_todo, udp_closed, udp_answered);
		phase = "listen";
		/* the discovery requests once more, then only listen */
		uint8_t pkt[200];
		const uint16_t ask[] = {5353, 1900, 5355};
		for (size_t i = 0; i < sizeof(ask) / sizeof(ask[0]) && run && ok; i++) {
			t.sin_port = htons(ask[i]);
			size_t len = udp_payload(ask[i], pkt, sizeof(pkt));
			if (sendto(s.udp, pkt, len, 0, (struct sockaddr *) &t, sizeof(t)) >= 0) sent++;
		}
		if (ok) wait_drain(&s, listen_s * 1000);
		ok = ok && same_target();
	}
	if (s.tcp >= 0) close(s.tcp);
	if (s.icmp >= 0) close(s.icmp);
	if (s.udp_raw >= 0) close(s.udp_raw);
	if (s.udp >= 0) close(s.udp); /* drops the group memberships too */
	return ok && run;
}

static void task_main(void *arg) {
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		uint32_t my_run = run_no;
		sta_nif = sta ? esp_netif_get_netif_impl(sta) : NULL;
		bool ok = sta_nif ? scan() : fail("no Wi-Fi station");
		t_end = esp_timer_get_time();
		bool ended_itself = run;
		phase = ok ? "done" : run ? "failed" : "stopped";
		run = false;
		if (ended_itself) hifi_lab_esp_finished("lanscan", my_run, ok);
		ESP_LOGI(TAG, "%s: %u probes sent, %u TCP open", phase, (unsigned) sent, (unsigned) hifi_scan_count(tcp_open));
		alive = false;
	}
}

static uint32_t num(const char *body, const char *key, uint32_t dflt) {
	double v;
	if (!hj_get_num(body, key, &v) || !(v >= 0)) return dflt;
	return v >= 4294967295.0 ? UINT32_MAX : (uint32_t) v;
}

static int start(uint32_t run_n, const char *body, char *err, size_t n) {
	if (alive) {
		snprintf(err, n, "the last run is still stopping");
		return 409;
	}
	char s[24];
	uint32_t tip;
	uint8_t pmac[6];
	if (!hj_get_str(body, "target", s, sizeof(s)) || !hifi_scan_parse_ip(s, &tip)) {
		snprintf(err, n, "target (the other board's IPv4 address) is required");
		return 400;
	}
	if (!hj_get_str(body, "peer", s, sizeof(s)) || !hifi_link_parse_mac(s, pmac)) {
		snprintf(err, n, "peer (the target board's MAC or device id) is required");
		return 400;
	}
	uint32_t rt = num(body, "rate", 300), ls = num(body, "listen_s", 20);
	if (rt < 20 || rt > 1000 || ls > 300) {
		snprintf(err, n, "rate 20-1000, listen_s 0-300");
		return 400;
	}
	esp_netif_t *ni = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip;
	if (!ni || esp_netif_get_ip_info(ni, &ip) != ESP_OK || !ip.ip.addr) {
		snprintf(err, n, "the Wi-Fi station has no address");
		return 503;
	}
	if (!hifi_scan_target_ok(ntohl(ip.ip.addr), ntohl(ip.netmask.addr), ntohl(ip.gw.addr), tip, err, n)) return 400;

	if (!worker) {
		/* the stack in PSRAM only where the build allows it (the task never
		 * touches flash), else internal */
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_SPIRAM);
#else
		StackType_t *stack = heap_caps_malloc(STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
		uint8_t *maps = heap_caps_calloc(6, HIFI_SCAN_BITS, MALLOC_CAP_SPIRAM);
		if (!arp_done) arp_done = xSemaphoreCreateBinary();
		if (!stack || !maps || !arp_done) {
			free(stack);
			free(maps);
			snprintf(err, n, "out of memory");
			return 503;
		}
		tcp_todo = maps;
		tcp_open = maps + HIFI_SCAN_BITS;
		tcp_closed = maps + 2 * HIFI_SCAN_BITS;
		udp_todo = maps + 3 * HIFI_SCAN_BITS;
		udp_closed = maps + 4 * HIFI_SCAN_BITS;
		udp_answered = maps + 5 * HIFI_SCAN_BITS;
		worker = xTaskCreateStaticPinnedToCore(task_main, "lab_lanscan", STACK, NULL, ESP_TASK_PRIO_MIN + 4, stack,
						       &worker_tcb, 0);
		if (!worker) {
			free(stack);
			free(maps);
			tcp_todo = NULL;
			snprintf(err, n, "no worker task");
			return 503;
		}
	}
	/* the worker is idle (alive is false); result() reads the maps, and from
	 * here on they no longer match the last run's answers, so it shows
	 * nothing until a run starts */
	have_result = false;
	char spec[160];
	uint32_t tn, un;
	if (!hj_get_str(body, "tcp", spec, sizeof(spec))) strlcpy(spec, "1-65535", sizeof(spec));
	if (!hifi_scan_ports(spec, tcp_todo, &tn)) {
		snprintf(err, n, "tcp: ports like 1-1024,5000");
		return 400;
	}
	if (!hj_get_str(body, "udp", spec, sizeof(spec))) strlcpy(spec, UDP_DEFAULT, sizeof(spec));
	if (!hifi_scan_ports(spec, udp_todo, &un)) {
		snprintf(err, n, "udp: ports like 1-1024,5353");
		return 400;
	}
	/* the first pass with 10 % for pacing and losses, 30 s for the ARP
	 * checks and the 1.5 s after each pass, and the listening: a run its
	 * deadline would cut short is refused */
	double max_s = HIFI_LAB_DEFAULT_S;
	hj_get_num(body, "max_s", &max_s);
	uint32_t need = (uint32_t) ((uint64_t) (tn + un) * 11 / (rt * 10)) + 30 + ls;
	if (need > max_s) {
		snprintf(err, n, "max_s too short: these ports at %u/s need at least %u s", (unsigned) rt, (unsigned) need);
		return 400;
	}
	target_ip = tip;
	memcpy(peer, pmac, 6);
	rate = rt;
	listen_s = ls;
	tcp_n = tn;
	udp_n = un;
	self_ip = ntohl(ip.ip.addr);
	sta = ni;
	moved = false;
	memset(tcp_open, 0, HIFI_SCAN_BITS);
	memset(tcp_closed, 0, HIFI_SCAN_BITS);
	memset(udp_closed, 0, HIFI_SCAN_BITS);
	memset(udp_answered, 0, HIFI_SCAN_BITS);
	portENTER_CRITICAL(&res_mux);
	heard_n = 0;
	heard_lost = 0;
	portEXIT_CRITICAL(&res_mux);
	sent = send_err = 0;
	heap_min = heap_now();
	error[0] = 0;
	mac_ok = false;
	memset(seen_mac, 0, 6);
	t_start = esp_timer_get_time();
	t_end = 0;
	phase = "starting";
	run_no = run_n;
	have_result = true;
	run = alive = true;
	xTaskNotifyGive(worker);
	char ts[16];
	hifi_scan_ip_str(target_ip, ts, sizeof(ts));
	ESP_LOGI(TAG, "scanning %s: %u TCP, %u UDP ports at %u/s", ts, (unsigned) tcp_n, (unsigned) udp_n, (unsigned) rate);
	return 0;
}

static bool stop(void) {
	run = false;
	for (int i = 0; i < 300 && alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
	if (alive) ESP_LOGE(TAG, "the task didn't stop in 3 s");
	return !alive;
}

/* ports in todo with no answer in a or b, into out */
static void silent(uint8_t *out, const uint8_t *todo, const uint8_t *a, const uint8_t *b) {
	for (int i = 0; i < HIFI_SCAN_BITS; i++) out[i] = todo[i] & (uint8_t) ~(a[i] | b[i]);
}

static void result(void *hj) {
	hj_t *j = hj;
	if (!tcp_todo || !have_result) return;
	char s[40];
	hifi_scan_ip_str(target_ip, s, sizeof(s));
	hj_str(j, "target", s);
	hj_str(j, "phase", phase);
	if (error[0]) hj_str(j, "error", error);
	if (moved) hj_bool(j, "moved", true);
	if (!mac_ok && seen_mac[0] | seen_mac[1] | seen_mac[2]) {
		snprintf(s, sizeof(s), "%02x%02x%02x%02x%02x%02x", seen_mac[0], seen_mac[1], seen_mac[2], seen_mac[3],
			 seen_mac[4], seen_mac[5]);
		hj_str(j, "target_mac", s);
	}
	hj_int(j, "rate", rate);
	hj_int(j, "s", ((t_end ? t_end : esp_timer_get_time()) - t_start) / 1000000);
	hj_int(j, "sent", sent);
	if (send_err) hj_int(j, "send_err", send_err);
	hj_int(j, "heap_min", heap_min);
	/* a scratch map in PSRAM for the silent ports, made when first asked */
	static uint8_t *quiet;
	if (!quiet) quiet = heap_caps_malloc(HIFI_SCAN_BITS, MALLOC_CAP_SPIRAM);
	hj_obj(j, "tcp");
	hj_int(j, "ports", tcp_n);
	hifi_scan_ports_json(j, "open", tcp_open, 16);
	hj_int(j, "closed", hifi_scan_count(tcp_closed));
	if (quiet) {
		silent(quiet, tcp_todo, tcp_open, tcp_closed);
		hifi_scan_ports_json(j, "silent", quiet, 24);
	}
	hj_obj_end(j);
	hj_obj(j, "udp");
	hj_int(j, "ports", udp_n);
	hifi_scan_ports_json(j, "answered", udp_answered, 8);
	hj_int(j, "closed", hifi_scan_count(udp_closed));
	if (quiet) {
		silent(quiet, udp_todo, udp_closed, udp_answered);
		hifi_scan_ports_json(j, "silent", quiet, 16);
	}
	hj_obj_end(j);
	hifi_scan_heard_t h[HIFI_SCAN_HEARD_MAX];
	int hn;
	uint32_t lost;
	portENTER_CRITICAL(&res_mux);
	hn = heard_n;
	lost = heard_lost;
	memcpy(h, heard, sizeof(h));
	portEXIT_CRITICAL(&res_mux);
	hj_arr(j, "heard");
	for (int i = 0; i < hn; i++) {
		hifi_scan_heard_str(&h[i], s, sizeof(s));
		hj_str(j, NULL, s);
	}
	hj_arr_end(j);
	if (lost) hj_int(j, "heard_lost", lost);
}

const hifi_lab_exp_t lab_lanscan = {
	.name = "lanscan",
	.help = "scan another of our boards from the LAN: target (IPv4), peer (its MAC), tcp, udp, rate, listen_s",
	.start = start,
	.stop = stop,
	.result = result,
};
