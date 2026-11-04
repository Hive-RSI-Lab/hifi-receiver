"""The measurements: self-test, power sweep, thermal soak, and the staged
bring-up that strings them together (docs/plan.md item 10)."""
from __future__ import annotations

import math
import time
from dataclasses import asdict, dataclass, field, replace

from . import dsp, predict
from .api import ApiError, Device
from .capture import Capture, TempProbe, differential

LEVELS = [-30, -24, -20, -16, -12, -10, -8, -6, -5, -4, -3, -2.5, -2, -1.5, -1,
          -0.75, -0.5, -0.25, 0]
STREAMING = ["Spotify Connect plays", "AirPlay plays", "Bluetooth A2DP pairs and plays",
             "Lyrion (LMS) finds the player and plays", "RCA line in plays through",
             "DJ-direct bypass passes the FLX4 with the board's DSP off",
             "Sub outputs carry the low end (both RCAs)", "Buttons: vol-, vol+, play/source, hold = pairing",
             "IR remote learns and works"]
# Each model's own controls (docs/firmware.md; board_core.c, board_lite.c)
CONTROLS = {
    "hifi-lite": "Buttons: vol-, vol+, play/source, hold = pairing",
    "hifi-plus": "Controls: knob turns volume, push = play/pause (hold: power); "
                 "BTN1 = source (hold: mute); BTN2 = next (hold: previous)",
    "hifi-ultra": "Front controls: knob turns volume, push = play/pause (hold: standby); "
                  "BTN1 = input (hold: mute); BTN2 = previous; BTN3 = next",
}


def streaming(model: str | None, board: str | None = None) -> list[str]:
    """The by-ear-and-hand checklist with the board's own controls item. The
    model comes from info (hifi-lite, hifi-plus, hifi-ultra); with none, the
    board name (core means Plus)."""
    model = model or {"lite-main": "hifi-lite", "core": "hifi-plus"}.get(board or "", "hifi-lite")
    item = CONTROLS.get(model, CONTROLS["hifi-lite"])
    return [item if i.startswith("Buttons:") else i for i in STREAMING]


def supply(state: dict) -> dict:
    """The source and contract from /state in either shape: the firmware's
    power.contract {protocol, v, i_max_a} (hifi_api.c), or the mock's
    pd {source, contract_v, contract_a}. low is True on a source the amp
    may not run from (under 9 V, the firmware's own pd_contract rule)."""
    if "power" in state:
        p = state.get("power") or {}
        c = p.get("contract") or {}
        out = {"source": c.get("protocol"), "contract_v": c.get("v"), "contract_a": c.get("i_max_a")}
        v = out["contract_v"] if out["contract_v"] is not None else p.get("vbus_v")
    else:
        pd = state.get("pd") or {}
        out = {"source": pd.get("source"), "contract_v": pd.get("contract_v"), "contract_a": pd.get("contract_a")}
        v = out["contract_v"]
    out["low"] = out["source"] == "5v" or (v is not None and v < 9)
    return out


def amp_powered(state: dict) -> bool:
    """PVDD on or the amp enabled, in either /state shape."""
    if "power" in state:
        return bool((state.get("power") or {}).get("pvdd_on") or (state.get("amp") or {}).get("enabled"))
    return bool((state.get("amp") or {}).get("enabled"))


# Self-test items that can't pass while the board sits on 5 V: no contract, so
# no PVDD and no amp (hifi_main.c op_selftest; the mock's names too)
FAIL_ON_5V = ("pd_contract", "pvdd_on", "pvdd_sensed", "amp_no_fault", "amp_faultz_high")
# ... and before --wifi has set the network (stage 1 sets it after the self-test)
FAIL_OFFLINE = ("network", "wifi_or_eth_up")


class Rig:
    """What a person does at the bench: swap loads, change chargers, answer
    yes/no. Interactive by default; the mock's rig does it itself."""

    interactive = True

    def __init__(self, ask: bool = True):
        self.interactive = ask

    def say(self, msg: str):
        print(msg, flush=True)

    def set_load(self, ohm: float):
        self.wait(f"Connect the {ohm:g} ohm dummy loads (both channels) and the capture leads to the L channel.")

    def set_charger(self, name: str):
        self.wait(f"Power the board from: {name}.")

    def wait(self, msg: str):
        self.say("> " + msg)
        if self.interactive:
            input("  press Enter when done ")

    def check(self, item: str) -> str:
        if not self.interactive:
            return "not run"
        a = input(f"  {item}? [y/n/s(kip)] ").strip().lower()[:1]
        return {"y": "pass", "n": "fail"}.get(a, "not run")


@dataclass
class Config:
    board: str = "lite-main"
    volts_per_fs: float = 30.0   # differential volts at the speaker terminals per capture full scale
    mode: str = "diff"
    freq_hz: float = 1000.0
    channel: str = "both"        # the channels driven; the capture is on one of them
    settle_s: float = 0.3
    record_s: float = 0.4
    rest_s: float = 0.3          # tone off between steps, so the dummy loads cool
    stop_thdn: float = 0.10
    max_w: float = 120.0         # never drive past this, whatever the THD says
    soak_s: float = 600.0
    soak_every_s: float = 15.0
    temp_limit_c: float = 100.0
    levels: list = field(default_factory=lambda: list(LEVELS))


@dataclass
class Point:
    ohm: float
    governor: bool
    level_dbfs: float
    freq_hz: float
    vrms: float
    power_w: float
    thd_pct: float
    thdn_pct: float
    pvdd_v: float | None
    vbus_v: float | None
    temp_c: float | None
    faultz: bool | None
    gov_active: bool | None
    gov_limit_db: float | None


def measure(dev: Device, cap: Capture, cfg: Config, ohm: float, gov: bool, level: float,
            probe: TempProbe | None = None) -> Point | None:
    dev.tone(cfg.freq_hz, level, cfg.channel)
    time.sleep(cfg.settle_s)
    chans = cap.record(cfg.record_s)
    st = dev.state()
    if cfg.rest_s:
        dev.tone_stop()
    x = [v * cfg.volts_per_fs for v in differential(chans, cfg.mode)]
    try:
        t = dsp.analyse(x, cap.rate, cfg.freq_hz)
    except ValueError:
        t = None
    amp, g = st.get("amp", {}), st.get("governor", {})
    temp = probe.read(st) if probe and probe.kind != "manual" else st.get("temp_c")
    if t is None:
        vrms = dsp.rms(x)
        return Point(ohm, gov, level, 0, vrms, vrms * vrms / ohm, float("nan"), float("nan"),
                     st.get("pvdd_v"), st.get("vbus_v"), temp, amp.get("faultz"), g.get("active"), g.get("limit_db"))
    return Point(ohm, gov, level, round(t.freq_hz, 3), round(t.rms, 4), round(t.rms ** 2 / ohm, 3),
                 round(t.thd * 100, 4), round(t.thdn * 100, 4), st.get("pvdd_v"), st.get("vbus_v"), temp,
                 amp.get("faultz"), g.get("active"), g.get("limit_db"))


def at_thdn(points: list[Point], pct: float) -> float | None:
    """Power where THD+N first crosses pct, interpolated in log(THD+N)."""
    prev = None
    for p in points:
        if math.isnan(p.thdn_pct) or p.thdn_pct <= 0:
            continue
        if p.thdn_pct >= pct:
            if prev is None:
                return None
            a, b = math.log(prev.thdn_pct), math.log(p.thdn_pct)
            f = (math.log(pct) - a) / (b - a) if b != a else 1
            return round(prev.power_w + f * (p.power_w - prev.power_w), 2)
        prev = p
    return None


def sweep(dev: Device, cap: Capture, cfg: Config, ohm: float, gov: bool, probe=None, say=print) -> dict:
    """Step the 1 kHz tone up into one dummy load until 10% THD+N, a fault, or max_w."""
    dev.volume(0)
    dev.governor(gov)
    dev.amp(True)
    pd = supply(dev.state())
    contract = f"{pd['contract_v'] if pd['contract_v'] is not None else '?'} V " \
               f"{pd['contract_a'] if pd['contract_a'] is not None else '?'} A"
    pred = predict.predicted(cfg.board, ohm)
    budget = (pd["contract_v"] or 0) * (pd["contract_a"] or 0)
    # The amp's own limit, ungoverned: if the charger can't feed both channels
    # at the predicted power (Plus at 4 ohm: 2 x 77 W of sine is ~175 W in,
    # past 140 W), drive only the measured one rather than trip the charger.
    driven = cfg.channel
    if not gov and driven == "both" and pred and budget and 2 * pred / 0.88 > 0.95 * budget:
        driven = "L"
    cfg = replace(cfg, channel=driven)
    pts: list[Point] = []
    stop = "top level reached"
    try:
        for lv in cfg.levels:
            p = measure(dev, cap, cfg, ohm, gov, lv, probe)
            pts.append(p)
            say(f"  {ohm:g} ohm gov={'on' if gov else 'off'} {lv:6.2f} dBFS: {p.power_w:7.2f} W "
                f"THD+N {p.thdn_pct:7.3f}%  PVDD {p.pvdd_v}")
            if p.faultz is False:
                stop = "FAULTZ low (the amp or the charger shut down)"
                break
            if math.isnan(p.thdn_pct):
                stop = "no tone in the capture"
                break
            if p.thdn_pct >= cfg.stop_thdn * 100:
                stop = f"THD+N past {cfg.stop_thdn * 100:g}%"
                break
            if p.power_w >= cfg.max_w:
                stop = f"past max_w {cfg.max_w:g} W"
                break
    finally:
        dev.tone_stop()
    p1, p10 = at_thdn(pts, 1.0), at_thdn(pts, 10.0)
    pvdds = [p.pvdd_v for p in pts if p.pvdd_v]
    held = min(pvdds) if pvdds else None
    model = predict.at_pvdd(cfg.board, held, ohm) if held else None
    # Only the ungoverned sweep is the amp's own limit; with the governor on,
    # the sweep shows where the charger budget stops it.
    verdict = None
    if not gov and pred:
        verdict = "pass" if p1 and p1 >= predict.PASS_RATIO * pred else "fail"
    clean = [p for p in pts if p.thdn_pct == p.thdn_pct and p.power_w >= 1]
    return {"ohm": ohm, "governor": gov, "contract": contract, "driven": driven, "stop": stop, "p1_w": p1, "p10_w": p10,
            "predicted_w": pred, "model_at_pvdd_w": round(model, 2) if model else None,
            "pvdd_min_v": held, "thdn_1w_pct": clean[0].thdn_pct if clean else None,
            "gov_engaged": any(p.gov_active for p in pts), "verdict": verdict,
            "points": [asdict(p) for p in pts]}


def level_for(points: list[dict], target_w: float) -> float | None:
    """The tone level giving target_w, from the cleanest point of a sweep."""
    ok = [p for p in points if p["thdn_pct"] == p["thdn_pct"] and p["thdn_pct"] < 0.5 and p["power_w"] > 0.1]
    if not ok:
        return None
    ref = max(ok, key=lambda p: p["power_w"])
    return round(ref["level_dbfs"] + 10 * math.log10(target_w / ref["power_w"]), 2)


def cool(dev: Device, cfg: Config, probe: TempProbe, say=print, clock=time.monotonic, sleep=time.sleep,
         max_s: float = 600.0) -> float | None:
    """Amp on, no tone, until the chip temperature stops falling (under
    0.5 C per reading three times running), so a soak doesn't start warm
    from the sweep before it."""
    dev.tone_stop()
    t0, last, still = clock(), None, 0
    while clock() - t0 < max_s:
        t = probe.read(dev.state())
        if t is None:
            return None
        say(f"  cooling: chip {t} C")
        still = still + 1 if last is not None and last - t < 0.5 else 0
        if still >= 3:
            return t
        last = t
        sleep(cfg.soak_every_s)
    return last


def soak(dev: Device, cap: Capture, cfg: Config, ohm: float, target_w: float, level: float,
         probe: TempProbe, say=print, clock=time.monotonic, sleep=time.sleep) -> dict:
    """Hold a tone at target_w per channel and log the chip temperature."""
    dev.volume(0)
    dev.governor(True)
    dev.amp(True)
    start = cool(dev, cfg, probe, say, clock, sleep) if probe.kind != "manual" else None
    rows = []
    t0 = clock()
    stop = "done"
    dev.tone(cfg.freq_hz, level, cfg.channel)
    try:
        while True:
            el = clock() - t0
            st = dev.state()
            x = [v * cfg.volts_per_fs for v in differential(cap.record(cfg.record_s), cfg.mode)]
            try:
                w = round(dsp.analyse(x, cap.rate, cfg.freq_hz).rms ** 2 / ohm, 3)
            except ValueError:
                w = 0.0
            temp = probe.read(st)
            rows.append({"t_s": round(el, 1), "power_w": w, "temp_c": temp, "pvdd_v": st.get("pvdd_v"),
                         "faultz": st.get("amp", {}).get("faultz")})
            say(f"  soak {el:6.0f} s: {w:6.2f} W  chip {temp} C  PVDD {st.get('pvdd_v')}")
            if st.get("amp", {}).get("faultz") is False:
                stop = "FAULTZ low"
                break
            if temp is not None and temp >= cfg.temp_limit_c:
                stop = f"chip reached {cfg.temp_limit_c:g} C"
                break
            if el >= cfg.soak_s:
                break
            sleep(cfg.soak_every_s)
    finally:
        dev.tone_stop()
    temps = [r["temp_c"] for r in rows if r["temp_c"] is not None]
    last = temps[-1] if temps else None
    verdict = None
    if temps:
        verdict = "pass" if stop == "done" and last < cfg.temp_limit_c else "fail"
    return {"ohm": ohm, "target_w": round(target_w, 2), "level_dbfs": level, "stop": stop,
            "temp_start_c": start, "temp_end_c": last, "temp_max_c": max(temps) if temps else None, "probe": probe.kind,
            "verdict": verdict or "no temperature (give --temp-cmd or --temp-manual)", "rows": rows}


def selftest(dev: Device, expect_5v: bool = False, expect_offline: bool = False, settle_s: float = 0.5) -> dict:
    """The board's own self-test, plus the 5 V rule checked from outside:
    on a 5 V-only port the amp must refuse to start. On 5 V the items that
    need a contract (FAIL_ON_5V) are expected to fail and marked so; with
    expect_offline (before --wifi), the network items too."""
    out = dev.selftest()
    st = dev.state()
    sup = supply(st)
    on_5v = expect_5v or sup["low"]
    extra = []
    if on_5v:
        # The firmware takes the request (200) and its sequencer keeps PVDD
        # off without power-good; the mock answers 409. Either is a refusal.
        after = st
        try:
            dev.amp(True)
            deadline = time.monotonic() + settle_s
            while True:
                after = dev.state()
                refused = not amp_powered(after)
                if not refused or time.monotonic() >= deadline:
                    break
                time.sleep(0.05)
        except ApiError as e:
            refused = e.status in (403, 409)
        finally:
            try:
                dev.amp(False)
            except ApiError:
                pass
        vbus = (after.get("power") or {}).get("vbus_v", after.get("vbus_v"))
        extra.append({"name": "bench: amp refused on 5 V", "pass": refused, "value": vbus,
                      "limit": "refused", "detail": "PVDD stays off without a contract's power-good"})
    expected = (FAIL_ON_5V if on_5v else ()) + (FAIL_OFFLINE if expect_offline else ())
    for i in out.get("items", []):
        if i["name"] in expected and not i["pass"]:
            i["expected"] = True
    items = out.get("items", []) + extra
    ok = all(i["pass"] or i.get("expected") for i in items)
    return {"pass": ok, "source": sup["source"], "contract_v": sup["contract_v"], "items": items}
