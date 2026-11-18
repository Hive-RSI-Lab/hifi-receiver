#!/usr/bin/env python3
"""Every planned restart and every update goes quiet first (core/hifi_quiet.h),
and the amp stays off at boot until the codec is steady (run by make -C
firmware/test). A source check: the link wraps esp_restart, the wrap runs the
quiet stop before the real restart, nothing calls round the wrap, the OTA
writer goes quiet before it opens the slot, and the power task gates the amp."""
import os
import re
import sys

FW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
HIFI = os.path.join(FW, "components", "hifi")
fails = []


def read(*p):
    with open(os.path.join(*p), encoding="utf-8") as f:
        return f.read()


def body(src, head):
    """the braces' contents after the first line that starts with head"""
    i = src.index(head)
    i = src.index("{", i)
    depth = 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i + 1 : j]
    raise ValueError(head)


def check(cond, what):
    if not cond:
        fails.append(what)


def before(text, a, b, what):
    ia, ib = text.find(a), text.find(b)
    check(ia >= 0 and ib >= 0 and ia < ib, what)


def code(src):
    """src without comments, string or char literals (one pass, so a // in a
    string or a quote in a comment can't throw it)"""
    tok = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
    return tok.sub(lambda m: '""' if m.group(0)[0] in "\"'" else " ", src)


# 1. the link wraps esp_restart for every object, IDF's rollback included
check("-Wl,--wrap=esp_restart" in read(HIFI, "CMakeLists.txt"), "CMakeLists.txt: no --wrap=esp_restart")
check("core/hifi_quiet.c" in read(HIFI, "CMakeLists.txt"), "CMakeLists.txt: hifi_quiet.c isn't built")

# 2. the wrap goes quiet before the real restart
w = code(body(read(HIFI, "esp", "hifi_crash.c"), "void __wrap_esp_restart(void)"))
before(w, "hifi_quiet_stop(", "__real_esp_restart(", "__wrap_esp_restart: hifi_quiet_stop must come before __real_esp_restart")

# 3. nothing restarts round the wrap: no __real_esp_restart, esp_restart_noos
# or a raw CPU/system reset anywhere but the wrap, in our code or our patches
ROUND = re.compile(r"\b(__real_esp_restart|esp_restart_noos\w*|software_reset\w*|esp_cpu_reset|esp_rom_software_reset_\w+|rtc_\w*sw_\w*reset)\s*\(")
for root, _, files in os.walk(FW):
    if "/third_party" in root or "/test/build" in root or "/.git" in root or "/.fw-src" in root:
        continue
    for f in files:
        if not f.endswith((".c", ".h", ".cpp", ".patch")):
            continue
        p = os.path.join(root, f)
        src = read(p)
        if f.endswith(".patch"):  # only the lines a patch adds
            src = "\n".join(l[1:] for l in src.splitlines() if l.startswith("+") and not l.startswith("+++"))
        src = code(src)
        if p.endswith(os.path.join("esp", "hifi_crash.c")):
            src = src.replace(w, "")
            src = re.sub(r"void __real_esp_restart\(void\);", "", src)
        for m in ROUND.finditer(src):
            fails.append("%s: %s() restarts round the quiet stop" % (os.path.relpath(p, FW), m.group(1)))

# 4. updates: the OTA writer goes quiet before it touches the slot, and an
# update that doesn't go ahead gives the sound back; it is the only writer
relay = read(HIFI, "esp", "hifi_relay.c")
fo = code(body(relay, "static int f_open("))
before(fo, "hifi_quiet_stop(", "esp_ota_begin(", "f_open: hifi_quiet_stop must come before esp_ota_begin")
check("hifi_quiet_release(" in code(body(relay, "static void f_abort(")), "f_abort: no hifi_quiet_release")
for root, _, files in os.walk(HIFI):
    for f in files:
        if f.endswith(".c"):
            n = len(re.findall(r"\besp_ota_begin\s*\(", code(read(root, f))))
            if f == "hifi_relay.c":
                check(n == 1, "hifi_relay.c: %d esp_ota_begin calls, want 1 (in f_open)" % n)
            elif n:
                fails.append("%s: esp_ota_begin outside hifi_relay.c's f_open skips the quiet stop" % f)
ota = code(read(HIFI, "core", "hifi_ota.c"))
check("io->abort" in body(ota, "void hifi_ota_abort("), "hifi_ota_abort: doesn't call io->abort")
fin = body(ota, "int hifi_ota_finish(")
check(fin.count("hifi_ota_abort(") >= 3, "hifi_ota_finish: a failed update must abort (and so release)")

# 5. the power task: no routing once quiet, the amp gated by
# hifi_quiet_amp_allowed (codec up, its hold over, not quiet) before it is driven
pt = code(body(read(HIFI, "esp", "hifi_main.c"), "static void power_task("))
check("!quiet_req" in pt, "power_task: the routing must stop once quiet_req is set")
before(pt, "hifi_quiet_amp_allowed(", "hifi_bsp_power_out(", "power_task: hifi_quiet_amp_allowed must gate the amp before hifi_bsp_power_out")
check(re.search(r"hifi_quiet_amp_allowed\(codec_up, amp_hold_until, amp_off \|\| in_recovery,", pt) is not None,
      "power_task: the amp gate must use codec_up, amp_hold_until and amp_off (not quiet_req: "
      "the amp would go off before the ramp and the mute)")
check(re.search(r"\bamp_off\s*=\s*amp_off_req\b", pt) is not None, "power_task: amp_off must be read from amp_off_req")
main = code(read(HIFI, "esp", "hifi_main.c"))
sets = [m.start() for m in re.finditer(r"\bamp_off_req\s*=\s*true", main)]
qa = body(main, "static void q_amp_off(")
check(len(sets) == 1 and "amp_off_req = true" in code(qa),
      "hifi_main.c: amp_off_req must be set only in q_amp_off (after the ramp and the mute)")
# a restart's stop is final: no release can undo it
check(re.search(r"hifi_quiet_stop\([^;]*,\s*true\)", code(read(HIFI, "esp", "hifi_crash.c"))) is not None,
      "hifi_crash.c: the esp_restart wrap must call hifi_quiet_stop(..., true)")
check("quiet_final" in code(body(main, "void hifi_quiet_release(")), "hifi_quiet_release: must not undo a restart's stop")
rel = code(body(main, "void hifi_quiet_release("))
check("quiet_mux" in rel and "quiet_mux" in code(body(main, "void hifi_quiet_stop(")),
      "hifi_quiet_release/stop: quiet_final and quiet_done must be checked under quiet_mux")
# nothing raises squeezelite's gain while a stop holds it at 0
check("!quiet_req" in code(body(main, "static void apply_volume(void) {")), "apply_volume: must not raise the gain during a stop")
check("quiet_req" in code(body(main, "bool hifi_volume_hook(")), "hifi_volume_hook: must hold the stream at 0 during a stop")
# squeezelite's unity save/restore writes its gain directly: no tone or line copy starts or stops during a stop
check("quiet_req" in code(body(main, "static int op_tone_start(")), "op_tone_start: must refuse during a stop")
check("quiet_req" in code(body(main, "static void tone_main(")), "tone_main: must not stop the sink during a stop")
check(code(body(main, "static void line_main(")).count("quiet_req") >= 2, "line_main: no line start or stop during a stop")

# 6. Lite's boot: SDZ is driven low before the pin becomes an output
init = code(body(read(HIFI, "esp", "board_lite.c"), "void hifi_bsp_init("))
before(init, "gpio_set_level(PIN_AMP_SDZ, 0)", "gpio_config(&out)", "board_lite.c: SDZ must be low before it is an output")
amp_off = code(body(read(HIFI, "esp", "board_lite.c"), "void hifi_bsp_amp_off_now("))
check("gpio_set_level(PIN_AMP_SDZ, 0)" in amp_off, "board_lite.c: hifi_bsp_amp_off_now must drop SDZ")

for f in fails:
    print("test_quiet_paths: " + f, file=sys.stderr)
print("test_quiet_paths: %s" % ("ok" if not fails else "%d failed" % len(fails)))
sys.exit(1 if fails else 0)
