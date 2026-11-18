/* Party mode's group (hifi_party_group.c): a simulated LAN of boards sending
 * each other real encoded packets. The cases are those of
 * projects/hifi-lite-lab/party-mode.md, 8.9. */
#include <stdlib.h>

#include "check.h"
#include "hifi_party_group.h"

#define NB 8
#define STEP 10 /* ms per step */
#define QMAX 2048
#define NET 0xc0a80100u /* 192.168.1.0/24 */
#define MASK 0xffffff00u
#define BCAST (NET | 0xffu)
#define DLEN 96 /* the longest datagram here (an announce is 78 B) */

typedef struct {
	uint32_t src, dst; /* dst BCAST: every board */
	uint16_t len;
	uint8_t buf[DLEN];
} dgram_t;

typedef struct {
	bool up; /* powered and on the Wi-Fi */
	bool on; /* party switched on, as kept in NVS */
	uint8_t secret[HIFI_PARTY_SECRET];
	uint32_t ip;
	const char *name;
	int ap; /* the access point it's on: whose TSF it reads */
	hifi_party_group_t g;
	hifi_link_key_t akey, gkey;
	uint32_t gkey_group;
	uint32_t rnd;
	bool source, low_heap;
	bool deaf_clock; /* CLOCKs to it are lost on the air */
	bool line; /* input=line, and line in holds the output (source 6) */
	int64_t nvs_term; /* NVS: kept across a power cut */
	bool nvs_picked;
	int nvs_writes;
	uint32_t clock_seq, last_clock, join_seq;
	int tsf_tick;
	int lost, clocks, bad_tag;
	bool noaudio_seen;
	dgram_t last[6], last_leave; /* what it sent last, per type: to play back */
} board_t;

static struct {
	board_t b[NB];
	int nb;
	uint32_t now;   /* the boards' ms clock: wraps */
	int64_t ms;     /* the same, never wrapping: what the TSF counts */
	int64_t ap_tsf[2]; /* each AP's TSF is now + this (µs) */
	dgram_t q[QMAX], cur[QMAX];
	int nq;
	int loss;      /* per 1000 datagrams */
	uint32_t lrnd; /* the loss draws */
	bool noaudio;  /* leaders send CLOCK with F_NOAUDIO */
} L;

static uint32_t xs(uint32_t *s) {
	uint32_t x = *s ? *s : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static int64_t tsf(board_t *b) { /* rises by at least 1 µs per read, as on the board */
	return L.ms * 1000 + L.ap_tsf[b->ap] + (b->tsf_tick++ % 1000);
}

static void reset(uint32_t base) {
	memset(&L, 0, sizeof(L));
	L.now = base;
	L.lrnd = 12345;
}

static int add(const char *name, uint32_t host, uint8_t secret_byte) {
	board_t *b = &L.b[L.nb];
	memset(b, 0, sizeof(*b));
	b->name = name;
	b->ip = NET | host;
	memset(b->secret, secret_byte, sizeof(b->secret));
	b->secret[0] = 0x42;
	uint8_t k[HIFI_LINK_KEY];
	hifi_party_announce_key(b->secret, k);
	hifi_link_key_init(&b->akey, k);
	b->rnd = 0x1000u + host * 7919u;
	return L.nb++;
}

static void push(const dgram_t *d) {
	if (L.loss && (int) (xs(&L.lrnd) % 1000) < L.loss) return;
	if (L.nq < QMAX) L.q[L.nq++] = *d;
}

static void send(board_t *b, const hifi_link_key_t *k, const hifi_party_pkt_t *p, const void *payload, uint32_t dst) {
	dgram_t d = {.src = b->ip, .dst = dst};
	d.len = (uint16_t) hifi_party_encode(k, p, payload, d.buf, sizeof(d.buf));
	CHECK(d.len > 0);
	if (!d.len) return;
	if (p->type == HIFI_PARTY_JOIN && (p->flags & HIFI_PARTY_F_LEAVE))
		b->last_leave = d;
	else
		b->last[p->type] = d;
	push(&d);
}

static void use_group_key(board_t *b) {
	if (b->g.role != HIFI_PARTY_ROLE_LEADER && b->g.role != HIFI_PARTY_ROLE_FOLLOWER) {
		hifi_link_key_wipe(&b->gkey);
		b->gkey_group = 0;
		return;
	}
	if (b->gkey.ready && b->gkey_group == b->g.group) return;
	uint8_t k[HIFI_LINK_KEY];
	hifi_party_group_key(b->secret, b->g.group, k);
	hifi_link_key_init(&b->gkey, k);
	b->gkey_group = b->g.group;
}

/* what the ESP side does with a call's result */
static void act(board_t *b, int r) {
	if (r <= 0) return;
	hifi_party_pkt_t p;
	if (r & HIFI_PARTY_DO_LEAVE) {
		uint8_t pl[4], k[HIFI_LINK_KEY];
		hifi_link_key_t lk;
		hifi_party_group_key(b->secret, b->g.leave_group, k);
		hifi_link_key_init(&lk, k);
		hifi_party_join_build(&b->g, tsf(b), true, ++b->join_seq, &p, pl);
		CHECK(p.group == b->g.leave_group);
		send(b, &lk, &p, pl, b->g.leave_ip);
	}
	if (r & HIFI_PARTY_DO_ROLE) {
		use_group_key(b);
		if (b->g.role == HIFI_PARTY_ROLE_FOLLOWER) b->source = false; /* its own player is stopped */
		b->last_clock = L.now;
	}
	if (r & HIFI_PARTY_DO_LOST) b->lost++;
	if (r & HIFI_PARTY_DO_ANNOUNCE) {
		uint8_t pl[HIFI_PARTY_ANNOUNCE_LEN];
		hifi_party_announce_build(&b->g, tsf(b), b->name, &p, pl);
		send(b, &b->akey, &p, pl, BCAST);
	}
	if (r & HIFI_PARTY_DO_JOIN) {
		uint8_t pl[4];
		hifi_party_join_build(&b->g, tsf(b), false, ++b->join_seq, &p, pl);
		send(b, &b->gkey, &p, pl, b->g.leader_ip);
	}
}

static void save(board_t *b) {
	if (!b->g.dirty) return;
	b->nvs_term = b->g.max_term;
	b->nvs_picked = b->g.picked;
	b->nvs_writes++;
	b->g.dirty = false;
}

static void receive(board_t *b, const dgram_t *d) {
	if (!b->up || b->g.role == HIFI_PARTY_ROLE_OFF) return;
	if ((d->dst != BCAST && d->dst != b->ip) || d->src == b->ip || d->len < 4) return;
	if (!hifi_party_group_admit(&b->g, L.now, d->src, d->buf[3])) return;
	const hifi_link_key_t *k = d->buf[3] == HIFI_PARTY_ANNOUNCE ? &b->akey : &b->gkey;
	hifi_party_pkt_t p;
	const uint8_t *pl;
	size_t plen;
	if (hifi_party_decode(k, d->buf, d->len, &p, &pl, &plen) != HIFI_PARTY_OK) {
		b->bad_tag++;
		return;
	}
	switch (p.type) {
	case HIFI_PARTY_ANNOUNCE: {
		hifi_party_announce_t a;
		int r = hifi_party_announce_parse(&p, pl, plen, d->src, b->ip, MASK, tsf(b), &a);
		if (r > 0)
			act(b, hifi_party_group_announce(&b->g, L.now, &a));
		else if (r < 0)
			hifi_party_group_other_ap(&b->g, L.now, d->src, p.t);
		break;
	}
	case HIFI_PARTY_JOIN: {
		int64_t t;
		bool leave;
		if (p.group == b->g.group && hifi_party_join_parse(&p, pl, plen, d->src, b->ip, MASK, tsf(b), &t, &leave))
			hifi_party_group_join(&b->g, L.now, d->src, t, leave);
		break;
	}
	case HIFI_PARTY_CLOCK:
		if (b->deaf_clock) break;
		if (p.group == b->g.group && hifi_party_group_heard(&b->g, L.now, d->src, p.seq)) {
			b->clocks++;
			if (p.flags & HIFI_PARTY_F_NOAUDIO) b->noaudio_seen = true;
		}
		break;
	default: break;
	}
	save(b);
}

static void step(void) {
	L.now += STEP;
	L.ms += STEP;
	for (int i = 0; i < L.nb; i++) {
		board_t *b = &L.b[i];
		if (!b->up) continue;
		/* a follower takes its output back from line in: the line stops */
		if (b->line && hifi_party_retakes(b->g.role, 6)) b->line = false;
		bool own = b->source || (b->line && hifi_party_local_source(b->g.role, 6));
		act(b, hifi_party_group_tick(&b->g, L.now, xs(&b->rnd), own, b->low_heap));
		if (b->g.role == HIFI_PARTY_ROLE_LEADER && L.now - b->last_clock >= 200) {
			b->last_clock = L.now;
			static const uint8_t bssid[6] = {2, 0, 0, 0, 0, 1};
			for (int j = 0; j < b->g.npeers; j++) {
				hifi_party_pkt_t p = {.type = HIFI_PARTY_CLOCK, .group = b->g.group, .seq = ++b->clock_seq, .k = 1000,
						      .t = tsf(b), .rate = 44100000, .flags = L.noaudio ? HIFI_PARTY_F_NOAUDIO : 0};
				send(b, &b->gkey, &p, bssid, b->g.peers[j]);
			}
		}
		save(b);
	}
	int n = L.nq;
	memcpy(L.cur, L.q, (size_t) n * sizeof(dgram_t));
	L.nq = 0;
	for (int i = 0; i < n; i++)
		for (int j = 0; j < L.nb; j++) receive(&L.b[j], &L.cur[i]);
}

static void run(uint32_t ms) {
	for (uint32_t t = 0; t < ms; t += STEP) step();
}

static void inject(const dgram_t *d) {
	if (L.nq < QMAX) L.q[L.nq++] = *d;
}

static void power_on(int i) {
	board_t *b = &L.b[i];
	b->up = true;
	hifi_party_group_init(&b->g, b->ip, MASK, b->nvs_term, b->nvs_picked);
	use_group_key(b);
	if (b->on) act(b, hifi_party_group_on(&b->g, L.now, xs(&b->rnd)));
}

static void power_cut(int i) { L.b[i].up = false; }

static void switch_on(int i) {
	board_t *b = &L.b[i];
	b->on = true;
	if (!b->up) {
		power_on(i);
		return;
	}
	act(b, hifi_party_group_on(&b->g, L.now, xs(&b->rnd)));
}

static void switch_off(int i) {
	board_t *b = &L.b[i];
	b->on = false;
	act(b, hifi_party_group_off(&b->g));
}

static int lead(int i) {
	board_t *b = &L.b[i];
	int r = hifi_party_group_lead(&b->g, L.now, xs(&b->rnd));
	act(b, r);
	save(b);
	return r;
}

static hifi_party_role_t role(int i) { return L.b[i].g.role; }
static bool follows(int i, int leader) {
	return L.b[i].g.role == HIFI_PARTY_ROLE_FOLLOWER && L.b[i].g.leader_ip == L.b[leader].ip &&
	       L.b[i].g.group == L.b[leader].g.group;
}
static bool serves(int leader, int i) { return hifi_party_group_is_peer(&L.b[leader].g, L.b[i].ip); }
static int leaders(void) {
	int n = 0;
	for (int i = 0; i < L.nb; i++) n += L.b[i].up && L.b[i].g.role == HIFI_PARTY_ROLE_LEADER;
	return n;
}

/* board i's dead mark for group, or NULL */
static const hifi_party_dead_t *dead_mark(int i, uint32_t group) {
	for (int k = 0; k < HIFI_PARTY_DEAD_SLOTS; k++) {
		if (group && L.b[i].g.dead[k].group == group) return &L.b[i].g.dead[k];
	}
	return NULL;
}

/* ------------------------------------------------------------- the cases */

static void first_on(uint32_t base) {
	reset(base);
	int a = add("Kitchen", 10, 1), b = add("Den", 11, 1), c = add("Porch", 12, 1);
	switch_on(a);
	run(1990);
	CHECK(role(a) == HIFI_PARTY_ROLE_LISTENING);
	run(600);
	CHECK(role(a) == HIFI_PARTY_ROLE_LEADER);
	CHECK(L.b[a].g.term == 1 && !L.b[a].g.picked);
	CHECK(L.b[a].nvs_term == 1 && L.b[a].nvs_writes == 1);
	switch_on(b);
	switch_on(c);
	run(4000);
	CHECK(follows(b, a) && follows(c, a));
	CHECK(L.b[a].g.npeers == 2 && serves(a, b) && serves(a, c));
	CHECK(L.b[b].g.clocked && L.b[c].g.clocked);
	CHECK_STR(L.b[b].g.leader_name, "Kitchen");
	CHECK_STR(hifi_party_role_name(role(b)), "follower");
	run(30000);
	CHECK(follows(b, a) && follows(c, a) && L.b[a].g.npeers == 2);
	CHECK(L.b[b].lost == 0 && L.b[c].lost == 0 && L.b[b].clocks > 140);
	CHECK(L.b[a].nvs_writes == 1); /* NVS only when the term or the pick changes */
	CHECK(L.b[b].g.follows == 1 && L.b[a].g.leads == 1);
}

static void join_leave_full(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1), d = add("D", 13, 1), e = add("E", 14, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	switch_on(d);
	run(4000);
	switch_on(e);
	run(4000);
	CHECK(L.b[a].g.npeers == 3 && serves(a, b) && serves(a, c) && serves(a, d) && !serves(a, e));
	CHECK(role(e) == HIFI_PARTY_ROLE_FOLLOWER && L.b[e].g.full && !L.b[e].g.clocked);
	CHECK_STR(L.b[e].g.why, "group full");
	CHECK(L.b[a].g.refused > 0 && L.b[e].clocks == 0);
	run(10000); /* the full board waits, alive while the announces come */
	CHECK(role(e) == HIFI_PARTY_ROLE_FOLLOWER && L.b[e].g.full && L.b[e].lost == 0);

	switch_off(d); /* leave: its place goes at once */
	CHECK(role(d) == HIFI_PARTY_ROLE_OFF);
	run(20);
	CHECK(L.b[a].g.npeers == 2 && !serves(a, d));
	run(2500);
	CHECK(serves(a, e) && L.b[e].g.clocked && !L.b[e].g.full && L.b[e].lost == 0);
	CHECK_STR(L.b[e].g.why, "");

	power_cut(c); /* a quiet follower drops off after 3 s */
	run(2500);
	CHECK(serves(a, c));
	run(1000);
	CHECK(!serves(a, c) && L.b[a].g.npeers == 2);
	CHECK(L.b[b].lost == 0 && L.b[e].lost == 0);
}

static void leads(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	run(4000);
	CHECK(follows(b, a) && follows(c, a));

	/* Lead moves the group */
	CHECK(lead(b) > 0);
	CHECK(role(b) == HIFI_PARTY_ROLE_LEADER && L.b[b].g.picked && L.b[b].g.term == 2);
	CHECK(L.b[b].nvs_term == 2 && L.b[b].nvs_picked);
	run(3000);
	CHECK(follows(a, b) && follows(c, b) && L.b[b].g.npeers == 2);
	CHECK(L.b[a].g.leader_picked && !L.b[a].g.picked);
	CHECK(L.b[a].lost == 0 && L.b[c].lost == 0);

	/* the rate limit; and Lead on the leader keeps the group */
	CHECK(lead(b) == HIFI_PARTY_E_SOON);
	run(2100);
	uint32_t group = L.b[b].g.group, fa = L.b[a].g.follows;
	CHECK(lead(b) == HIFI_PARTY_DO_ANNOUNCE);
	CHECK(L.b[b].g.group == group && L.b[b].g.term == 3);
	run(2000);
	CHECK(follows(a, b) && L.b[a].g.follows == fa && L.b[a].g.term == 3);

	/* a picked lead beats an older picked lead */
	CHECK(lead(c) > 0 && L.b[c].g.term == 4);
	run(3000);
	CHECK(follows(a, c) && follows(b, c) && !L.b[b].g.picked && L.b[b].nvs_picked == false);

	/* two Leads at once: one term, the higher IP wins */
	run(5000);
	CHECK(lead(a) > 0);
	CHECK(lead(b) > 0);
	CHECK(L.b[a].g.term == 5 && L.b[b].g.term == 5);
	run(3000);
	CHECK(leaders() == 1 && role(b) == HIFI_PARTY_ROLE_LEADER);
	CHECK(follows(a, b) && follows(c, b) && !L.b[c].g.picked && !L.b[a].g.picked);
	uint32_t f[3] = {L.b[a].g.follows, L.b[b].g.follows, L.b[c].g.follows};
	run(20000); /* and it stays */
	CHECK(L.b[a].g.follows == f[0] && L.b[b].g.follows == f[1] && L.b[c].g.follows == f[2]);
	CHECK(L.b[a].lost == 0 && L.b[c].lost == 0);

	switch_off(c);
	CHECK(lead(c) == HIFI_PARTY_E_OFF);
}

static void leader_loss(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	run(4000);
	power_cut(a);
	run(2900);
	CHECK(follows(b, a) && L.b[b].lost == 0);
	run(300);
	CHECK(L.b[b].lost == 1 && L.b[c].lost == 1);
	CHECK(role(b) == HIFI_PARTY_ROLE_LISTENING && L.b[b].g.orphan);
	CHECK_STR(L.b[b].g.why, "leader lost");
	CHECK(!L.b[b].g.dead[0].group); /* it had CLOCKs: no dead group */
	run(20000);
	CHECK(leaders() == 0); /* a follower never leads silence by itself */

	power_on(a); /* the old leader comes back: the group re-forms */
	run(2600);
	CHECK(role(a) == HIFI_PARTY_ROLE_LEADER && L.b[a].g.term == 2 && !L.b[a].g.picked);
	run(1500);
	CHECK(follows(b, a) && follows(c, a) && L.b[b].g.clocked && !L.b[b].g.orphan);

	/* lost again; this time another board is asked to lead */
	power_cut(a);
	run(3500);
	CHECK(role(b) == HIFI_PARTY_ROLE_LISTENING && role(c) == HIFI_PARTY_ROLE_LISTENING);
	CHECK(lead(b) > 0);
	run(1500);
	CHECK(follows(c, b));
	power_on(a);
	run(4000);
	CHECK(follows(a, b) && follows(c, b) && leaders() == 1);
}

static void power_cuts(void) {
	/* a picked leader back from a power cut leads again */
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(a);
	switch_on(b);
	run(4000);
	int first = role(a) == HIFI_PARTY_ROLE_LEADER ? a : b, other = first == a ? b : a;
	CHECK(leaders() == 1 && follows(other, first));
	run(5000);
	CHECK(lead(a) > 0 || role(a) == HIFI_PARTY_ROLE_LEADER);
	run(3000);
	CHECK(follows(b, a) && L.b[a].g.picked && L.b[a].nvs_picked);
	int64_t term = L.b[a].g.term;
	CHECK(L.b[a].nvs_term == term);
	power_cut(a);
	run(5000);
	power_on(a);
	CHECK(L.b[a].g.picked && L.b[a].g.max_term == term);
	run(3000);
	CHECK(role(a) == HIFI_PARTY_ROLE_LEADER && L.b[a].g.picked && L.b[a].g.term == term + 1);
	run(2000);
	CHECK(follows(b, a));

	/* ...unless another board was picked meanwhile */
	power_cut(a);
	run(3500);
	CHECK(lead(b) > 0);
	power_on(a);
	run(4000);
	CHECK(follows(a, b) && !L.b[a].g.picked && !L.b[a].nvs_picked);

	/* ...and takes it back from a lead nobody picked */
	run(6000);
	CHECK(lead(a) > 0);
	run(2000);
	CHECK(follows(b, a));
	power_cut(a);
	run(3500); /* b is an orphan now; c switched on leads, not picked */
	switch_on(c);
	run(5000);
	CHECK(role(c) == HIFI_PARTY_ROLE_LEADER && !L.b[c].g.picked && follows(b, c));
	power_on(a);
	run(4000);
	CHECK(role(a) == HIFI_PARTY_ROLE_LEADER && L.b[a].g.picked);
	CHECK(follows(b, a) && follows(c, a) && leaders() == 1);
}

static void replays(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), x = add("X", 20, 9); /* x has another secret */
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(x);
	run(4000);
	CHECK(follows(b, a) && L.b[a].g.npeers == 1);
	/* a board without the secret: not followed, not joined, counted */
	CHECK(role(x) == HIFI_PARTY_ROLE_LEADER && L.b[x].g.npeers == 0);
	CHECK(L.b[a].bad_tag > 0 && L.b[b].bad_tag > 0 && L.b[x].bad_tag > 0);
	power_cut(x);

	/* a JOIN played back after a leave, and the leave played back after a re-join */
	dgram_t join = L.b[b].last[HIFI_PARTY_JOIN];
	switch_off(b);
	run(20);
	CHECK(L.b[a].g.npeers == 0);
	uint32_t rep = L.b[a].g.replays;
	inject(&join);
	run(20);
	CHECK(L.b[a].g.npeers == 0 && L.b[a].g.replays == rep + 1);
	dgram_t leave = L.b[b].last_leave;
	switch_on(b);
	run(4000);
	CHECK(follows(b, a) && L.b[a].g.npeers == 1);
	inject(&leave); /* wrong group as well as old: dropped either way */
	run(20);
	CHECK(L.b[a].g.npeers == 1);

	/* played back inside the TSF window: the per-sender TSF catches them */
	join = L.b[b].last[HIFI_PARTY_JOIN];
	rep = L.b[a].g.replays;
	inject(&join);
	run(20);
	CHECK(L.b[a].g.npeers == 1 && L.b[a].g.replays == rep + 1);
	switch_off(b);
	run(20);
	CHECK(L.b[a].g.npeers == 0);
	leave = L.b[b].last_leave;
	switch_on(b);
	run(4000);
	CHECK(follows(b, a) && L.b[a].g.npeers == 1);
	inject(&join); /* both older than b's last JOIN */
	inject(&leave);
	run(20);
	CHECK(L.b[a].g.npeers == 1);

	/* a CLOCK played back can't keep a gone leader alive */
	dgram_t clock = L.b[a].last[HIFI_PARTY_CLOCK];
	dgram_t ann = L.b[a].last[HIFI_PARTY_ANNOUNCE];
	int64_t ann_tsf = L.ms * 1000;
	power_cut(a);
	for (int i = 0; i < 8; i++) {
		inject(&clock);
		run(400);
	}
	CHECK(L.b[b].lost == 1 && role(b) == HIFI_PARTY_ROLE_LISTENING);

	/* an announce played back, again and again: its TSF is off ours but
	 * never moves on, so it isn't taken for another AP */
	run(5000);
	for (int i = 0; i < 3; i++) {
		inject(&ann);
		run(1000);
	}
	CHECK(L.b[b].g.other_aps == 0 && !L.b[b].g.other_ap && role(b) == HIFI_PARTY_ROLE_LISTENING);
	/* the AP restarts and its TSF comes round to the capture's: joined,
	 * no CLOCK, so the group is dead and its announces skipped after */
	run(3000);
	L.ap_tsf[0] = ann_tsf - L.ms * 1000;
	inject(&ann);
	run(20);
	CHECK(follows(b, a));
	run(3100);
	CHECK(role(b) == HIFI_PARTY_ROLE_LISTENING && dead_mark(b, L.b[a].g.group) && dead_mark(b, L.b[a].g.group)->live);
	CHECK(L.b[b].lost == 2);
	rep = L.b[b].g.replays;
	L.ap_tsf[0] = ann_tsf - L.ms * 1000;
	inject(&ann);
	run(3000);
	CHECK(role(b) == HIFI_PARTY_ROLE_LISTENING && L.b[b].g.replays == rep + 1 && L.b[b].lost == 2);

	/* the real leader back, under a higher term: followed */
	power_on(a);
	run(4000);
	CHECK(follows(b, a) && L.b[b].g.clocked);
}

static void flood(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	run(4000);
	hifi_link_key_t junk;
	uint8_t k[HIFI_LINK_KEY] = {7};
	hifi_link_key_init(&junk, k);
	int tags = L.b[a].bad_tag;
	for (int s = 0; s < 1000; s++) { /* 5000 a second, from an address outside the group */
		for (int i = 0; i < 50; i++) {
			dgram_t d = {.src = NET | 99, .dst = BCAST};
			hifi_party_pkt_t p = {.type = (uint8_t) (i % 2 ? HIFI_PARTY_ANNOUNCE : HIFI_PARTY_CLOCK), .group = (uint32_t) i,
					      .k = 1};
			uint8_t pl[HIFI_PARTY_ANNOUNCE_LEN] = {0};
			d.len = (uint16_t) hifi_party_encode(&junk, &p, pl, d.buf, sizeof(d.buf));
			inject(&d);
		}
		step();
	}
	CHECK(L.b[b].lost == 0 && L.b[c].lost == 0 && L.b[a].g.npeers == 2);
	CHECK(L.b[a].g.dropped > 40000);
	CHECK(L.b[a].bad_tag - tags <= 11 * HIFI_PARTY_STRANGER_SRC_PER_S); /* one address: hashed at most 4 a second */
}

static void local_source(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	run(4000);
	/* the leader's own source: the lead is picked, nothing else changes */
	uint32_t fb = L.b[b].g.follows, grp = L.b[a].g.group;
	L.b[a].source = true;
	run(2000);
	CHECK(role(a) == HIFI_PARTY_ROLE_LEADER && L.b[a].g.picked && L.b[a].g.group == grp);
	CHECK(L.b[b].g.follows == fb && L.b[b].g.leader_picked && L.b[b].lost == 0);
	/* a follower's: the lead goes there */
	run(4000);
	L.b[c].source = true;
	run(3000);
	CHECK(role(c) == HIFI_PARTY_ROLE_LEADER && L.b[c].g.picked);
	CHECK(follows(a, c) && follows(b, c) && !L.b[a].source);
	run(3000);
	L.b[b].source = true;
	run(3000);
	CHECK(role(b) == HIFI_PARTY_ROLE_LEADER && follows(a, b) && follows(c, b) && !L.b[c].source);
	uint32_t f[3] = {L.b[a].g.follows, L.b[b].g.follows, L.b[c].g.follows};
	run(20000);
	CHECK(L.b[a].g.follows == f[0] && L.b[b].g.follows == f[1] && L.b[c].g.follows == f[2] && leaders() == 1);
}

static void line_in(void) {
	/* a follower set to input=line: line in takes the output when a test
	 * signal ends. The board stays a follower and takes the output back;
	 * line in doesn't lead the group from it */
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	run(4000);
	CHECK(follows(b, a));
	uint32_t fb = L.b[b].g.follows, la = L.b[a].g.leads;
	for (int i = 0; i < 20; i++) {
		L.b[b].line = true;
		CHECK(!hifi_party_local_source(role(b), 6) && hifi_party_retakes(role(b), 6));
		run(1000);
		CHECK(follows(b, a) && !L.b[b].line);
	}
	CHECK(role(a) == HIFI_PARTY_ROLE_LEADER && L.b[a].g.leads == la && L.b[b].g.follows == fb);
	CHECK(L.b[b].lost == 0 && leaders() == 1);
	/* off the party, line in is the board's own source and leads */
	switch_off(b);
	run(1000);
	switch_on(b);
	L.b[b].line = true;
	run(1);
	CHECK(role(b) == HIFI_PARTY_ROLE_LEADER && L.b[b].g.picked);
	run(4000);
	CHECK(follows(a, b) && leaders() == 1);
}

static void output_stopped(void) {
	/* the leader's output stopped, or at 48 kHz: CLOCKs with "no audio" keep the group */
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	run(4000);
	L.noaudio = true;
	run(20000);
	CHECK(follows(b, a) && L.b[b].lost == 0 && L.b[b].noaudio_seen);
}

static void low_heap(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	run(4000);
	L.b[a].low_heap = true;
	run(20);
	CHECK(role(a) == HIFI_PARTY_ROLE_LISTENING && L.b[a].g.low_heap == 1);
	CHECK_STR(L.b[a].g.why, "low memory");
	CHECK(lead(a) == HIFI_PARTY_E_OFF);
	run(3500);
	CHECK(L.b[b].lost == 1 && L.b[c].lost == 1 && leaders() == 0);
	L.b[a].low_heap = false;
	run(10000);
	CHECK(leaders() == 0);
	CHECK(lead(b) > 0);
	run(3000);
	CHECK(follows(a, b) && follows(c, b));
}

static void other_ap(void) {
	reset(0);
	L.ap_tsf[1] = 60 * 1000000LL;
	int a = add("A", 10, 1), b = add("B", 11, 1), d = add("D", 13, 1);
	L.b[d].ap = 1;
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(d);
	run(10000);
	CHECK(follows(b, a) && role(d) == HIFI_PARTY_ROLE_LISTENING && L.b[d].g.other_aps > 5);
	CHECK_STR(L.b[d].g.why, "another access point");
	power_cut(a);
	run(6000); /* no leader heard on its own AP: it leads */
	CHECK(role(d) == HIFI_PARTY_ROLE_LEADER);
	CHECK(role(b) == HIFI_PARTY_ROLE_LISTENING); /* and b doesn't follow across APs */
}

static void lossy(void) {
	reset(0);
	L.loss = 20; /* 2% of datagrams lost */
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1), d = add("D", 13, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	switch_on(c);
	switch_on(d);
	run(5000);
	run(120000);
	CHECK(follows(b, a) && follows(c, a) && follows(d, a) && leaders() == 1);
	CHECK(L.b[b].lost + L.b[c].lost + L.b[d].lost == 0);
}

/* run until the follower's role is r, at most max ms: the ms it took, or -1 */
static int until_role(int i, hifi_party_role_t r, uint32_t max) {
	for (uint32_t t = 0; t <= max; t += STEP) {
		if (role(i) == r) return (int) t;
		step();
	}
	return -1;
}

static void backoff(uint32_t base);

/* a follower that never had a CLOCK from its leader (the leader is real,
 * its CLOCKs were lost) skips it for a while, then joins it again by itself */
static void missed_clock(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1);
	switch_on(a);
	run(3000);
	L.b[b].deaf_clock = true;
	switch_on(b);
	CHECK(until_role(b, HIFI_PARTY_ROLE_FOLLOWER, 4000) >= 0);
	CHECK(until_role(b, HIFI_PARTY_ROLE_LISTENING, 4000) >= 0);
	const hifi_party_dead_t *m = dead_mark(b, L.b[a].g.group);
	CHECK(L.b[b].lost == 1 && L.b[b].g.orphan && m && m->live && m->ms == HIFI_PARTY_DEAD_MS);
	CHECK(hifi_party_group_skip_ms(&L.b[b].g, L.now) > HIFI_PARTY_DEAD_MS - 1000);
	L.b[b].deaf_clock = false; /* the air clears */
	uint32_t rep = L.b[b].g.replays;
	run(HIFI_PARTY_DEAD_MS - 500);
	CHECK(role(b) == HIFI_PARTY_ROLE_LISTENING && L.b[b].g.replays > rep); /* still skipped */
	CHECK(until_role(b, HIFI_PARTY_ROLE_FOLLOWER, 1600) >= 0); /* the next announce after the wait */
	run(1000);
	CHECK(follows(b, a) && L.b[b].g.clocked && L.b[a].g.npeers == 1);
	CHECK(!dead_mark(b, L.b[a].g.group) && hifi_party_group_skip_ms(&L.b[b].g, L.now) == 0); /* a real leader: forgotten */
	run(30000);
	CHECK(follows(b, a) && L.b[b].lost == 1);

	backoff(0);
}

/* the CLOCKs stay lost: each new try waits twice as long, up to the cap;
 * the same across the ms clock's wrap, and skip_ms never more than the wait */
static void backoff(uint32_t base) {
	reset(base);
	int a, b;
	a = add("A", 10, 1);
	b = add("B", 11, 1);
	switch_on(a);
	run(3000);
	L.b[b].deaf_clock = true;
	switch_on(b);
	CHECK(until_role(b, HIFI_PARTY_ROLE_FOLLOWER, 4000) >= 0);
	CHECK(until_role(b, HIFI_PARTY_ROLE_LISTENING, 4000) >= 0);
	uint32_t want[] = {HIFI_PARTY_DEAD_MS, 2 * HIFI_PARTY_DEAD_MS, 4 * HIFI_PARTY_DEAD_MS, HIFI_PARTY_DEAD_MAX_MS,
			   HIFI_PARTY_DEAD_MAX_MS};
	for (int k = 0; k < 5; k++) {
		CHECK(dead_mark(b, L.b[a].g.group)->ms == want[k] && L.b[b].lost == k + 1);
		CHECK(hifi_party_group_skip_ms(&L.b[b].g, L.now) <= want[k]);
		int t = until_role(b, HIFI_PARTY_ROLE_FOLLOWER, want[k] + 1600);
		CHECK(t >= (int) want[k] && t <= (int) want[k] + 1100);
		CHECK(until_role(b, HIFI_PARTY_ROLE_LISTENING, 4000) >= 0);
	}
	CHECK(leaders() == 1 && role(a) == HIFI_PARTY_ROLE_LEADER); /* the orphan never leads */
	L.b[b].deaf_clock = false;
	CHECK(until_role(b, HIFI_PARTY_ROLE_FOLLOWER, HIFI_PARTY_DEAD_MAX_MS + 1600) >= 0);
	run(1000);
	CHECK(follows(b, a) && L.b[b].g.clocked && !dead_mark(b, L.b[a].g.group));
}

/* a mark that expired with no announce after it stays expired, however
 * long, across the ms clock's wrap */
static void dead_wrap(void) {
	hifi_party_group_t g;
	hifi_party_group_init(&g, 0x0a00000b, MASK, 0, false);
	g.dead[0] = (hifi_party_dead_t){.group = 5, .term = 1, .at = 0xfffff000u, .ms = HIFI_PARTY_DEAD_MS, .live = true};
	CHECK(hifi_party_group_skip_ms(&g, 0xfffff000u + 4000) == HIFI_PARTY_DEAD_MS - 4000);
	CHECK(hifi_party_group_skip_ms(&g, 0x7ffff000u) == 0); /* 24.8 days on, never live again */
	CHECK(hifi_party_group_skip_ms(&g, 0xffffe000u) == 0); /* almost 49.7 days on */
	hifi_party_group_tick(&g, 0xfffff000u + HIFI_PARTY_DEAD_MS, 1, false, false);
	CHECK(!g.dead[0].live && g.dead[0].ms == HIFI_PARTY_DEAD_MS); /* expired by tick, its wait kept */
	CHECK(hifi_party_group_skip_ms(&g, 0xfffff000u + 100) == 0); /* 49.7 days on */
}

/* announces of old groups played back over and over: each costs a short
 * follow at most once a minute, however many take turns, up to the slots */
static uint32_t played_back(int n) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	run(4000);
	dgram_t anns[HIFI_PARTY_DEAD_SLOTS];
	int64_t tsfs[HIFI_PARTY_DEAD_SLOTS];
	for (int k = 0; k < n; k++) { /* each a group of its own */
		anns[k] = L.b[a].last[HIFI_PARTY_ANNOUNCE];
		tsfs[k] = L.ms * 1000;
		switch_off(a);
		run(5000);
		switch_on(a);
		run(6000);
	}
	power_cut(a);
	run(8000);
	uint32_t f0 = L.b[b].g.follows;
	for (int i = 0; i < 300; i++) {
		L.ap_tsf[0] = tsfs[i % n] - L.ms * 1000; /* the AP's TSF restarted where the announce was sent */
		inject(&anns[i % n]);
		run(1000);
	}
	CHECK(leaders() == 0);
	return L.b[b].g.follows - f0;
}

static void played_back_groups(void) {
	uint32_t one = played_back(1);
	CHECK(one >= 1 && one <= 300 / (HIFI_PARTY_DEAD_MAX_MS / 1000) + 3);
	for (int n = 2; n <= HIFI_PARTY_DEAD_SLOTS; n++) {
		uint32_t f = played_back(n);
		printf("played back: %d groups, %u follows in 300 s (one group: %u)\n", n, f, one);
		CHECK(f <= (uint32_t) n * one + 1); /* each group its own wait */
	}
}

/* B has just joined a real, idle leader A and has no CLOCK yet when a picked
 * announce of an old group C is played back to it: B hops to C, but A isn't
 * marked dead for it, so B is back with A as soon as C is lost */
static void hop_just_joined(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1), c = add("C", 12, 1);
	switch_on(c);
	run(3000);
	lead(c);
	run(2000);
	dgram_t ann = L.b[c].last[HIFI_PARTY_ANNOUNCE];
	int64_t tsf = L.ms * 1000;
	power_cut(c);
	run(8000);
	switch_on(a);
	run(3000);
	switch_on(b);
	CHECK(until_role(b, HIFI_PARTY_ROLE_FOLLOWER, 6000));
	CHECK(!L.b[b].g.clocked);
	uint32_t t0 = L.now;
	L.ap_tsf[0] = tsf - L.ms * 1000;
	ann.dst = L.b[b].ip; /* to B only: A stays */
	inject(&ann);
	run(1);
	CHECK(L.b[b].g.leader_ip == L.b[c].ip);
	CHECK(hifi_party_group_skip_ms(&L.b[b].g, L.now) == 0); /* A not marked */
	while (!(follows(b, a) && L.b[b].g.clocked) && L.now - t0 < 60000) run(10);
	CHECK(follows(b, a) && L.b[b].g.clocked);
	CHECK(L.now - t0 < 25000); /* C lost after 3 s, A rejoined at its next announce */
}

static void new_address(void) {
	reset(0);
	int a = add("A", 10, 1), b = add("B", 11, 1);
	switch_on(a);
	run(3000);
	switch_on(b);
	run(4000);
	CHECK(follows(b, a));
	L.b[b].ip = NET | 50;
	int r = hifi_party_group_set_ip(&L.b[b].g, L.now, L.b[b].ip, MASK, 1);
	CHECK(r == (HIFI_PARTY_DO_ROLE | HIFI_PARTY_DO_LOST) && role(b) == HIFI_PARTY_ROLE_LISTENING && !L.b[b].g.orphan);
	act(&L.b[b], r);
	run(4000);
	CHECK(follows(b, a) && serves(a, b));
	CHECK(hifi_party_group_set_ip(&L.b[b].g, L.now, L.b[b].ip, MASK, 1) == 0);
}

static void units(void) {
	uint32_t n = NET;
	CHECK(hifi_party_same_subnet(n | 5, n | 10, MASK));
	CHECK(!hifi_party_same_subnet(n | 10, n | 10, MASK));
	CHECK(!hifi_party_same_subnet(n | 0, n | 10, MASK) && !hifi_party_same_subnet(n | 255, n | 10, MASK));
	CHECK(!hifi_party_same_subnet(0xc0a80205u, n | 10, MASK));
	CHECK(hifi_party_same_subnet(0xc0a80005u, n | 10, 0xfffffe00u)); /* a /23 */
	CHECK(!hifi_party_same_subnet(0xc0a80205u, n | 10, 0xfffffe00u));
	CHECK(!hifi_party_same_subnet(0xc0a801ffu, n | 10, 0xfffffe00u)); /* inside a /23: a host */
	CHECK(hifi_party_same_subnet(0xc0a801ffu, 0xc0a80010u, 0xfffffe00u) == false); /* 192.168.0.0/23 ends at 1.255 */
	CHECK(hifi_party_same_subnet(0x0a000001u, 0x0a000000u, 0xfffffffeu)); /* a /31 has no broadcast */
	CHECK(!hifi_party_same_subnet(0, n | 10, MASK));

	uint8_t s1[32], s2[32], k1[16], k2[16], k3[16];
	memset(s1, 1, 32);
	memset(s2, 2, 32);
	hifi_party_group_key(s1, 7, k1);
	hifi_party_group_key(s1, 8, k2);
	hifi_party_group_key(s2, 7, k3);
	CHECK(memcmp(k1, k2, 16) && memcmp(k1, k3, 16));
	hifi_party_group_key(s1, 7, k2);
	CHECK(!memcmp(k1, k2, 16));
	hifi_party_announce_key(s1, k2);
	CHECK(memcmp(k1, k2, 16));
	CHECK(hifi_party_secret_id(s1) == hifi_party_secret_id(s1));
	{ /* the secret as the Hive gives it: 64 hex digits, nothing else */
		char hex[65];
		uint8_t got[32], want[32];
		for (int i = 0; i < 32; i++) {
			want[i] = (uint8_t) (i * 37 + 5);
			snprintf(hex + 2 * i, 3, i & 1 ? "%02X" : "%02x", want[i]);
		}
		CHECK(hifi_party_parse_secret(hex, got) && !memcmp(got, want, 32));
		memset(got, 0x55, 32);
		hex[63] = 'g';
		CHECK(!hifi_party_parse_secret(hex, got));
		hex[63] = 0;
		CHECK(!hifi_party_parse_secret(hex, got));
		CHECK(!hifi_party_parse_secret("", got) && !hifi_party_parse_secret(NULL, got));
		memset(hex, '0', 64);
		hex[64] = 0;
		CHECK(!hifi_party_parse_secret(hex, got)); /* all zeros is a blank */
		CHECK(got[0] == 0x55 && got[31] == 0x55); /* untouched on a failure */
		hex[5] = '1';
		CHECK(hifi_party_parse_secret(hex, got) && got[2] == 0x01 && got[0] == 0);
	}
	/* HMAC-SHA256 itself, RFC 4231 case 2 does not fit a 32 B key; check
	 * the construction against a key of 0x0b * 32 computed with OpenSSL:
	 * HMAC(key, "hifi-party-id-v1")[0] */
	CHECK_STR(hifi_party_role_name(HIFI_PARTY_ROLE_OFF), "off");
	CHECK_STR(hifi_party_role_name(HIFI_PARTY_ROLE_LISTENING), "listening");
	CHECK_STR(hifi_party_role_name(HIFI_PARTY_ROLE_LEADER), "leader");

	/* announce fields */
	hifi_party_group_t g;
	hifi_party_group_init(&g, n | 10, MASK, 4, false);
	CHECK(hifi_party_group_on(&g, 0, 1) == HIFI_PARTY_DO_ROLE);
	CHECK(hifi_party_group_on(&g, 0, 1) == 0);
	CHECK(hifi_party_group_lead(&g, 0, 77) > 0 && g.term == 5 && g.group == 77 && g.dirty);
	g.npeers = 1;
	g.peers[0] = n | 11;
	hifi_party_pkt_t p;
	uint8_t pl[HIFI_PARTY_ANNOUNCE_LEN];
	hifi_party_announce_build(&g, 5000000, "Living\x01room speaker too long", &p, pl);
	CHECK(p.type == HIFI_PARTY_ANNOUNCE && p.group == 77 && p.k == 5 && p.n == 1 && p.flags == HIFI_PARTY_F_PICKED);
	CHECK(pl[0] == 192 && pl[1] == 168 && pl[2] == 1 && pl[3] == 10 && pl[7] == 11 && pl[8] == 0 && pl[11] == 0);
	hifi_party_announce_t a;
	CHECK(hifi_party_announce_parse(&p, pl, sizeof(pl), n | 10, n | 20, MASK, 5000000, &a) == 1);
	CHECK(a.ip == (n | 10) && a.group == 77 && a.term == 5 && a.picked && a.n == 1 && a.followers[0] == (n | 11));
	CHECK_STR(a.name, "Living?room spea");
	CHECK(hifi_party_announce_parse(&p, pl, sizeof(pl), n | 10, n | 20, MASK, 7000001, &a) == -1);
	CHECK(hifi_party_announce_parse(&p, pl, sizeof(pl), n | 10, n | 20, MASK, 3000000, &a) == 1);
	CHECK(hifi_party_announce_parse(&p, pl, sizeof(pl), n | 12, n | 20, MASK, 5000000, &a) == 0); /* not its IP */
	CHECK(hifi_party_announce_parse(&p, pl, sizeof(pl), n | 10, 0xc0a80214u, MASK, 5000000, &a) == 0); /* another subnet */
	CHECK(hifi_party_announce_parse(&p, pl, sizeof(pl) - 1, n | 10, n | 20, MASK, 5000000, &a) == 0);
	hifi_party_pkt_t q = p;
	q.k = 0;
	CHECK(hifi_party_announce_parse(&q, pl, sizeof(pl), n | 10, n | 20, MASK, 5000000, &a) == 0);
	q = p;
	q.group = 0;
	CHECK(hifi_party_announce_parse(&q, pl, sizeof(pl), n | 10, n | 20, MASK, 5000000, &a) == 0);
	q = p;
	q.type = HIFI_PARTY_JOIN;
	CHECK(hifi_party_announce_parse(&q, pl, sizeof(pl), n | 10, n | 20, MASK, 5000000, &a) == 0);

	/* JOIN fields */
	uint8_t jp[4];
	g.role = HIFI_PARTY_ROLE_FOLLOWER;
	g.leave_group = 99;
	hifi_party_join_build(&g, 1000, false, 1, &p, jp);
	CHECK(p.type == HIFI_PARTY_JOIN && p.group == 77 && p.flags == 0 && p.t == 1000);
	int64_t t;
	bool leave;
	CHECK(hifi_party_join_parse(&p, jp, 4, n | 10, n | 1, MASK, 1000, &t, &leave) && t == 1000 && !leave);
	CHECK(!hifi_party_join_parse(&p, jp, 4, n | 11, n | 1, MASK, 1000, &t, &leave));
	CHECK(!hifi_party_join_parse(&p, jp, 4, n | 10, n | 1, MASK, 3000000, &t, &leave));
	CHECK(!hifi_party_join_parse(&p, jp, 3, n | 10, n | 1, MASK, 1000, &t, &leave));
	hifi_party_join_build(&g, 2000, true, 2, &p, jp);
	CHECK(p.group == 99 && p.flags == HIFI_PARTY_F_LEAVE);
	CHECK(hifi_party_join_parse(&p, jp, 4, n | 10, n | 1, MASK, 2000, &t, &leave) && leave);

	/* the leader's JOIN table: rising TSF, three places */
	hifi_party_group_init(&g, n | 1, MASK, 0, false);
	hifi_party_group_on(&g, 0, 1);
	hifi_party_group_lead(&g, 0, 5);
	for (uint32_t i = 2; i <= 9; i++) CHECK(hifi_party_group_join(&g, 0, n | i, 100, false) == (i <= 4));
	CHECK(g.npeers == 3 && g.refused == 5);
	CHECK(!hifi_party_group_join(&g, 0, n | 2, 100, false) && g.replays == 1);
	CHECK(hifi_party_group_join(&g, 0, n | 2, 101, true) && g.npeers == 2);
	CHECK(hifi_party_group_join(&g, 0, n | 9, 200, false) && g.npeers == 3);
	for (uint32_t i = 10; i <= 40; i++) hifi_party_group_join(&g, 0, n | i, 100, false); /* strangers churn the table */
	CHECK(!hifi_party_group_join(&g, 0, n | 9, 200, false)); /* a follower's last TSF is kept */
	CHECK(hifi_party_group_nack_ok(&g, n | 9, 5) && !hifi_party_group_nack_ok(&g, n | 9, 5) && hifi_party_group_nack_ok(&g, n | 9, 6));
	CHECK(!hifi_party_group_nack_ok(&g, n | 40, 1));

	/* the board's own source: never idle, the test signal or the party; line
	 * in except on a follower, which takes the output back from it */
	for (int r = HIFI_PARTY_ROLE_OFF; r <= HIFI_PARTY_ROLE_FOLLOWER; r++) {
		for (int src = 0; src <= 7; src++) {
			bool own = src >= 1 && src <= 4 ? true : src == 6 ? r != HIFI_PARTY_ROLE_FOLLOWER : false;
			CHECK(hifi_party_local_source((hifi_party_role_t) r, src) == own);
			CHECK(hifi_party_retakes((hifi_party_role_t) r, src) == (r == HIFI_PARTY_ROLE_FOLLOWER && (src == 0 || src == 6)));
		}
	}
	/* the stranger budget */
	int ok = 0;
	for (int i = 0; i < 100; i++) ok += hifi_party_group_admit(&g, 5000, n | 50, HIFI_PARTY_JOIN);
	CHECK(ok == HIFI_PARTY_STRANGER_SRC_PER_S); /* one address gets its share */
	for (uint32_t i = 60; i < 100; i++) ok += hifi_party_group_admit(&g, 5000, n | i, HIFI_PARTY_ANNOUNCE);
	CHECK(ok == HIFI_PARTY_STRANGERS_PER_S); /* the others the rest, up to the cap */
	CHECK(hifi_party_group_admit(&g, 5000, n | 9, HIFI_PARTY_NACK)); /* a follower, past the budget */
	CHECK(!hifi_party_group_admit(&g, 5000, n | 50, HIFI_PARTY_NACK));
	CHECK(!hifi_party_group_admit(&g, 5000, n | 50, HIFI_PARTY_AUDIO));
	CHECK(hifi_party_group_admit(&g, 6000, n | 50, HIFI_PARTY_ANNOUNCE));
	CHECK(hifi_party_group_off(&g) == HIFI_PARTY_DO_ROLE && !hifi_party_group_admit(&g, 9000, n | 9, HIFI_PARTY_JOIN));

	/* another AP: two announces from one address whose TSF moves on with ours */
	hifi_party_group_init(&g, n | 1, MASK, 0, false);
	int64_t t0 = 900000000;
	for (int i = 0; i < 5; i++) hifi_party_group_other_ap(&g, 1000u + 1000u * (uint32_t) i, n | 7, t0); /* one replayed */
	CHECK(!g.other_ap && g.other_aps == 0);
	hifi_party_group_other_ap(&g, 7000, n | 7, t0 + 5000000); /* moved on, but not by the 2 s since */
	CHECK(!g.other_ap);
	hifi_party_group_other_ap(&g, 8000, n | 8, t0 + 6000000); /* another address */
	CHECK(!g.other_ap);
	hifi_party_group_other_ap(&g, 8500, n | 8, t0 + 1000000); /* an older one in between */
	hifi_party_group_other_ap(&g, 9000, n | 8, t0 + 7000000 + 30000);
	CHECK(g.other_ap && g.other_ap_at == 9000 && g.other_aps == 1);
	hifi_party_group_other_ap(&g, 9000, n | 8, t0 + 7000000 + 30000); /* the same again */
	hifi_party_group_other_ap(&g, 10000, n | 8, t0 + 8000000);
	CHECK(g.other_aps == 2 && g.other_ap_at == 10000);
	hifi_party_group_other_ap(&g, 14000, n | 8, t0 + 12000000); /* after HIFI_PARTY_LOSS_MS: starts over */
	CHECK(g.other_aps == 2);

	/* times far apart, and a ms clock about to wrap: a fresh state counts as long ago */
	hifi_party_group_init(&g, n | 1, MASK, 0, false);
	hifi_party_group_on(&g, 0xfffff000u, 1);
	CHECK(hifi_party_group_admit(&g, 0xfffff000u, n | 50, HIFI_PARTY_JOIN));
	CHECK(hifi_party_group_lead(&g, 0xfffff000u, 5) > 0);
	CHECK(hifi_party_group_lead(&g, 0xfffff000u + 4000u, 5) == HIFI_PARTY_E_SOON); /* across the wrap */
	CHECK(hifi_party_group_lead(&g, 0xfffff000u + 25u * 86400000u, 5) == HIFI_PARTY_DO_ANNOUNCE); /* 25 days on */
}

int main(void) {
	units();
	first_on(0);
	first_on(0xfffff000u); /* the ms clock wraps under it */
	join_leave_full();
	leads();
	leader_loss();
	power_cuts();
	replays();
	flood();
	local_source();
	line_in();
	output_stopped();
	low_heap();
	other_ap();
	lossy();
	missed_clock();
	backoff(0xffffffffu - 20000);
	backoff(0xffffffffu - 100000);
	dead_wrap();
	played_back_groups();
	hop_just_joined();
	new_address();
	return check_report("party_group");
}
