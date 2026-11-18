/* Regression for patch 0021: AirPlay RTP is taken only from the session's
 * sender. Before it, rtp.c fed the decoder (and the clock) from any host on
 * the LAN that sent to its UDP ports. Builds upstream rtp.c's own
 * rtp_take_from and rtp_check_stalled, as patched, against a stub of its
 * context: packets from the RTSP peer are kept, packets from anyone else are
 * dropped and counted, and a flood from someone else neither hides a stalled
 * sender nor makes a stall out of a live one. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "../check.h"

typedef uint32_t u32_t;
typedef enum { RAOP_PLAY, RAOP_STALLED } raop_event_t;

static int n_warn, n_stalled;
#define LOG_WARN(...) ((void) n_warn++)

static u32_t now_ms;
static u32_t gettime_ms(void) { return now_ms; }

static bool cmd_cb(raop_event_t ev, ...) {
	if (ev == RAOP_STALLED) n_stalled++;
	return true;
}

/* the fields of rtp.c's struct rtp_s these functions use */
typedef struct rtp_s {
	struct in_addr host;
	struct sockaddr_in rtp_host;
	u32_t foreign_packets;
	u32_t last_rx;
	bool stall_sent;
	bool (*cmd_cb)(raop_event_t event, ...);
} rtp_t;

#include "rtp_fns.c"

static struct sockaddr_in from(const char *ip, unsigned short port) {
	struct sockaddr_in a = { 0 };
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = inet_addr(ip);
	a.sin_port = htons(port);
	return a;
}

/* what the receive loop does with one packet that arrived at now_ms */
static bool receive(rtp_t *ctx, struct sockaddr_in a) {
	if (!rtp_take_from(ctx, &a)) {
		rtp_check_stalled(ctx);
		return false;
	}
	ctx->last_rx = now_ms;
	ctx->stall_sent = false;
	return true;
}

static rtp_t fresh(const char *host) {
	rtp_t ctx = { 0 };
	ctx.host.s_addr = host ? inet_addr(host) : INADDR_ANY;
	ctx.rtp_host.sin_family = AF_INET;
	ctx.rtp_host.sin_addr.s_addr = INADDR_ANY;
	ctx.cmd_cb = cmd_cb;
	ctx.last_rx = now_ms;
	return ctx;
}

int main(void) {
	now_ms = 1000;

	/* the session's sender: kept, and its address and port remembered */
	rtp_t ctx = fresh("192.168.2.10");
	CHECK(receive(&ctx, from("192.168.2.10", 6001)));
	CHECK(ctx.rtp_host.sin_addr.s_addr == inet_addr("192.168.2.10"));
	CHECK(ctx.rtp_host.sin_port == htons(6001));
	CHECK(ctx.rtp_host.sin_family == AF_INET);
	CHECK(ctx.foreign_packets == 0);

	/* a resend from the same sender on another port (control) is kept too */
	CHECK(receive(&ctx, from("192.168.2.10", 6002)));
	CHECK(ctx.rtp_host.sin_port == htons(6002));

	/* anyone else: dropped, counted, warned once per 256, rtp_host untouched */
	n_warn = 0;
	for (int i = 0; i < 600; i++) CHECK(!receive(&ctx, from("192.168.2.66", 6001)));
	CHECK(ctx.foreign_packets == 600);
	CHECK(n_warn == 3);
	CHECK(ctx.rtp_host.sin_addr.s_addr == inet_addr("192.168.2.10"));
	CHECK(ctx.rtp_host.sin_port == htons(6002));

	/* the sender goes quiet while a stranger floods: STALLED once, at 30 s */
	n_stalled = 0;
	ctx.last_rx = now_ms = 10000;
	for (now_ms = 10000; now_ms <= 40000; now_ms += 10) receive(&ctx, from("192.168.2.66", 6001));
	CHECK(n_stalled == 0);
	now_ms = 40001;
	receive(&ctx, from("192.168.2.66", 6001));
	CHECK(n_stalled == 1);
	for (now_ms = 40001; now_ms < 50000; now_ms += 10) receive(&ctx, from("192.168.2.66", 6001));
	CHECK(n_stalled == 1);
	/* the sender comes back: a later gap stalls again */
	CHECK(receive(&ctx, from("192.168.2.10", 6001)));
	CHECK(!ctx.stall_sent);
	now_ms += 30001;
	rtp_check_stalled(&ctx);
	CHECK(n_stalled == 2);

	/* a live sender amid a flood from a stranger: no false stall */
	n_stalled = 0;
	ctx = fresh("192.168.2.10");
	for (int i = 0; i < 100000; i++) {
		now_ms += 1;
		receive(&ctx, from(i % 50 ? "192.168.2.66" : "192.168.2.10", 6001));
	}
	CHECK(n_stalled == 0);

	/* the clock wraps (49 days up): still no false stall, still a real one */
	now_ms = 0xFFFFFF00u;
	ctx = fresh("192.168.2.10");
	now_ms += 0x200;
	rtp_check_stalled(&ctx);
	CHECK(n_stalled == 0);
	now_ms += 30000;
	rtp_check_stalled(&ctx);
	CHECK(n_stalled == 1);

	/* no RTSP peer known (INADDR_ANY): every sender is taken, as upstream did */
	ctx = fresh(NULL);
	CHECK(receive(&ctx, from("192.168.2.66", 6001)));
	CHECK(ctx.rtp_host.sin_addr.s_addr == inet_addr("192.168.2.66"));
	CHECK(receive(&ctx, from("192.168.2.77", 6003)));
	CHECK(ctx.rtp_host.sin_addr.s_addr == inet_addr("192.168.2.77"));
	CHECK(ctx.foreign_packets == 0);

	return check_report("up_rtp");
}
