/*
 * The speakers on this network (GET /api/v1/peers), for the app's
 * Speakers list: every box advertises _hifi._tcp over mDNS with its name,
 * model and firmware in the TXT record (esp/hifi_main.c), and asks for the
 * others every HIFI_PEERS_ROUND_US while someone is looking
 * (esp/hifi_peers_esp.c). This part keeps the table and writes the reply,
 * so it runs in the host tests.
 */
#ifndef HIFI_PEERS_H
#define HIFI_PEERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_PEERS_MAX 12
/* a speaker not heard from in this long is dropped (several rounds) */
#define HIFI_PEERS_GONE_US (90LL * 1000 * 1000)
/* a new mDNS question at most this often, and only when the list is read */
#define HIFI_PEERS_ROUND_US (10LL * 1000 * 1000)
/* the longest reply: self and HIFI_PEERS_MAX rows, every field full of
 * characters that escape to two bytes (test_peers checks it fits) */
#define HIFI_PEERS_REPLY 6144

typedef struct {
	char ip[16];    /* dotted IPv4; the key */
	char host[64];  /* mDNS host name, without .local (63 at most) */
	char name[65];  /* the player name people see */
	char model[16]; /* lite, plus, ultra */
	char fw[24];
	long long seen_us;
} hifi_peer_t;

typedef struct {
	hifi_peer_t p[HIFI_PEERS_MAX];
	int n;
	/* this box's subnet (host order): a row elsewhere is not a speaker the
	 * app may be sent to. mask 0: private addresses only (no subnet yet) */
	uint32_t net, mask;
} hifi_peers_t;

/* a speaker answered: update its row (by IP; an older row with the same
 * host name, from before a DHCP change, goes), or add one. A full table
 * gives up the row heard from longest ago. Ignored: no IP or no host, or
 * an IP that is not private and on t's subnet (a device on the LAN can
 * answer for _hifi._tcp with any address, and the app opens and polls
 * each row's). Control characters in any field are kept as spaces. */
void hifi_peers_seen(hifi_peers_t *t, const hifi_peer_t *p, long long now_us);

/* drop the rows not heard from in HIFI_PEERS_GONE_US */
void hifi_peers_expire(hifi_peers_t *t, long long now_us);

/* one TXT item of a _hifi._tcp answer into p: name, model, fw (others
 * ignored); val is not 0-terminated */
void hifi_peers_txt(hifi_peer_t *p, const char *key, const char *val, size_t len);

/* the reply: {"peers":[self first, then the rest by name], "searching"}.
 * A row with self's IP (or, before self has one, its host) is self's own
 * answer and is left out; self's row takes that answer's host, the name
 * mDNS settled on (hifi-lite-2 when another box had hifi-lite).
 * Returns 200, or 500 with an error if cap was too small (HIFI_PEERS_REPLY
 * always fits). */
int hifi_peers_json(const hifi_peers_t *t, const hifi_peer_t *self, bool searching, long long now_us,
		    char *out, size_t cap);

/* the host name mDNS settled on, into out: set if it is ours, else the
 * names mDNS renames to after a clash, in its order (hifi-lite, then
 * hifi-lite-2, -3 ...; hifi-lite-4 goes on to -5), the first that is ours.
 * mDNS never gives a box its own answer and IDF 4.4 can't be asked for the
 * name, only whether a name is ours (mdns_hostname_exists). set when no
 * candidate up to -HIFI_PEERS_RENAMES is ours. */
#define HIFI_PEERS_RENAMES 32
void hifi_peers_own_host(const char *set, bool (*ours)(const char *host), char *out, size_t cap);

#endif
