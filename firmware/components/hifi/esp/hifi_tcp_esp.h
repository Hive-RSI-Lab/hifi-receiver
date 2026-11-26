/* The printer relay on the board (esp/hifi_tcp.c; docs/remote.md, "The TCP relay"). */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Reads the enabled printer from NVS and starts the relay's task. */
void hifi_tcp_start(void);
/* A signed /relay/ command (hifi_remote_t.relay), on the command worker. */
int hifi_tcp_relay(const char *method, const char *path, const char *body, char *out, size_t cap);
/* A binary frame from the tower, [sid][data] (hifi_ws_bin_cb_t). */
void hifi_tcp_on_bin(const uint8_t *data, size_t len);
