/* Regression: the source handover's sink side (patch 0034), the Opus review
 * of #249. A sink that lost the output while it waited for room in the
 * buffer kept writing into the new owner's audio; a Spotify volume change
 * refused during a handover was lost; and each new Spotify track let an idle
 * Bluetooth phone go. Builds upstream's own decode_external.c functions, as
 * patched, with our real hifi_source_decide, against stubs of squeezelite's
 * output, buffer and locks. */
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../check.h"
#include "../../components/hifi/core/hifi_source.c"

/* --- the squeezelite bits these functions touch --- */
#define CONFIG_BT_SINK 1
#define BYTES_PER_FRAME 4
#define LOG_SDEBUG(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
static int n_warn;
#define LOG_WARN(...) (n_warn++)
#define min(a, b) ((a) < (b) ? (a) : (b))
typedef uint32_t u32_t;
typedef int16_t s16_t;
typedef int32_t ISAMPLE_T;

enum { DECODE_BT = 1, DECODE_RAOP, DECODE_CSPOT, DECODE_HIFI_TEST, DECODE_HIFI_LINE, DECODE_HIFI_PARTY };
enum { SRC_ALLOW, SRC_TAKEOVER, SRC_REFUSE, SRC_IGNORE };
enum { SRC_EV_START, SRC_EV_OWN, SRC_EV_SESSION, SRC_EV_PLAY };
enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_BUFFER, OUTPUT_RUNNING };
enum { DECODE_STOPPED, DECODE_READY, DECODE_RUNNING, DECODE_COMPLETE, DECODE_ERROR };
typedef enum { CSPOT_START, CSPOT_DISC, CSPOT_FLUSH, CSPOT_STOP, CSPOT_PLAY, CSPOT_PAUSE, CSPOT_SEEK, CSPOT_NEXT,
	       CSPOT_PREV, CSPOT_TOGGLE, CSPOT_TRACK_INFO, CSPOT_TRACK_MARK, CSPOT_VOLUME, CSPOT_VOLUME_UP,
	       CSPOT_VOLUME_DOWN, CSPOT_BUSY, CSPOT_QUERY_STARTED, CSPOT_QUERY_REMAINING } cspot_event_t;

static struct {
	int external, state, threshold;
	uint32_t current_sample_rate, next_sample_rate, frames_played, stop_time;
	bool track_started;
	uint8_t *track_start;
	char device[16];
} output;
static struct { int state; } decode;
static enum { SINK_RUNNING, SINK_ABORT, SINK_DISCARD } sink_state;
static bool hifi_output_flushed;
static bool enable_bt_sink = true, bt_sink_off;

/* the output buffer: room for 4096 bytes, written from the start */
static uint8_t mem[4096];
static struct buffer { uint8_t *writep; size_t used, size; } outbuf = {mem, 0, sizeof(mem)}, *outputbuf = &outbuf;
static size_t _buf_space(struct buffer *b) { return b->size - b->used; }
static size_t _buf_cont_write(struct buffer *b) { return b->size - b->used; }
static size_t _buf_used(struct buffer *b) { return b->used; }
static void _buf_inc_writep(struct buffer *b, size_t n) { b->used += n, b->writep += n; }
static void _buf_flush(struct buffer *b) { b->used = 0, b->writep = mem; }
static void _buf_limit(struct buffer *b, size_t n) { (void) b, (void) n; }

/* the locks, with a hook for what another task does while one is taken */
static int lock_o, lock_d;
static void (*on_lock)(void);
static void take_o(void) {
	if (on_lock) { void (*f)(void) = on_lock; on_lock = NULL; f(); }
	lock_o++;
}
#define LOCK_O take_o()
#define UNLOCK_O (lock_o--)
#define LOCK_D (lock_d++)
#define UNLOCK_D (lock_d--)
/* the wait for room: another task may run meanwhile */
static void (*on_wait)(void);
static int n_waits;
static int usleep(unsigned us) {
	(void) us;
	n_waits++;
	if (on_wait) on_wait();
	return 0;
}
static uint32_t gettime_ms(void) { return 1000; }

/* what the handover asked of the rest */
static int n_quiet, n_restore, restored, n_let_go, n_volume, volume_locked;
static u32_t volume_set;
static void quiet(bool down) { (void) down; n_quiet++; }
static void (*hifi_handover_quiet)(bool) = quiet;
static int (*decide_fn)(int, bool, int, int) = hifi_source_decide;
#define hifi_source_decide decide_fn
static void decode_restore(int external) { n_restore++, restored = external; }
static void hifi_sink_stop(void) {}
static void bt_sink_let_go(void) { n_let_go++; }
static void set_volume(u32_t l, u32_t r) {
	(void) r;
	n_volume++, volume_set = l;
	volume_locked = lock_o; /* set_volume takes the output lock itself */
}

/* a static variable of decode_external.c's, which extract doesn't pull */
static int cspot_volume_held = -1;
void decode_phones_yield(int keep); /* squeezelite.h */
#include "ext_fns.c"

static bool cspot(cspot_event_t cmd, ...) {
	va_list args;
	va_start(args, cmd);
	bool ok = cspot_cmd_handler(cmd, args);
	va_end(args);
	return ok;
}
static void reset(void) { n_warn = n_waits = n_quiet = n_restore = n_let_go = n_volume = 0; }
static void bt_plays(void) {
	output.external = DECODE_BT, output.state = OUTPUT_RUNNING;
	sink_state = SINK_RUNNING;
	_buf_flush(outputbuf);
}
static void spotify_takes(void) {
	output.external = DECODE_CSPOT;
	sink_state = SINK_ABORT;
	_buf_flush(outputbuf);
}
/* ... and its audio starts: its first write sets the sink running again */
static void spotify_takes_and_plays(void) {
	spotify_takes();
	sink_state = SINK_RUNNING;
	_buf_inc_writep(outputbuf, 100);
}

int main(void) {
	static uint8_t pcm[1024];
	strcpy(output.device, "I2S");

	/* the owner writes */
	bt_plays();
	CHECK(sink_data_handler(pcm, 512, 10, DECODE_BT) == 512 && outbuf.used == 512);
	/* a source that doesn't own the output writes nothing */
	reset();
	CHECK(sink_data_handler(pcm, 512, 10, DECODE_CSPOT) == 0 && outbuf.used == 512);

	/* the output is taken between the first check and the lock: nothing written */
	bt_plays();
	on_lock = spotify_takes;
	CHECK(sink_data_handler(pcm, 512, 10, DECODE_BT) == 0 && outbuf.used == 0);
	CHECK(lock_o == 0);

	/* the buffer is full and the output is taken while Bluetooth waits for
	 * room: it stops at once, writes none of its rest into Spotify's audio,
	 * and doesn't warn about frames it no longer owns */
	bt_plays();
	outbuf.used = outbuf.size - 256, outbuf.writep = mem + outbuf.used;
	reset();
	on_wait = spotify_takes;
	CHECK(sink_data_handler(pcm, 1024, 10, DECODE_BT) == 256);
	CHECK(n_waits == 1 && outbuf.used == 0 && n_warn == 0 && lock_o == 0);
	/* the same with Spotify's audio already flowing when Bluetooth wakes */
	bt_plays();
	outbuf.used = outbuf.size - 256, outbuf.writep = mem + outbuf.used;
	reset();
	on_wait = spotify_takes_and_plays;
	CHECK(sink_data_handler(pcm, 1024, 10, DECODE_BT) == 256);
	CHECK(n_waits == 1 && outbuf.used == 100 && n_warn == 0 && lock_o == 0);
	on_wait = NULL;

	/* the owner that keeps waiting still warns and drops, as before */
	bt_plays();
	outbuf.used = outbuf.size, outbuf.writep = mem + outbuf.size;
	reset();
	CHECK(sink_data_handler(pcm, 512, 3, DECODE_BT) == 0);
	CHECK(n_waits == 4 && n_warn == 1 && lock_o == 0);

	/* a Spotify volume change while Bluetooth plays is kept, not lost: the
	 * track's start takes over and applies it, outside the output lock */
	bt_plays();
	reset();
	CHECK(cspot(CSPOT_VOLUME, (u32_t) 32768));
	CHECK(n_volume == 0);
	CHECK(cspot(CSPOT_START, (u32_t) 44100));
	CHECK(output.external == DECODE_CSPOT && n_restore == 1 && restored == DECODE_BT && n_quiet == 2);
	CHECK(n_volume == 1 && volume_set == (u32_t) (65536 * powf(0.5f, 4)) && volume_locked == 0);
	CHECK(lock_o == 0 && lock_d == 0);
	/* and once only */
	reset();
	CHECK(cspot(CSPOT_START, (u32_t) 44100));
	CHECK(n_volume == 0);

	/* a volume change of its own replaces the held one */
	bt_plays();
	cspot(CSPOT_VOLUME, (u32_t) 32768);
	output.external = DECODE_CSPOT;
	reset();
	cspot(CSPOT_VOLUME, (u32_t) 65535);
	CHECK(n_volume == 1);
	bt_plays();
	reset();
	cspot(CSPOT_START, (u32_t) 44100);
	CHECK(n_volume == 0);

	/* the Spotify session ends while Bluetooth plays: its held volume goes
	 * with it, so the next session doesn't start at the old one's volume */
	bt_plays();
	cspot(CSPOT_VOLUME, (u32_t) 32768);
	cspot(CSPOT_DISC);
	CHECK(output.external == DECODE_BT); /* the session end left Bluetooth alone */
	reset();
	cspot(CSPOT_START, (u32_t) 44100);
	CHECK(n_volume == 0);

	/* Spotify starting afresh lets an idle Bluetooth phone go; its next
	 * track doesn't, so a phone that connected since is kept */
	output.external = 0, output.state = OUTPUT_STOPPED;
	reset();
	cspot(CSPOT_START, (u32_t) 44100);
	CHECK(n_let_go == 1);
	output.state = OUTPUT_RUNNING;
	reset();
	cspot(CSPOT_START, (u32_t) 44100);
	CHECK(n_let_go == 0 && n_restore == 0);

	/* a Bluetooth output (the speaker plays to a BT device) has no phone to let go */
	output.external = 0, output.state = OUTPUT_STOPPED;
	strcpy(output.device, "BT");
	reset();
	cspot(CSPOT_START, (u32_t) 44100);
	CHECK(n_let_go == 0);
	strcpy(output.device, "I2S");

	/* would a phone's audio cut in? Only over another phone source that has the output */
	output.external = DECODE_CSPOT, output.state = OUTPUT_RUNNING;
	CHECK(source_decide(DECODE_BT, SRC_EV_START) == SRC_TAKEOVER);
	output.external = 0, output.state = OUTPUT_STOPPED;
	CHECK(source_decide(DECODE_BT, SRC_EV_START) == SRC_ALLOW);
	output.external = DECODE_BT;
	CHECK(source_decide(DECODE_BT, SRC_EV_START) == SRC_ALLOW);

	/* a source that lost the output asks how much is queued: nothing of its own */
	bt_plays();
	outbuf.used = 1000;
	uint32_t ms = 99;
	CHECK(cspot(CSPOT_QUERY_REMAINING, &ms) && ms == 0);

	return check_report("up_ext");
}
