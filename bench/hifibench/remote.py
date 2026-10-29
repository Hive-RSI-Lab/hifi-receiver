"""The unattended bring-up of a board left at home (docs/remote.md, "The Hive's
side"): no dummy loads, no interface, no one at the bench. Everything it
measures comes from the board itself: its self-test and state, and its
loopback taps (one leg of each speaker output into the codec's ADC).

Quiet by default: the box may have its speakers on, so tones stay at or below
-30 dBFS unless the run is `loud`.
"""
from __future__ import annotations

import base64
import json
import math
import struct
import time
from pathlib import Path

from . import dsp, predict
from .api import ApiError, Device

QUIET_MAX_DBFS = -30.0
FR_HZ = [50, 100, 1000, 5000, 10000, 18000]
GAIN_TOL_DB = 3.0      # the expected gain is a datasheet estimate until bring-up calibrates it
THDN_LIMIT = 0.01      # 1% at the quiet levels: far below where a working amp clips
FR_TOL_DB = 3.0


def decode(cap: dict) -> list[list[float]]:
    """A capture reply's samples, per channel, in ADC full scale."""
    raw = base64.b64decode(cap["pcm"])
    n = len(raw) // 2
    pcm = struct.unpack(f"<{n}h", raw[:2 * n])
    ch = len(cap.get("channels") or [0, 0])
    return [[v / 32768 for v in pcm[c::ch]] for c in range(ch)]


def tone(dev: Device, freq: float, level: float, kind: str = "sine", seconds: float = 4.0) -> dict:
    body = {"type": kind, "freq_hz": freq, "level_dbfs": level, "channel": "both", "duration_s": seconds}
    if level > -6:
        body["force"] = True
    return dev.t.call("tone", body, post=True)


def stop(dev: Device):
    try:
        dev.t.call("tone", {"type": "stop"}, post=True)
    except ApiError:
        pass


def capture(dev: Device, frames: int = 4096, source: str = "loopback") -> dict:
    return dev.t.call("capture", {"source": source, "frames": frames}, post=True)


def measure(dev: Device, freq: float, level: float, frames: int = 4096, settle_s: float = 0.3) -> dict:
    """Play a sine, record the taps, and analyse each channel."""
    tone(dev, freq, level)
    time.sleep(settle_s)
    cap = capture(dev, frames)
    chans = decode(cap)
    vpf = cap.get("v_per_fs") or [None] * len(chans)
    out = {"freq_hz": freq, "level_dbfs": level, "channels": []}
    for name, x, v, st in zip(cap.get("channels", []), chans, vpf, cap.get("stats", [{}] * len(chans))):
        row = {"name": name, "clipped": st.get("clipped", 0)}
        try:
            t = dsp.analyse(x, cap["rate"], freq)
            row.update(amp_fs=t.amp, thdn=t.thdn, thd=t.thd, freq_hz=t.freq_hz,
                       vpk=t.amp * v if v else None)
        except ValueError as e:
            row.update(error=str(e))
        out["channels"].append(row)
    return out


def expected_leg_vpk(board: str, level: float) -> float | None:
    """A leg carries half the bridge's swing: half the 0 dBFS peak, scaled."""
    fs = predict.TIERS.get(board, {}).get("fs_vpk")
    return fs / 2 * 10 ** (level / 20) if fs else None


def _db(a, b):
    return 20 * math.log10(a / b) if a and b else None


def selftest(dev: Device, state: dict) -> dict:
    """The board's self-test. On a 5 V source the charger-dependent items are
    expected to fail and are marked so (the box may be on a laptop port)."""
    r = dev.selftest()
    items = r.get("items") or r.get("checks") or []
    v = _contract_v(state)
    if v is not None and v < 9:
        for i in items:
            if not i["pass"] and i["name"] in ("pd_contract", "pvdd_on", "amp_no_fault", "amp_faultz_high"):
                i["expected"] = True
    return {"pass": all(i["pass"] or i.get("expected") for i in items), "items": items, "contract_v": v}


def _contract_v(state: dict) -> float | None:
    if "power" in state:   # the firmware's shape
        return (state["power"].get("contract") or {}).get("v")
    return (state.get("pd") or {}).get("contract_v")


def bringup(dev: Device, board: str | None = None, loud: bool = False, max_dbfs: float | None = None,
            say=print) -> dict:
    info = dev.info()
    board = board or info.get("board", "lite-main")
    top = (0.0 if loud else QUIET_MAX_DBFS) if max_dbfs is None else max_dbfs
    if not loud:
        top = min(top, QUIET_MAX_DBFS)
    s = {"kind": "remote", "board": board, "info": info, "via": str(dev), "started": time.strftime("%Y-%m-%d %H:%M:%S"),
         "loud": loud, "max_dbfs": top, "levels": [], "noise": None, "response": [], "verdicts": {}, "events": [],
         "not_measured": ["power into a load and the 1% / 10% power points (need dummy loads; docs/bench.md)",
                          "the amplifier chip's temperature (Lite has no sensor; needs a probe)",
                          "streaming services, buttons and the remote (need a person or a phone)"]}
    say(f"remote bring-up of {info.get('board')} {info.get('mac')} fw {info.get('fw', info.get('fw_version'))} via {dev}")
    s["state"] = st = dev.state()
    try:
        s["ota"] = dev.t.call("ota")
    except ApiError as e:
        s["ota"] = {"error": str(e)}
    s["selftest"] = sel = selftest(dev, st)
    s["verdicts"]["self-test"] = "pass" if sel["pass"] else "fail: " + ", ".join(
        i["name"] for i in sel["items"] if not (i["pass"] or i.get("expected")))
    v = sel["contract_v"]
    if v is not None and v < 9:
        s["verdicts"]["loopback"] = f"not measured: on a {v:g} V source the amp stays off; move it to its charger"
        return s
    try:
        _loopback(dev, s, board, top, say)
    except ApiError as e:
        if e.op == "capture" and e.status in (404, 409, 501):
            s["verdicts"]["loopback"] = f"not measured: the board can't capture ({e.status} {e.msg})"
        else:
            s["events"].append(f"stopped on an API error: {e}")
            s["verdicts"]["loopback"] = f"fail: {e}"
    finally:
        stop(dev)
    return s


def _loopback(dev: Device, s: dict, board: str, top: float, say):
    levels = [-40.0, -30.0]
    if top > -30:
        levels += [x for x in (-20.0, -12.0, -6.0, -3.0, 0.0) if x <= top]
    levels = [x for x in levels if x <= top] or [top]
    clip_at = None
    for lv in levels:
        m = measure(dev, 1000, lv)
        exp = expected_leg_vpk(board, lv)
        for c in m["channels"]:
            c["expected_vpk"] = exp
            c["gain_err_db"] = _db(c.get("vpk"), exp)
        s["levels"].append(m)
        say("  1 kHz at {:g} dBFS: ".format(lv) + "; ".join(
            f"{c['name']} {c.get('vpk') or 0:.3f} V pk, THD+N {100 * c.get('thdn', 1):.3f}%" for c in m["channels"]))
        if any(c.get("clipped") or c.get("thdn", 1) > 0.1 for c in m["channels"]):
            clip_at = lv
            break
    ref = next(m for m in s["levels"] if m["level_dbfs"] == min(-30.0, top)) if any(
        m["level_dbfs"] == min(-30.0, top) for m in s["levels"]) else s["levels"][-1]
    # Both channels play, at about the gain the design expects.
    bad = [c for c in ref["channels"] if "error" in c or not c.get("vpk")]
    if bad:
        s["verdicts"]["both channels play"] = "fail: no tone on " + ", ".join(c["name"] for c in bad)
    else:
        s["verdicts"]["both channels play"] = "pass"
        errs = [c["gain_err_db"] for c in ref["channels"]]
        if None in errs:
            s["verdicts"]["gain"] = "measured, no expected figure for " + board + ": " + ", ".join(
                f"{c['name']} {c['vpk']:.3f} V pk" for c in ref["channels"])
        else:
            worst = max(errs, key=abs)
            s["verdicts"][f"gain within ±{GAIN_TOL_DB:g} dB of the design"] = (
                ("pass" if abs(worst) <= GAIN_TOL_DB else "fail") + f": worst {worst:+.2f} dB")
        thdn = max(c["thdn"] for c in ref["channels"])
        s["verdicts"][f"THD+N at {ref['level_dbfs']:g} dBFS under {100 * THDN_LIMIT:g}%"] = (
            ("pass" if thdn < THDN_LIMIT else "fail") + f": {100 * thdn:.3f}% (one leg; ADC-limited at low levels)")
    if clip_at is not None:
        s["clip_dbfs"] = clip_at
        s["verdicts"]["clipping point"] = f"measured: clips at {clip_at:g} dBFS"
    elif top > -30:
        s["verdicts"]["clipping point"] = f"measured: clean up to {s['levels'][-1]['level_dbfs']:g} dBFS"
    # The noise floor: digital silence with the amp on.
    tone(dev, 1000, -90, kind="silence")
    time.sleep(0.3)
    cap = capture(dev, 4096)
    chans = decode(cap)
    vpf = cap.get("v_per_fs") or [None] * len(chans)
    s["noise"] = [{"name": n, "uv_rms": dsp.rms(x) * v * 1e6 if v else None, "rms_fs": dsp.rms(x)}
                  for n, x, v in zip(cap["channels"], chans, vpf)]
    s["verdicts"]["noise floor (silence, amp on)"] = "measured: " + ", ".join(
        f"{n['name']} {n['uv_rms']:.0f} µV rms" if n["uv_rms"] is not None else f"{n['name']} {n['rms_fs']:.2e} FS"
        for n in s["noise"])
    # A coarse frequency response, from stepped sines at the quiet level.
    lv = min(-30.0, top)
    for f in FR_HZ:
        m = measure(dev, f, lv, frames=8192 if f < 200 else 4096)
        s["response"].append(m)
    one_k = {c["name"]: c.get("amp_fs") for c in next(m for m in s["response"] if m["freq_hz"] == 1000)["channels"]}
    worst = 0.0
    for m in s["response"]:
        for c in m["channels"]:
            c["rel_db"] = _db(c.get("amp_fs"), one_k.get(c["name"]))
            if c["rel_db"] is None:
                worst = float("inf")
            else:
                worst = max(worst, abs(c["rel_db"]))
    s["verdicts"][f"response {FR_HZ[0]} Hz to {FR_HZ[-1] // 1000} kHz within ±{FR_TOL_DB:g} dB"] = (
        ("pass" if worst <= FR_TOL_DB else "fail") + (f": worst {worst:.2f} dB" if worst != float("inf") else ": a tone missing"))


def markdown(s: dict) -> str:
    i = s.get("info", {})
    L = [f"# Remote bring-up: {i.get('board', s['board'])} {i.get('mac', '')}", "",
         f"{s['started']} · firmware {i.get('fw', i.get('fw_version', '?'))} · via {s['via']}"
         + (" · **MOCK board**" if s.get("mock") else ""),
         "", "No one at the bench: everything here is what the board measured on itself. "
         + ("Loud run (dummy loads or no speakers)." if s["loud"] else f"Quiet run: tones at or below {s['max_dbfs']:g} dBFS."),
         "", "## Verdicts", ""]
    L += [f"- **{k}:** {v}" for k, v in s["verdicts"].items()]
    L += ["", "**Not measured here:**", ""] + [f"- {x}" for x in s["not_measured"]]
    if s.get("selftest"):
        L += ["", "## Self-test", "", "| check | result | value |", "|---|---|---|"]
        for c in s["selftest"]["items"]:
            L.append(f"| {c['name']} | {'pass' if c['pass'] else ('expected' if c.get('expected') else 'FAIL')} | {c.get('value', '')} |")
    if s["levels"]:
        L += ["", "## 1 kHz, by level (loopback, one leg of each output)", "",
              "| dBFS | channel | V peak | expected | error dB | THD+N % | clipped |", "|---|---|---|---|---|---|---|"]
        for m in s["levels"]:
            for c in m["channels"]:
                L.append(f"| {m['level_dbfs']:g} | {c['name']} | {_n(c.get('vpk'), '{:.3f}')} | {_n(c.get('expected_vpk'), '{:.3f}')} "
                         f"| {_n(c.get('gain_err_db'), '{:+.2f}')} | {_n(100 * c['thdn'] if 'thdn' in c else None, '{:.3f}')} | {c.get('clipped', 0)} |")
    if s["response"]:
        L += ["", "## Frequency response (re 1 kHz)", "", "| Hz | " + " | ".join(c["name"] for c in s["response"][0]["channels"]) + " |",
              "|---|" + "---|" * len(s["response"][0]["channels"])]
        for m in s["response"]:
            L.append(f"| {m['freq_hz']:g} | " + " | ".join(_n(c.get("rel_db"), "{:+.2f}") for c in m["channels"]) + " |")
    if s.get("events"):
        L += ["", "## Events", ""] + [f"- {e}" for e in s["events"]]
    return "\n".join(L) + "\n"


def _n(v, fmt):
    return "–" if v is None else fmt.format(v)


def write(out: Path, s: dict) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    (out / "session.json").write_text(json.dumps(s, indent=1, default=str) + "\n")
    (out / "report.md").write_text(markdown(s))
    return out / "report.md"
