/*
 * Party mode you can switch on: how boards find a group, join it, and agree
 * on who leads (projects/hifi-lite-lab/party-mode.md, section 8). Plain C
 * with no radio, so the host tests run a whole simulated LAN of boards.
 *
 * Keys. The Hive gives every board in the house the same 32-byte party
 * secret. A leader draws a random group id each time it starts leading; the
 * group's key (AUDIO, CLOCK, NACK, JOIN) is HMAC-SHA256(secret,
 * "hifi-party-v1" | group) cut to 16 bytes, and announces go under
 * HMAC-SHA256(secret, "hifi-party-announce-v1"), cut the same way. So a
 * board without the secret can't join or be joined, and no key crosses the
 * LAN.
 *
 * Roles. Off; listening (switched on, waiting to hear a leader); leader;
 * follower. A board switched on (or booted on) listens HIFI_PARTY_LISTEN_MS
 * plus up to HIFI_PARTY_LISTEN_JITTER_MS and follows the best leader it
 * heard; if it heard none, it leads, so the first board on leads. A leader
 * announces once a second; a follower sends its leader a JOIN once a second
 * (the join and the keepalive), and the leader sends audio and CLOCKs to the
 * up to HIFI_PARTY_FOLLOWERS it heard from in the last HIFI_PARTY_LOSS_MS.
 *
 * Who leads. An announce ranks by (picked, term, IPv4). A lead is picked
 * when it was asked for (Lead, or a source starting on the board: the lead
 * goes where the music is); the first-on lead is not. A new lead takes the
 * term one past the highest the board has held or heard. A leader that
 * hears a higher rank follows it, and a follower moves to it, so a Lead or a
 * cast moves the whole group. The term and the pick survive a reboot (the
 * caller keeps them when dirty is set). A board takes a Lead at most once
 * per HIFI_PARTY_LEAD_GAP_MS.
 *
 * Liveness. A follower counts its leader alive only while CLOCKs come (they
 * go to it alone, under the group key); an announce proves nothing (an old
 * one can be played back once an AP's TSF restarts). A follower that hears
 * no CLOCK for HIFI_PARTY_LOSS_MS mutes and listens again, but never leads
 * by itself (it would lead silence); and if it never had one CLOCK from that
 * group, the group is marked dead and its announces are skipped for
 * HIFI_PARTY_DEAD_MS, or until one comes with a higher term. The mark
 * expires so that a real leader whose CLOCKs were lost (a busy channel, a
 * JOIN that didn't get through) is joined again; each time the same group
 * and term is marked again the wait doubles, up to HIFI_PARTY_DEAD_MAX_MS,
 * so an announce played back again and again costs a short follow once a
 * minute at most. Up to HIFI_PARTY_DEAD_SLOTS groups are marked at once,
 * each with its own wait, so old groups played back in turn don't reset
 * each other's. A CLOCK from the group clears its mark. A follower the
 * leader has no room for (not in the announce's list) keeps sending JOINs
 * and waits, alive while the leader's announces come.
 *
 * IPv4 addresses are host-order uint32 here; times are ms from any clock
 * that wraps at 2^32 (compared by difference).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hifi_party.h"

#define HIFI_PARTY_SECRET 32
#define HIFI_PARTY_LISTEN_MS 2000u
#define HIFI_PARTY_LISTEN_JITTER_MS 500u
#define HIFI_PARTY_ANNOUNCE_MS 1000u   /* a leader's announce, a follower's JOIN */
#define HIFI_PARTY_LOSS_MS 3000u       /* a follower, or a leader's CLOCK, not heard this long is gone */
#define HIFI_PARTY_LEAD_GAP_MS 5000u   /* the shortest time between two leads a board takes */
#define HIFI_PARTY_TSF_SKEW_US 2000000 /* an announce whose TSF is further from ours: a replay, or another AP */
#define HIFI_PARTY_STRANGERS_PER_S 20  /* packets from outside the group hashed per second */
#define HIFI_PARTY_STRANGER_SRC_PER_S 4 /* of those, from any one address */
#define HIFI_PARTY_STRANGER_SRCS 8     /* addresses whose share the board keeps */
#define HIFI_PARTY_OTHER_AP_SLACK_US 500000 /* an off-TSF announce's clock may run this far from ours between two */
#define HIFI_PARTY_JOIN_SEEN 8         /* JOIN senders whose last TSF the leader keeps */
#define HIFI_PARTY_DEAD_MS 10000u      /* a group joined with no CLOCK is skipped this long */
#define HIFI_PARTY_DEAD_MAX_MS 60000u  /* the same group and term again: the wait doubles up to this */
#define HIFI_PARTY_DEAD_SLOTS 4        /* groups marked dead at once */
#define HIFI_PARTY_HOP_MARK_MS 500u    /* a group left this long after joining, with no CLOCK, is marked dead */

typedef enum {
	HIFI_PARTY_ROLE_OFF = 0,
	HIFI_PARTY_ROLE_LISTENING = 1,
	HIFI_PARTY_ROLE_LEADER = 2,
	HIFI_PARTY_ROLE_FOLLOWER = 3,
} hifi_party_role_t;

const char *hifi_party_role_name(hifi_party_role_t r); /* "off", "listening", "leader", "follower" */

/* what an announce says, once its tag and fields have passed */
typedef struct {
	uint32_t ip, group;
	int64_t term;
	bool picked;
	int n; /* followers listed */
	uint32_t followers[HIFI_PARTY_FOLLOWERS];
	char name[HIFI_PARTY_NAME + 1];
} hifi_party_announce_t;

/* what the caller should do after a call (bits) */
#define HIFI_PARTY_DO_ANNOUNCE 1 /* send an announce now (a leader) */
#define HIFI_PARTY_DO_JOIN 2     /* send a JOIN to the leader now (a follower) */
#define HIFI_PARTY_DO_ROLE 4     /* the role, the leader or the group changed: new key, start the stream afresh */
#define HIFI_PARTY_DO_LOST 8     /* the leader went quiet: mute */
#define HIFI_PARTY_DO_LEAVE 16   /* send a JOIN with F_LEAVE to leave_ip under leave_group's key (the old leader) */
#define HIFI_PARTY_E_SOON (-1)   /* hifi_party_group_lead: a lead too soon after the last */
#define HIFI_PARTY_E_OFF (-2)    /* hifi_party_group_lead: party is off, or the heap is low */

typedef struct {
	uint32_t ip, at;
	int64_t tsf;
} hifi_party_seen_t;

typedef struct {
	uint32_t ip, at; /* at: the start of its second */
	int n;           /* hashed in that second */
} hifi_party_stranger_t;

/* a group joined with no CLOCK: its announces up to term are skipped while
 * live, until at + ms. ms stays set after it expires, so the next wait for
 * the same group and term doubles. group 0 is a free slot. */
typedef struct {
	uint32_t group;
	int64_t term;
	uint32_t at, ms;
	bool live;
} hifi_party_dead_t;

typedef struct {
	hifi_party_role_t role;
	uint32_t self_ip, mask;
	bool orphan;          /* listening after a lost leader, or after a lead stopped: never leads by itself */
	uint32_t listen_until;
	int64_t term;         /* the group's: ours when leading, the leader's when following */
	int64_t max_term;     /* the highest held or heard (kept across reboots) */
	bool picked;          /* this board's lead was asked for (kept across reboots) */
	bool dirty;           /* max_term or picked changed: the caller saves them, then clears this */
	uint32_t group;
	uint32_t last_lead;
	bool led;             /* last_lead is set */
	const char *why;      /* why the board isn't playing with a group, "" when nothing to say */
	/* listening */
	bool have_best;
	hifi_party_announce_t best;
	uint32_t best_at;
	bool heap_low;        /* the last tick's low_heap */
	hifi_party_dead_t dead[HIFI_PARTY_DEAD_SLOTS]; /* groups joined with no CLOCK */
	uint32_t other_ap_at; /* last announce whose tag checked but whose TSF was off, confirmed by an earlier one */
	bool other_ap;
	uint32_t off_ip, off_at; /* the last off-TSF announce: from, when, its TSF */
	int64_t off_tsf;
	/* leader */
	int npeers;
	uint32_t peers[HIFI_PARTY_FOLLOWERS], peer_seen[HIFI_PARTY_FOLLOWERS], peer_nack_seq[HIFI_PARTY_FOLLOWERS];
	bool peer_nacked[HIFI_PARTY_FOLLOWERS];
	hifi_party_seen_t seen[HIFI_PARTY_JOIN_SEEN];
	uint32_t last_announce;
	uint32_t announce_seq;
	/* follower */
	uint32_t leader_ip;
	bool leader_picked;
	uint32_t leave_ip, leave_group; /* with DO_LEAVE */
	uint32_t clock_seq;   /* the last CLOCK's, when clocked */
	char leader_name[HIFI_PARTY_NAME + 1];
	uint32_t joined_at, last_heard, last_join, last_announce_heard;
	bool clocked;         /* a CLOCK came from this leader and group */
	bool full;            /* the leader has its HIFI_PARTY_FOLLOWERS and we aren't one */
	/* the stranger budget (hifi_party_group_admit) */
	uint32_t budget_at;
	int budget;
	hifi_party_stranger_t strangers[HIFI_PARTY_STRANGER_SRCS];
	/* counters */
	uint32_t leads, follows, losses, refused, replays, dropped, other_aps, low_heap;
} hifi_party_group_t;

/* term and picked: what the board kept from before its last restart */
void hifi_party_group_init(hifi_party_group_t *g, uint32_t self_ip, uint32_t mask, int64_t term, bool picked);
/* the board's address changed (DHCP): a leader or follower listens again */
int hifi_party_group_set_ip(hifi_party_group_t *g, uint32_t now, uint32_t self_ip, uint32_t mask, uint32_t rnd);
/* switched on (or booted on): listens first. rnd: any random number. */
int hifi_party_group_on(hifi_party_group_t *g, uint32_t now, uint32_t rnd);
/* switched off: DO_LEAVE when the board was following */
int hifi_party_group_off(hifi_party_group_t *g);
/* Lead asked for (the page, the API, or a source starting on this board):
 * leads now with a new term and group, picked (a board already leading keeps
 * its group and only takes the new term and the pick). HIFI_PARTY_E_SOON
 * within HIFI_PARTY_LEAD_GAP_MS of the last lead, HIFI_PARTY_E_OFF when off
 * or the heap is low. The caller stops its own player when the board then
 * follows (DO_ROLE), or local_source would take the lead straight back. */
int hifi_party_group_lead(hifi_party_group_t *g, uint32_t now, uint32_t rnd);
/* every 10 to 100 ms. local_source: a source of the board's own plays (it
 * takes the lead); low_heap: internal RAM is under the floor (a leader stops,
 * nobody starts leading) */
int hifi_party_group_tick(hifi_party_group_t *g, uint32_t now, uint32_t rnd, bool local_source, bool low_heap);
/* source: hifi_sink_source()'s number for what the output plays (0 idle,
 * 1 LMS, 2 Bluetooth, 3 AirPlay, 4 Spotify, 5 test signal, 6 line in, 7 party).
 * Whether it is the board's own source, which takes the lead (tick's
 * local_source). The test signal never is. Line in is only on a board that
 * isn't following: a follower stays a follower, and its retake takes the
 * output back from line in (hifi_party_retakes), so line in never leads the
 * group from a follower. */
bool hifi_party_local_source(hifi_party_role_t role, int source);
/* whether a follower takes its output back: the output is idle (a test signal
 * ended), or line in took it */
bool hifi_party_retakes(hifi_party_role_t role, int source);
/* an announce that passed hifi_party_announce_parse */
int hifi_party_group_announce(hifi_party_group_t *g, uint32_t now, const hifi_party_announce_t *a);
/* an announce from ip whose tag passed but whose TSF (tsf_us) is off ours:
 * another AP, or a replay. It counts as another AP only when the last such
 * announce was from the same ip within HIFI_PARTY_LOSS_MS and its TSF has
 * moved on by the time between them (within HIFI_PARTY_OTHER_AP_SLACK_US):
 * one replayed announce, sent again and again, never does. */
void hifi_party_group_other_ap(hifi_party_group_t *g, uint32_t now, uint32_t ip, int64_t tsf_us);
/* A JOIN (or leave) under the group key that passed hifi_party_join_parse:
 * false when the board isn't leading, the TSF isn't past the last JOIN from
 * that IPv4 (a replay), or HIFI_PARTY_FOLLOWERS others are served. */
bool hifi_party_group_join(hifi_party_group_t *g, uint32_t now, uint32_t ip, int64_t tsf, bool leave);
/* A CLOCK under the group key from ip, with seq: the leader is still there.
 * False (and nothing changes) when ip isn't the leader or seq isn't past the
 * last CLOCK's, so a CLOCK played back can't keep a gone leader alive. */
bool hifi_party_group_heard(hifi_party_group_t *g, uint32_t now, uint32_t ip, uint32_t seq);
/* the ms left before every dead mark has expired, 0 when none is live */
uint32_t hifi_party_group_skip_ms(const hifi_party_group_t *g, uint32_t now);
/* the leader sends to this follower */
bool hifi_party_group_is_peer(const hifi_party_group_t *g, uint32_t ip);
/* a NACK under the group key from ip with seq: true when ip is a follower
 * and seq is past its last NACK's */
bool hifi_party_group_nack_ok(hifi_party_group_t *g, uint32_t ip, uint32_t seq);
/* Before the HMAC: hash a datagram of type (its byte 3, unchecked) from
 * src_ip? Members (the leader, the followers) always; an ANNOUNCE or JOIN
 * from anyone else within HIFI_PARTY_STRANGER_SRC_PER_S from that address and
 * HIFI_PARTY_STRANGERS_PER_S in all; nothing else. */
bool hifi_party_group_admit(hifi_party_group_t *g, uint32_t now, uint32_t src_ip, uint8_t type);

/* ip is another host on the subnet (self_ip, mask) */
bool hifi_party_same_subnet(uint32_t ip, uint32_t self_ip, uint32_t mask);

/* the group key for group, the announce key, and a one-byte id of the
 * secret (to compare boards' secrets without showing them) */
void hifi_party_group_key(const uint8_t secret[HIFI_PARTY_SECRET], uint32_t group, uint8_t key[HIFI_LINK_KEY]);
void hifi_party_announce_key(const uint8_t secret[HIFI_PARTY_SECRET], uint8_t key[HIFI_LINK_KEY]);
uint8_t hifi_party_secret_id(const uint8_t secret[HIFI_PARTY_SECRET]);
/* the secret from 64 hex digits (as the Hive gives it and NVS keeps it);
 * false, and secret untouched, for anything else (all zeros too) */
bool hifi_party_parse_secret(const char *s, uint8_t secret[HIFI_PARTY_SECRET]);

/* The leader's announce: the packet header (type, group, term, TSF, n,
 * flags) and its payload. */
void hifi_party_announce_build(hifi_party_group_t *g, int64_t tsf_us, const char *name, hifi_party_pkt_t *p,
			       uint8_t payload[HIFI_PARTY_ANNOUNCE_LEN]);
/* A decoded ANNOUNCE (its tag passed) from src_ip: 1 when the IPv4 in it is
 * src_ip, another host on our subnet (self_ip, mask), and its TSF is within
 * HIFI_PARTY_TSF_SKEW_US of ours; 0 when it fails those; -1 when only the
 * TSF is off (a replay, or a leader on another AP, whose TSF isn't ours). */
int hifi_party_announce_parse(const hifi_party_pkt_t *p, const uint8_t *payload, size_t plen, uint32_t src_ip, uint32_t self_ip,
			      uint32_t mask, int64_t tsf_us, hifi_party_announce_t *a);
/* A follower's JOIN to its leader, or with leave its leave (F_LEAVE, to
 * leave_ip in leave_group, or to the leader). Encode it under that group's key. */
void hifi_party_join_build(const hifi_party_group_t *g, int64_t tsf_us, bool leave, uint32_t seq, hifi_party_pkt_t *p,
			   uint8_t payload[4]);
/* A decoded JOIN from src_ip: true when the IPv4 in it is src_ip, another
 * host on our subnet, and its TSF within HIFI_PARTY_TSF_SKEW_US of ours
 * (tsf_us); fills its TSF and the leave flag. */
bool hifi_party_join_parse(const hifi_party_pkt_t *p, const uint8_t *payload, size_t plen, uint32_t src_ip, uint32_t self_ip,
			   uint32_t mask, int64_t tsf_us, int64_t *tsf, bool *leave);
