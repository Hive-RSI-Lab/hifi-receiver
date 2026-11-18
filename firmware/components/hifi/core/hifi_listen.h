/*
 * "Cannot open the home network" (projects/hifi-lite-lab/security.md): the
 * board lists its own listening sockets (lwIP's TCP listeners and bound UDP
 * sockets) and fails the self-test and the OTA trial on anything outside the
 * allow list below. Nothing here opens a router port: the check only reads.
 *
 * Allowed, from the LAN:
 *   tcp 80    the web UI and the Spotify Connect ZeroConf endpoint
 *   tcp 5000  AirPlay (RTSP)
 *   udp 5353  mDNS
 *   udp 68    the DHCP client
 *   udp 4951  party mode, only while it is switched on (bound to the station's
 *             address; every datagram's tag is checked under the Hive key)
 *   udp 67, 53  DHCP server and captive-portal DNS, only while the setup AP is up
 *   udp 49152-65535: reply ports lwIP hands to clients (DNS, SNTP,
 *             squeezelite discovery, AirPlay's RTP while it plays)
 * and anything bound to loopback (esp_http_server's control socket,
 * squeezelite's wake sockets). TCP has no ephemeral exemption: a TCP
 * listener on any other port fails.
 *
 * lwIP's DNS client binds each query to a random port from 1024 up and drops
 * it when the answer comes, so a socket only counts when two scans a few
 * seconds apart both show it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIFI_LISTEN_MAX 48

typedef struct {
	bool tcp;       /* a TCP listener; false: a bound UDP socket */
	uint16_t port;  /* local port */
	bool loopback;  /* bound to 127.0.0.0/8 or ::1 */
} hifi_sock_t;

/* Is this one socket allowed? ap_on: the setup access point is up. */
bool hifi_listen_allowed(const hifi_sock_t *s, bool ap_on);

/* Counts the sockets in cur outside the allow list that prev (an earlier scan)
 * also has; prev NULL counts every one. why gets "N open: tcp/23 udp/1900"
 * listing the first few, or "N sockets, all allowed". ncur < 0 (the list
 * couldn't be read) counts as one stray. */
int hifi_listen_check(const hifi_sock_t *prev, int nprev, const hifi_sock_t *cur, int ncur, bool ap_on, char *why,
		      size_t nwhy);

/* L4, the running report: the verdict of every pair of scans in normal
 * running (not only in an OTA trial) goes through this. events counts each
 * time strays appear or the stray list changes; open holds the newest list
 * ("" while all clear). Returns true when the caller should log: a new or
 * changed set of strays. */
typedef struct {
	uint32_t events; /* stray sets seen since boot */
	int strays;      /* in the newest verdict; -1: no verdict yet */
	char open[64];
} hifi_listen_report_t;

void hifi_listen_report_init(hifi_listen_report_t *r);
bool hifi_listen_report(hifi_listen_report_t *r, int strays, const char *why);
