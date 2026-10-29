#include "hifi_ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the names the app and the API use (docs/app.md) */
static const char *const NAMES[HIFI_IR_N] = {"volup", "voldown", "mute", "toggle", "next", "prev", "input", "power"};

const char *hifi_ir_action_name(int a) { return a >= 0 && a < HIFI_IR_N ? NAMES[a] : NULL; }

int hifi_ir_action_from(const char *name) {
	for (int a = 0; name && a < HIFI_IR_N; a++)
		if (!strcmp(name, NAMES[a])) return a;
	return -1;
}

void hifi_ir_init(hifi_ir_t *ir) {
	memset(ir, 0, sizeof(*ir));
	ir->learning = -1;
}

void hifi_ir_learn(hifi_ir_t *ir, int action, uint32_t now_ms) {
	if (action < 0 || action >= HIFI_IR_N) return;
	ir->learning = action;
	ir->learn_since_ms = now_ms;
}

void hifi_ir_forget(hifi_ir_t *ir, int action) {
	if (action < 0) memset(ir->have, 0, sizeof(ir->have));
	else if (action < HIFI_IR_N) ir->have[action] = false;
	ir->learning = -1;
}

int hifi_ir_learning(hifi_ir_t *ir, uint32_t now_ms) {
	if (ir->learning >= 0 && now_ms - ir->learn_since_ms > HIFI_IR_LEARN_MS) ir->learning = -1;
	return ir->learning;
}

static bool is_volume(int a) { return a == HIFI_IR_VOLUP || a == HIFI_IR_VOLDOWN; }

int hifi_ir_code(hifi_ir_t *ir, uint32_t code, uint32_t now_ms, bool *changed) {
	*changed = false;
	bool repeat = ir->have_last && code == ir->last_code && now_ms - ir->last_ms < HIFI_IR_REPEAT_MS;
	ir->have_last = true;
	ir->last_code = code;
	ir->last_ms = now_ms;

	int learning = hifi_ir_learning(ir, now_ms);
	if (learning >= 0) {
		if (repeat) return HIFI_IR_EATEN; /* the tail of a press before Teach */
		for (int a = 0; a < HIFI_IR_N; a++) /* one button, one action */
			if (ir->have[a] && ir->code[a] == code) ir->have[a] = false;
		ir->code[learning] = code;
		ir->have[learning] = true;
		ir->learning = -1;
		ir->fired_ms = now_ms; /* so the held button doesn't fire it at once */
		*changed = true;
		return HIFI_IR_EATEN;
	}

	int action = HIFI_IR_PASS;
	for (int a = 0; a < HIFI_IR_N; a++)
		if (ir->have[a] && ir->code[a] == code) action = a;
	if (action == HIFI_IR_PASS) return HIFI_IR_PASS;
	if (repeat && (!is_volume(action) || now_ms - ir->fired_ms < HIFI_IR_HOLD_STEP_MS)) return HIFI_IR_EATEN;
	ir->fired_ms = now_ms;
	return action;
}

void hifi_ir_save(const hifi_ir_t *ir, char *out, size_t cap) {
	size_t n = 0;
	if (cap) out[0] = '\0';
	for (int a = 0; a < HIFI_IR_N; a++) {
		if (!ir->have[a]) continue;
		int w = snprintf(out + n, cap > n ? cap - n : 0, "%s%s=%08lx", n ? "," : "", NAMES[a],
				 (unsigned long) ir->code[a]);
		if (w < 0 || n + (size_t) w >= cap) { /* cut short: keep what fit whole */
			if (cap) out[n] = '\0';
			return;
		}
		n += w;
	}
}

void hifi_ir_load(hifi_ir_t *ir, const char *s) {
	memset(ir->have, 0, sizeof(ir->have));
	while (s && *s) {
		const char *eq = strchr(s, '='), *end = strchr(s, ',');
		if (!end) end = s + strlen(s);
		if (eq && eq < end) {
			char name[16];
			size_t len = (size_t) (eq - s);
			if (len < sizeof(name)) {
				memcpy(name, s, len);
				name[len] = '\0';
				char *stop;
				unsigned long code = strtoul(eq + 1, &stop, 16);
				int a = hifi_ir_action_from(name);
				if (a >= 0 && stop == end && stop > eq + 1) {
					ir->code[a] = (uint32_t) code;
					ir->have[a] = true;
				}
			}
		}
		s = *end ? end + 1 : end;
	}
}
