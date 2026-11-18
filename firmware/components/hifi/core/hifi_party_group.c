/* Party mode's group: finding a leader, joining, and who leads. See
 * hifi_party_group.h and projects/hifi-lite-lab/party-mode.md, section 8. */
#include "hifi_party_group.h"

#include <string.h>

#include "hifi_sha256.h"

static bool after(uint32_t now, uint32_t t) { /* now is at or past t: for deadlines a few seconds out */
	return (int32_t) (now - t) >= 0;
}

static bool since(uint32_t now, uint32_t t, uint32_t ms) { /* ms or more since t, however long ago, across the wrap */
	return now - t >= ms;
}

static void expire_dead(hifi_party_group_t *g, uint32_t now) {
	for (int i = 0; i < HIFI_PARTY_DEAD_SLOTS; i++) {
		if (g->dead[i].live && since(now, g->dead[i].at, g->dead[i].ms)) g->dead[i].live = false;
	}
}

/* the group followed now had no CLOCK: skip it for a while, twice as long
 * as last time when the same group and term was marked before */
static void mark_dead(hifi_party_group_t *g, uint32_t now) {
	hifi_party_dead_t *d = NULL;
	for (int i = 0; i < HIFI_PARTY_DEAD_SLOTS && !d; i++) {
		if (g->dead[i].group == g->group && g->dead[i].term == g->term && g->dead[i].ms) d = &g->dead[i];
	}
	uint32_t ms = !d ? HIFI_PARTY_DEAD_MS : d->ms >= HIFI_PARTY_DEAD_MAX_MS / 2 ? HIFI_PARTY_DEAD_MAX_MS : d->ms * 2;
	if (!d) {
		/* a free slot, else the longest expired, else the oldest */
		for (int i = 0; i < HIFI_PARTY_DEAD_SLOTS; i++) {
			hifi_party_dead_t *e = &g->dead[i];
			if (!e->group) {
				d = e;
				break;
			}
			if (!d || (d->live && !e->live) ||
			    (d->live == e->live && now - e->at > now - d->at))
				d = e;
		}
	}
	d->group = g->group;
	d->term = g->term;
	d->at = now;
	d->ms = ms;
	d->live = true;
}

static uint32_t get_be32(const uint8_t *p) {
	return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

const char *hifi_party_role_name(hifi_party_role_t r) {
	switch (r) {
	case HIFI_PARTY_ROLE_LISTENING: return "listening";
	case HIFI_PARTY_ROLE_LEADER: return "leader";
	case HIFI_PARTY_ROLE_FOLLOWER: return "follower";
	default: return "off";
	}
}

bool hifi_party_same_subnet(uint32_t ip, uint32_t self_ip, uint32_t mask) {
	if (!ip || !self_ip || ip == self_ip || (ip & mask) != (self_ip & mask)) return false;
	uint32_t host = ip & ~mask;
	if (~mask > 1 && (host == 0 || host == ~mask)) return false; /* the network or its broadcast */
	return true;
}

/* ------------------------------------------------------------------ keys */

static void hmac(const uint8_t secret[HIFI_PARTY_SECRET], const uint8_t *msg, size_t n, uint8_t out[32]) {
	uint8_t pad[64], h[32];
	hifi_sha256_t s;
	memset(pad, 0x36, sizeof(pad));
	for (int i = 0; i < HIFI_PARTY_SECRET; i++) pad[i] ^= secret[i];
	hifi_sha256_init(&s);
	hifi_sha256_update(&s, pad, sizeof(pad));
	hifi_sha256_update(&s, msg, n);
	hifi_sha256_final(&s, h);
	memset(pad, 0x5c, sizeof(pad));
	for (int i = 0; i < HIFI_PARTY_SECRET; i++) pad[i] ^= secret[i];
	hifi_sha256_init(&s);
	hifi_sha256_update(&s, pad, sizeof(pad));
	hifi_sha256_update(&s, h, sizeof(h));
	hifi_sha256_final(&s, out);
	hifi_link_wipe(pad, sizeof(pad));
	hifi_link_wipe(&s, sizeof(s));
}

void hifi_party_group_key(const uint8_t secret[HIFI_PARTY_SECRET], uint32_t group, uint8_t key[HIFI_LINK_KEY]) {
	static const char label[] = "hifi-party-v1";
	uint8_t msg[sizeof(label) - 1 + 4], h[32];
	memcpy(msg, label, sizeof(label) - 1);
	put_be32(msg + sizeof(label) - 1, group);
	hmac(secret, msg, sizeof(msg), h);
	memcpy(key, h, HIFI_LINK_KEY);
	hifi_link_wipe(h, sizeof(h));
}

void hifi_party_announce_key(const uint8_t secret[HIFI_PARTY_SECRET], uint8_t key[HIFI_LINK_KEY]) {
	static const char label[] = "hifi-party-announce-v1";
	uint8_t h[32];
	hmac(secret, (const uint8_t *) label, sizeof(label) - 1, h);
	memcpy(key, h, HIFI_LINK_KEY);
	hifi_link_wipe(h, sizeof(h));
}

uint8_t hifi_party_secret_id(const uint8_t secret[HIFI_PARTY_SECRET]) {
	static const char label[] = "hifi-party-id-v1";
	uint8_t h[32];
	hmac(secret, (const uint8_t *) label, sizeof(label) - 1, h);
	return h[0];
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool hifi_party_parse_secret(const char *s, uint8_t secret[HIFI_PARTY_SECRET]) {
	if (!s || strlen(s) != 2 * HIFI_PARTY_SECRET) return false;
	uint8_t k[HIFI_PARTY_SECRET], any = 0;
	for (int i = 0; i < HIFI_PARTY_SECRET; i++) {
		int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			hifi_link_wipe(k, sizeof(k));
			return false;
		}
		k[i] = (uint8_t) (hi << 4 | lo);
		any |= k[i];
	}
	if (!any) return false; /* all zeros: a blank, not a key */
	memcpy(secret, k, sizeof(k));
	hifi_link_wipe(k, sizeof(k));
	return true;
}

/* ------------------------------------------------------------ the group */

/* a's rank is above (picked, term, ip) */
static bool outranks(bool picked, int64_t term, uint32_t ip, bool p2, int64_t t2, uint32_t ip2) {
	if (picked != p2) return picked;
	if (term != t2) return term > t2;
	return ip > ip2;
}

static void see_term(hifi_party_group_t *g, int64_t term) {
	if (term > g->max_term) {
		g->max_term = term;
		g->dirty = true;
	}
}

static void set_picked(hifi_party_group_t *g, bool picked) {
	if (g->picked != picked) {
		g->picked = picked;
		g->dirty = true;
	}
}

static void forget_peers(hifi_party_group_t *g) {
	g->npeers = 0;
	memset(g->peers, 0, sizeof(g->peers));
	memset(g->seen, 0, sizeof(g->seen));
}

/* a follower moving away tells its leader */
static int leave_leader(hifi_party_group_t *g) {
	if (g->role != HIFI_PARTY_ROLE_FOLLOWER) return 0;
	g->leave_ip = g->leader_ip;
	g->leave_group = g->group;
	return HIFI_PARTY_DO_LEAVE;
}

static void go_listen(hifi_party_group_t *g, uint32_t now, uint32_t rnd, bool orphan) {
	g->role = HIFI_PARTY_ROLE_LISTENING;
	g->orphan = orphan;
	g->listen_until = now + HIFI_PARTY_LISTEN_MS + rnd % (HIFI_PARTY_LISTEN_JITTER_MS + 1);
	g->have_best = false;
	g->leader_ip = 0;
	g->group = 0;
	g->full = false;
	forget_peers(g);
}

static int start_lead(hifi_party_group_t *g, uint32_t now, uint32_t rnd, bool picked) {
	int r = leave_leader(g) | HIFI_PARTY_DO_ROLE | HIFI_PARTY_DO_ANNOUNCE;
	see_term(g, g->max_term + 1);
	g->term = g->max_term;
	set_picked(g, picked);
	uint32_t group = rnd ? rnd : 1;
	if (group == g->group) group++; /* a new key: nothing of the old stream passes */
	g->group = group ? group : 1;
	g->role = HIFI_PARTY_ROLE_LEADER;
	g->orphan = false;
	g->leader_ip = 0;
	g->full = false;
	g->have_best = false;
	g->why = "";
	g->last_announce = now;
	forget_peers(g);
	g->leads++;
	return r;
}

static int follow(hifi_party_group_t *g, uint32_t now, const hifi_party_announce_t *a) {
	/* left with no CLOCK half a second after joining, for an announce with a higher
	 * term: marked as if lost. A real leader's first CLOCK comes well inside
	 * that, so one just joined isn't marked for a hop. */
	if (g->role == HIFI_PARTY_ROLE_FOLLOWER && !g->clocked && !g->full && a->group != g->group &&
	    since(now, g->last_heard, HIFI_PARTY_HOP_MARK_MS))
		mark_dead(g, now);
	int r = leave_leader(g) | HIFI_PARTY_DO_ROLE | HIFI_PARTY_DO_JOIN;
	g->role = HIFI_PARTY_ROLE_FOLLOWER;
	g->orphan = false;
	g->have_best = false;
	g->leader_ip = a->ip;
	g->group = a->group;
	g->term = a->term;
	g->leader_picked = a->picked;
	memcpy(g->leader_name, a->name, sizeof(g->leader_name));
	if (a->picked) set_picked(g, false); /* someone else's pick: ours is forgotten */
	forget_peers(g);
	g->joined_at = g->last_heard = g->last_join = g->last_announce_heard = now;
	g->clocked = false;
	bool listed = false;
	for (int i = 0; i < a->n; i++) listed |= a->followers[i] == g->self_ip;
	g->full = a->n >= HIFI_PARTY_FOLLOWERS && !listed;
	g->why = g->full ? "group full" : "";
	g->follows++;
	return r;
}

void hifi_party_group_init(hifi_party_group_t *g, uint32_t self_ip, uint32_t mask, int64_t term, bool picked) {
	memset(g, 0, sizeof(*g));
	g->self_ip = self_ip;
	g->mask = mask;
	g->max_term = term > 0 ? term : 0;
	g->picked = picked;
	g->why = "";
}

int hifi_party_group_on(hifi_party_group_t *g, uint32_t now, uint32_t rnd) {
	if (g->role != HIFI_PARTY_ROLE_OFF) return 0;
	go_listen(g, now, rnd, false);
	g->why = "";
	return HIFI_PARTY_DO_ROLE;
}

int hifi_party_group_off(hifi_party_group_t *g) {
	if (g->role == HIFI_PARTY_ROLE_OFF) return 0;
	int r = leave_leader(g) | HIFI_PARTY_DO_ROLE;
	g->role = HIFI_PARTY_ROLE_OFF;
	g->orphan = false;
	g->have_best = false;
	g->leader_ip = 0;
	g->group = 0;
	g->full = false;
	g->why = "";
	forget_peers(g);
	return r;
}

int hifi_party_group_set_ip(hifi_party_group_t *g, uint32_t now, uint32_t self_ip, uint32_t mask, uint32_t rnd) {
	if (self_ip == g->self_ip && mask == g->mask) return 0;
	g->self_ip = self_ip;
	g->mask = mask;
	if (g->role == HIFI_PARTY_ROLE_OFF) return 0;
	int r = HIFI_PARTY_DO_ROLE | (g->role == HIFI_PARTY_ROLE_FOLLOWER ? HIFI_PARTY_DO_LOST : 0);
	go_listen(g, now, rnd, false);
	return r;
}

int hifi_party_group_lead(hifi_party_group_t *g, uint32_t now, uint32_t rnd) {
	if (g->role == HIFI_PARTY_ROLE_OFF || g->heap_low) return HIFI_PARTY_E_OFF;
	if (g->led && !since(now, g->last_lead, HIFI_PARTY_LEAD_GAP_MS)) return HIFI_PARTY_E_SOON;
	g->led = true;
	g->last_lead = now;
	if (g->role == HIFI_PARTY_ROLE_LEADER) {
		/* already leading: picked now, under a new term, and the same
		 * group, so the followers' stream goes on */
		see_term(g, g->max_term + 1);
		g->term = g->max_term;
		set_picked(g, true);
		g->last_announce = now;
		g->leads++;
		return HIFI_PARTY_DO_ANNOUNCE;
	}
	return start_lead(g, now, rnd, true);
}

/* listening, the window over: follow the best leader heard, or lead */
static int decide(hifi_party_group_t *g, uint32_t now, uint32_t rnd) {
	if (g->have_best && since(now, g->best_at, HIFI_PARTY_LOSS_MS)) g->have_best = false; /* heard too long ago */
	if (g->have_best) {
		/* a board whose pick nobody overrode takes its lead back */
		if (g->picked && !g->best.picked && !g->orphan && !g->heap_low) return start_lead(g, now, rnd, true);
		return follow(g, now, &g->best);
	}
	if (g->heap_low) {
		g->why = "low memory";
		return 0;
	}
	if (g->other_ap && !since(now, g->other_ap_at, HIFI_PARTY_LOSS_MS)) {
		g->why = "another access point";
		return 0;
	}
	if (g->orphan) return 0; /* never leads silence by itself */
	return start_lead(g, now, rnd, g->picked);
}

bool hifi_party_local_source(hifi_party_role_t role, int source) {
	if (source == 6) return role != HIFI_PARTY_ROLE_FOLLOWER;
	return source >= 1 && source <= 4;
}

bool hifi_party_retakes(hifi_party_role_t role, int source) {
	return role == HIFI_PARTY_ROLE_FOLLOWER && (source == 0 || source == 6);
}

int hifi_party_group_tick(hifi_party_group_t *g, uint32_t now, uint32_t rnd, bool local_source, bool low_heap) {
	if (low_heap && !g->heap_low) g->low_heap++;
	g->heap_low = low_heap;
	expire_dead(g, now); /* every tick, so a mark can't come back when the ms clock wraps */
	switch (g->role) {
	case HIFI_PARTY_ROLE_OFF: return 0;
	case HIFI_PARTY_ROLE_LISTENING:
		if (local_source) {
			int r = hifi_party_group_lead(g, now, rnd);
			if (r >= 0) return r;
		}
		return after(now, g->listen_until) ? decide(g, now, rnd) : 0;
	case HIFI_PARTY_ROLE_LEADER: {
		if (low_heap) {
			/* stop leading: the followers mute when CLOCKs stop */
			go_listen(g, now, rnd, true);
			g->why = "low memory";
			return HIFI_PARTY_DO_ROLE;
		}
		if (local_source && !g->picked) {
			/* our own source plays: the lead is picked from now */
			int r = hifi_party_group_lead(g, now, rnd);
			if (r >= 0) return r;
		}
		for (int i = 0; i < g->npeers;) {
			if (since(now, g->peer_seen[i], HIFI_PARTY_LOSS_MS)) {
				g->npeers--;
				g->peers[i] = g->peers[g->npeers];
				g->peer_seen[i] = g->peer_seen[g->npeers];
				g->peer_nack_seq[i] = g->peer_nack_seq[g->npeers];
				g->peer_nacked[i] = g->peer_nacked[g->npeers];
				g->peers[g->npeers] = 0;
			} else {
				i++;
			}
		}
		if (since(now, g->last_announce, HIFI_PARTY_ANNOUNCE_MS)) {
			g->last_announce = now;
			return HIFI_PARTY_DO_ANNOUNCE;
		}
		return 0;
	}
	case HIFI_PARTY_ROLE_FOLLOWER: {
		if (local_source) {
			int r = hifi_party_group_lead(g, now, rnd);
			if (r >= 0) return r;
		}
		bool lost = g->full ? since(now, g->last_announce_heard, HIFI_PARTY_LOSS_MS)
				    : since(now, g->last_heard, HIFI_PARTY_LOSS_MS);
		if (lost) {
			if (!g->clocked && !g->full) {
				/* joined, never one CLOCK: a played-back announce, or a
				 * leader that can't reach us. Skip it for a while, longer
				 * each time the same one fails again. */
				mark_dead(g, now);
			}
			g->losses++;
			go_listen(g, now, rnd, true);
			g->why = "leader lost";
			return HIFI_PARTY_DO_LOST | HIFI_PARTY_DO_ROLE;
		}
		if (since(now, g->last_join, HIFI_PARTY_ANNOUNCE_MS)) {
			g->last_join = now;
			return HIFI_PARTY_DO_JOIN;
		}
		return 0;
	}
	}
	return 0;
}

int hifi_party_group_announce(hifi_party_group_t *g, uint32_t now, const hifi_party_announce_t *a) {
	if (g->role == HIFI_PARTY_ROLE_OFF || a->ip == g->self_ip) return 0;
	expire_dead(g, now); /* each expired one is tried again */
	for (int i = 0; i < HIFI_PARTY_DEAD_SLOTS; i++) {
		if (g->dead[i].live && a->group == g->dead[i].group && a->term <= g->dead[i].term) {
			g->replays++;
			return 0;
		}
	}
	see_term(g, a->term);
	switch (g->role) {
	case HIFI_PARTY_ROLE_LISTENING:
		if (!g->have_best || since(now, g->best_at, HIFI_PARTY_LOSS_MS) ||
		    outranks(a->picked, a->term, a->ip, g->best.picked, g->best.term, g->best.ip) ||
		    (a->ip == g->best.ip && a->group == g->best.group)) {
			g->best = *a;
			g->best_at = now;
			g->have_best = true;
		}
		return 0;
	case HIFI_PARTY_ROLE_LEADER:
		if (outranks(a->picked, a->term, a->ip, g->picked, g->term, g->self_ip)) return follow(g, now, a);
		return 0;
	case HIFI_PARTY_ROLE_FOLLOWER:
		if (a->ip == g->leader_ip) {
			if (a->group != g->group) /* the leader led again, or an old one played back */
				return a->term > g->term ? follow(g, now, a) : 0;
			bool listed = false;
			for (int i = 0; i < a->n; i++) listed |= a->followers[i] == g->self_ip;
			bool full = a->n >= HIFI_PARTY_FOLLOWERS && !listed;
			if (g->full && !full) g->last_heard = now; /* a place: the CLOCK wait starts now */
			g->full = full;
			g->why = full ? "group full" : "";
			g->last_announce_heard = now;
			g->term = a->term; /* a Lead on the leader raises it, same group */
			g->leader_picked = a->picked;
			if (a->picked) set_picked(g, false);
			memcpy(g->leader_name, a->name, sizeof(g->leader_name));
			return 0;
		}
		if (outranks(a->picked, a->term, a->ip, g->leader_picked, g->term, g->leader_ip)) return follow(g, now, a);
		return 0;
	default: return 0;
	}
}

void hifi_party_group_other_ap(hifi_party_group_t *g, uint32_t now, uint32_t ip, int64_t tsf_us) {
	bool same = ip && ip == g->off_ip && !since(now, g->off_at, HIFI_PARTY_LOSS_MS);
	if (same && tsf_us <= g->off_tsf) return; /* the same or an older one again: keep the one to beat */
	int64_t d = tsf_us - g->off_tsf - (int64_t) (now - g->off_at) * 1000; /* its clock against ours */
	bool moved = same && d <= HIFI_PARTY_OTHER_AP_SLACK_US && d >= -HIFI_PARTY_OTHER_AP_SLACK_US;
	g->off_ip = ip;
	g->off_at = now;
	g->off_tsf = tsf_us;
	if (!moved) return;
	g->other_ap = true;
	g->other_ap_at = now;
	g->other_aps++;
}

static int peer_index(const hifi_party_group_t *g, uint32_t ip) {
	for (int i = 0; i < g->npeers; i++)
		if (g->peers[i] == ip) return i;
	return -1;
}

bool hifi_party_group_join(hifi_party_group_t *g, uint32_t now, uint32_t ip, int64_t tsf, bool leave) {
	if (g->role != HIFI_PARTY_ROLE_LEADER || !ip) return false;
	/* the TSF must rise per sender, so a JOIN or a leave played back is dropped */
	hifi_party_seen_t *s = NULL;
	for (int i = 0; i < HIFI_PARTY_JOIN_SEEN; i++)
		if (g->seen[i].ip == ip) s = &g->seen[i];
	if (s && tsf <= s->tsf) {
		g->replays++;
		return false;
	}
	if (!s) {
		/* an empty place, or the one heard from longest ago that isn't a follower */
		for (int i = 0; i < HIFI_PARTY_JOIN_SEEN; i++) {
			hifi_party_seen_t *e = &g->seen[i];
			if (!e->ip) {
				s = e;
				break;
			}
			if (peer_index(g, e->ip) < 0 && (!s || (int32_t) (e->at - s->at) < 0)) s = e;
		}
		if (!s) return false; /* can't be: at most HIFI_PARTY_FOLLOWERS of the places are followers */
		s->ip = ip;
	}
	s->tsf = tsf;
	s->at = now;
	int i = peer_index(g, ip);
	if (leave) {
		if (i >= 0) {
			g->npeers--;
			g->peers[i] = g->peers[g->npeers];
			g->peer_seen[i] = g->peer_seen[g->npeers];
			g->peer_nack_seq[i] = g->peer_nack_seq[g->npeers];
			g->peer_nacked[i] = g->peer_nacked[g->npeers];
			g->peers[g->npeers] = 0;
		}
		return true;
	}
	if (i >= 0) {
		g->peer_seen[i] = now;
		return true;
	}
	if (g->npeers >= HIFI_PARTY_FOLLOWERS) {
		g->refused++;
		return false;
	}
	i = g->npeers++;
	g->peers[i] = ip;
	g->peer_seen[i] = now;
	g->peer_nacked[i] = false;
	g->peer_nack_seq[i] = 0;
	return true;
}

uint32_t hifi_party_group_skip_ms(const hifi_party_group_t *g, uint32_t now) {
	uint32_t left = 0;
	for (int i = 0; i < HIFI_PARTY_DEAD_SLOTS; i++) {
		const hifi_party_dead_t *d = &g->dead[i];
		if (d->live && !since(now, d->at, d->ms) && d->ms - (now - d->at) > left) left = d->ms - (now - d->at);
	}
	return left;
}

bool hifi_party_group_heard(hifi_party_group_t *g, uint32_t now, uint32_t ip, uint32_t seq) {
	if (g->role != HIFI_PARTY_ROLE_FOLLOWER || ip != g->leader_ip) return false;
	if (g->clocked && (int32_t) (seq - g->clock_seq) <= 0) {
		g->replays++;
		return false;
	}
	g->clock_seq = seq;
	g->clocked = true;
	for (int i = 0; i < HIFI_PARTY_DEAD_SLOTS; i++) {
		if (g->dead[i].group == g->group) memset(&g->dead[i], 0, sizeof(g->dead[i])); /* a real leader after all */
	}
	g->last_heard = now;
	g->full = false;
	g->why = "";
	return true;
}

bool hifi_party_group_is_peer(const hifi_party_group_t *g, uint32_t ip) {
	return g->role == HIFI_PARTY_ROLE_LEADER && ip && peer_index(g, ip) >= 0;
}

bool hifi_party_group_nack_ok(hifi_party_group_t *g, uint32_t ip, uint32_t seq) {
	int i = g->role == HIFI_PARTY_ROLE_LEADER ? peer_index(g, ip) : -1;
	if (i < 0 || (g->peer_nacked[i] && (int32_t) (seq - g->peer_nack_seq[i]) <= 0)) {
		g->replays++;
		return false;
	}
	g->peer_nacked[i] = true;
	g->peer_nack_seq[i] = seq;
	return true;
}

/* src_ip's own share of the stranger budget: a slot per address, the
 * longest-unused one taken for a new address */
static bool stranger_share(hifi_party_group_t *g, uint32_t now, uint32_t ip) {
	hifi_party_stranger_t *s = NULL, *old = &g->strangers[0];
	for (int i = 0; i < HIFI_PARTY_STRANGER_SRCS && !s; i++) {
		hifi_party_stranger_t *e = &g->strangers[i];
		if (e->ip == ip) s = e;
		else if (now - e->at > now - old->at) old = e;
	}
	if (!s) {
		s = old;
		s->ip = ip;
		s->n = 0;
		s->at = now;
	} else if (since(now, s->at, 1000)) {
		s->at = now;
		s->n = 0;
	}
	if (s->n >= HIFI_PARTY_STRANGER_SRC_PER_S) return false;
	s->n++;
	return true;
}

bool hifi_party_group_admit(hifi_party_group_t *g, uint32_t now, uint32_t src_ip, uint8_t type) {
	if (g->role == HIFI_PARTY_ROLE_OFF) return false;
	if (g->role == HIFI_PARTY_ROLE_LEADER && hifi_party_group_is_peer(g, src_ip)) return true;
	if (g->role == HIFI_PARTY_ROLE_FOLLOWER && src_ip == g->leader_ip) return true;
	if (type == HIFI_PARTY_ANNOUNCE || type == HIFI_PARTY_JOIN) {
		if (since(now, g->budget_at, 1000)) {
			g->budget_at = now;
			g->budget = HIFI_PARTY_STRANGERS_PER_S;
		}
		if (g->budget > 0 && stranger_share(g, now, src_ip)) {
			g->budget--;
			return true;
		}
	}
	g->dropped++;
	return false;
}

/* --------------------------------------------------------------- packets */

void hifi_party_announce_build(hifi_party_group_t *g, int64_t tsf_us, const char *name, hifi_party_pkt_t *p,
			       uint8_t payload[HIFI_PARTY_ANNOUNCE_LEN]) {
	memset(p, 0, sizeof(*p));
	p->type = HIFI_PARTY_ANNOUNCE;
	p->group = g->group;
	p->seq = ++g->announce_seq;
	p->k = g->term;
	p->t = tsf_us;
	p->n = (uint16_t) g->npeers;
	p->flags = g->picked ? HIFI_PARTY_F_PICKED : 0;
	memset(payload, 0, HIFI_PARTY_ANNOUNCE_LEN);
	put_be32(payload, g->self_ip);
	for (int i = 0; i < g->npeers; i++) put_be32(payload + 4 + 4 * i, g->peers[i]);
	if (name) {
		size_t n = strlen(name);
		memcpy(payload + 4 + 4 * HIFI_PARTY_FOLLOWERS, name, n < HIFI_PARTY_NAME ? n : HIFI_PARTY_NAME);
	}
}

static bool tsf_near(int64_t a, int64_t b) {
	int64_t d = a - b;
	return d <= HIFI_PARTY_TSF_SKEW_US && d >= -HIFI_PARTY_TSF_SKEW_US;
}

int hifi_party_announce_parse(const hifi_party_pkt_t *p, const uint8_t *payload, size_t plen, uint32_t src_ip, uint32_t self_ip,
			      uint32_t mask, int64_t tsf_us, hifi_party_announce_t *a) {
	if (p->type != HIFI_PARTY_ANNOUNCE || plen != HIFI_PARTY_ANNOUNCE_LEN || p->n > HIFI_PARTY_FOLLOWERS || !p->group ||
	    p->k < 1)
		return 0;
	if (get_be32(payload) != src_ip || !hifi_party_same_subnet(src_ip, self_ip, mask)) return 0;
	hifi_party_announce_t r = {.ip = src_ip, .group = p->group, .term = p->k, .picked = p->flags & HIFI_PARTY_F_PICKED, .n = p->n};
	for (int i = 0; i < r.n; i++) r.followers[i] = get_be32(payload + 4 + 4 * i);
	const uint8_t *nm = payload + 4 + 4 * HIFI_PARTY_FOLLOWERS;
	for (int i = 0; i < HIFI_PARTY_NAME && nm[i]; i++)
		r.name[i] = nm[i] >= 0x20 && nm[i] < 0x7f ? (char) nm[i] : '?'; /* shown on a page: plain ASCII */
	if (!tsf_near(p->t, tsf_us)) return -1;
	*a = r;
	return 1;
}

void hifi_party_join_build(const hifi_party_group_t *g, int64_t tsf_us, bool leave, uint32_t seq, hifi_party_pkt_t *p,
			   uint8_t payload[4]) {
	memset(p, 0, sizeof(*p));
	p->type = HIFI_PARTY_JOIN;
	p->group = leave ? g->leave_group : g->group;
	p->seq = seq;
	p->t = tsf_us;
	p->flags = leave ? HIFI_PARTY_F_LEAVE : 0;
	put_be32(payload, g->self_ip);
}

bool hifi_party_join_parse(const hifi_party_pkt_t *p, const uint8_t *payload, size_t plen, uint32_t src_ip, uint32_t self_ip,
			   uint32_t mask, int64_t tsf_us, int64_t *tsf, bool *leave) {
	if (p->type != HIFI_PARTY_JOIN || plen != 4 || get_be32(payload) != src_ip) return false;
	if (!hifi_party_same_subnet(src_ip, self_ip, mask) || !tsf_near(p->t, tsf_us)) return false;
	*tsf = p->t;
	*leave = p->flags & HIFI_PARTY_F_LEAVE;
	return true;
}
