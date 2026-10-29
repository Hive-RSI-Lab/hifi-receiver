#include <stddef.h>
#include <string.h>

#include "hifi_meta.h"

const char *hifi_play_name(hifi_play_t s) {
	switch (s) {
	case HIFI_PLAY_STOP: return "stop";
	case HIFI_PLAY_PLAY: return "play";
	case HIFI_PLAY_PAUSE: return "pause";
	default: return NULL;
	}
}

void hifi_utf8_copy(char *dst, size_t cap, const char *src) {
	if (!cap) return;
	size_t n = src ? strlen(src) : 0;
	if (n >= cap) {
		n = cap - 1;
		/* back off over continuation bytes to the start of a character, then
		 * drop that character if it doesn't fit whole */
		size_t start = n;
		while (start > 0 && ((unsigned char) src[start] & 0xC0) == 0x80) start--;
		n = start;
	}
	if (n) memcpy(dst, src, n);
	dst[n] = '\0';
}

void hifi_meta_init(hifi_meta_t *m) {
	memset(m, 0, sizeof(*m));
	m->elapsed_ms = m->duration_ms = -1;
}

void hifi_meta_active(hifi_meta_t *m) {
	hifi_meta_init(m);
	m->state = HIFI_PLAY_STOP;
}

void hifi_meta_idle(hifi_meta_t *m) { hifi_meta_init(m); }

void hifi_meta_play(hifi_meta_t *m, uint32_t now_ms) {
	if (m->state != HIFI_PLAY_PLAY) {
		m->elapsed_ms = hifi_meta_elapsed_ms(m, now_ms);
		m->tick_ms = now_ms;
	}
	m->state = HIFI_PLAY_PLAY;
}

void hifi_meta_pause(hifi_meta_t *m, uint32_t now_ms) {
	m->elapsed_ms = hifi_meta_elapsed_ms(m, now_ms);
	m->tick_ms = now_ms;
	m->state = HIFI_PLAY_PAUSE;
}

void hifi_meta_track(hifi_meta_t *m, const char *artist, const char *album, const char *title) {
	if (m->state == HIFI_PLAY_NONE) m->state = HIFI_PLAY_STOP;
	hifi_utf8_copy(m->artist, sizeof(m->artist), artist);
	hifi_utf8_copy(m->album, sizeof(m->album), album);
	hifi_utf8_copy(m->title, sizeof(m->title), title);
}

void hifi_meta_timer(hifi_meta_t *m, int32_t elapsed_ms, int32_t duration_ms, uint32_t now_ms) {
	if (elapsed_ms >= 0) {
		m->elapsed_ms = elapsed_ms;
		m->tick_ms = now_ms;
	}
	if (duration_ms > 0) m->duration_ms = duration_ms;
}

int32_t hifi_meta_elapsed_ms(const hifi_meta_t *m, uint32_t now_ms) {
	if (m->elapsed_ms < 0) return -1;
	int64_t e = m->elapsed_ms;
	if (m->state == HIFI_PLAY_PLAY) e += (uint32_t) (now_ms - m->tick_ms);
	if (m->duration_ms > 0 && e > m->duration_ms) e = m->duration_ms;
	return (int32_t) e;
}
