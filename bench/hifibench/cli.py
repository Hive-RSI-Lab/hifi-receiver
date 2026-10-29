"""bench.py: find, flash and test a receiver board. docs/bench.md is the guide."""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from . import discover, flash, mock, predict, report, run
from .api import ApiError, Console, Device, Http
from .capture import Arecord, SoundDevice, TempProbe, WavFile, differential
from .serialport import open_port

HERE = Path(__file__).resolve().parent.parent
RIG = HERE / "rig.json"
RESULTS = HERE / "results"


class MockRig(run.Rig):
    """The mock's hands: it swaps its own loads and chargers."""

    interactive = False

    def __init__(self, sim: mock.Sim):
        self.sim = sim

    def set_load(self, ohm):
        self.say(f"> (mock) {ohm:g} ohm loads")
        self.sim.load = float(ohm)

    def set_charger(self, name):
        self.say(f"> (mock) power from {name}")
        self.sim.set_charger(name)

    def wait(self, msg):
        self.say("> (mock) " + msg)

    def check(self, item):
        return "not run (mock)"


class Bench:
    """The device, capture, probe and rig one command works with."""

    def __init__(self, a):
        self.a = a
        rig = json.loads(RIG.read_text()) if RIG.is_file() else {}
        self.cfg = run.Config(board=a.board or rig.get("board", "lite-main"),
                              volts_per_fs=a.volts_per_fs or rig.get("volts_per_fs", 30.0),
                              mode=rig.get("mode", "diff"))
        if getattr(a, "soak_s", None) is not None:
            self.cfg.soak_s = a.soak_s
        self.sim = None
        if a.mock:
            self.sim = mock.Sim(self.cfg.board, charger=a.mock_charger, time_scale=a.mock_time_scale)
            self.cfg.settle_s = self.cfg.rest_s = 0
            self.rig = MockRig(self.sim)
        else:
            self.rig = run.Rig(ask=not a.yes)
        self.capture_spec = a.capture or rig.get("capture")
        temp_cmd = a.temp_cmd or rig.get("temp_cmd")
        self._cap = None
        if self.sim:
            srv = mock.serve(self.sim)
            self.mock_url = f"http://127.0.0.1:{srv.server_port}"
            self.probe = TempProbe(fn=lambda: round(self.sim.tick() and self.sim.temp, 2))
        else:
            self.probe = TempProbe(cmd=temp_cmd, manual=a.temp_manual)
        self.via = None

    # ---- the board ---------------------------------------------------------
    def device(self, prefer: str = "any") -> Device:
        a = self.a
        if self.sim:
            if prefer == "serial" or (prefer == "any" and self.sim.source == "5v"):
                self.via = "serial (mock pty)"
                return Device(Console(open_port(mock.serve_pty(self.sim)), name="mock pty"))
            self.via = self.mock_url
            return Device(Http(self.mock_url))
        if a.url and prefer != "serial":
            self.via = a.url
            return Device(Http(a.url))
        if a.port:
            self.via = a.port
            return Device(Console(open_port(a.port), name=a.port))
        if prefer != "serial":
            for _ in range(3 if prefer == "net" else 1):
                found = discover.browse(2.0)
                if found:
                    self.via = found[0].url
                    return Device(Http(found[0].url))
        ports = discover.serial_ports()
        if ports and prefer != "net":
            self.via = ports[0]
            return Device(Console(open_port(ports[0]), name=ports[0]))
        sys.exit("no board found: give --url or --port (bench.py find lists what's visible)")

    def capture(self):
        if self._cap:
            return self._cap
        if self.sim:
            self._cap = mock.MockCapture(self.sim, self.cfg.volts_per_fs)
            return self._cap
        spec = self.capture_spec
        if not spec:
            sys.exit("no capture: give --capture arecord:hw:1,0 | sounddevice[:NAME] | wav:FILE (docs/bench.md)")
        kind, _, arg = spec.partition(":")
        self._cap = {"arecord": lambda: Arecord(arg), "sounddevice": lambda: SoundDevice(arg or None),
                     "wav": lambda: WavFile(arg)}[kind]()
        return self._cap

    def outdir(self, info: dict) -> Path:
        if self.a.out:
            return Path(self.a.out)
        tag = "mock-" if self.sim else ""
        mac = info.get("mac", "x").replace(":", "")[-6:]
        return RESULTS / f"{tag}{info.get('board', self.cfg.board)}-{mac}-{time.strftime('%Y%m%d-%H%M%S')}"

    def session(self, dev: Device) -> dict:
        info = dev.info()
        return {"board": info.get("board", self.cfg.board), "info": info, "via": self.via, "mock": bool(self.sim),
                "started": time.strftime("%Y-%m-%d %H:%M:%S"), "pass_ratio": predict.PASS_RATIO,
                "config": vars(self.cfg), "selftests": [], "sweeps": [], "verdicts": {}}


def loads(s: str) -> list[float]:
    return [float(x) for x in s.split(",") if x]


def governors(s: str) -> list[bool]:
    return {"on": [True], "off": [False], "both": [False, True]}[s]


# ---- commands ---------------------------------------------------------------

def cmd_find(a, b: Bench):
    ports = discover.serial_ports()
    print("USB (CH340):", ", ".join(ports) or "none")
    found = discover.browse(a.timeout)
    print("mDNS _hifi._tcp:", "none" if not found else "")
    for f in found:
        print(f"  {f.name}  {f.url}  {f.txt}")
    return 0


def cmd_info(a, b: Bench):
    dev = b.device()
    print(json.dumps({"via": b.via, "info": dev.info(), "state": dev.state()}, indent=1))
    return 0


def cmd_flash(a, b: Bench):
    port = a.port or (discover.serial_ports() or [None])[0]
    if not port:
        sys.exit("no CH340 port found: plug the board into USB or give --port")
    return flash.flash(port, a.image, a.baud, a.esptool, a.dry_run)


def cmd_selftest(a, b: Bench):
    dev = b.device()
    r = run.selftest(dev)
    for i in r["items"]:
        print(f"  {'ok ' if i['pass'] else ('exp' if i.get('expected') else 'BAD')} {i['name']}: {i.get('value')} ({i.get('limit')})")
    print("self-test:", "pass" if r["pass"] else "FAIL")
    return 0 if r["pass"] else 1


def cmd_tone(a, b: Bench):
    dev = b.device()
    if a.stop:
        dev.tone_stop()
        return 0
    dev.volume(a.volume)
    dev.amp(True)
    print(dev.tone(a.freq, a.level, a.channel, a.duration))
    return 0


def cmd_calibrate(a, b: Bench):
    """Play a tone; the person reads the AC volts across the load on a meter."""
    dev = b.device()
    dev.volume(0)
    dev.amp(True)
    dev.tone(1000, a.level, "both")
    time.sleep(0.5)
    chans = b.capture().record(1.0)
    dev.tone_stop()
    rec = run.dsp.analyse(differential(chans, b.cfg.mode), b.capture().rate, 1000).rms
    vrms = a.vrms if a.vrms else float(input("AC volts across the L load (meter, true RMS): "))
    vpfs = vrms / rec
    rig = json.loads(RIG.read_text()) if RIG.is_file() else {}
    rig.update({"volts_per_fs": round(vpfs, 4), "mode": b.cfg.mode})
    if b.capture_spec:
        rig["capture"] = b.capture_spec
    if not b.sim:
        RIG.write_text(json.dumps(rig, indent=1) + "\n")
    print(f"volts_per_fs = {vpfs:.4f}" + ("" if b.sim else f" (saved in {RIG})"))
    return 0


def _sweeps(b: Bench, dev: Device, s: dict, ohms, govs):
    for ohm in ohms:
        b.rig.set_load(ohm)
        for g in govs:
            sw = run.sweep(dev, b.capture(), b.cfg, ohm, g, b.probe if b.probe.kind != "manual" else None)
            s["sweeps"].append(sw)
            if sw["verdict"]:
                s["verdicts"][f"power into {ohm:g} Ω (1% THD+N ≥ {predict.PASS_RATIO:.0%} of {sw['predicted_w']:g} W)"] = (
                    f"{sw['verdict']}: {sw['p1_w']} W")


def _soak(b: Bench, dev: Device, s: dict, ohm: float):
    ref = [w for w in s["sweeps"] if w["ohm"] == ohm]
    p1 = next((w["p1_w"] for w in ref if not w["governor"] and w["p1_w"]), None) or predict.predicted(b.cfg.board, ohm)
    target = p1 / 8
    level = run.level_for([p for w in ref for p in w["points"]], target) if ref else None
    if level is None:
        sw = run.sweep(dev, b.capture(), b.cfg, ohm, True)
        level = run.level_for(sw["points"], target)
    b.rig.set_load(ohm)
    b.rig.wait("Fix the temperature probe to the top of the amp chip.")
    clock = dict(clock=b.sim.now, sleep=b.sim.advance) if b.sim else {}
    s["soak"] = run.soak(dev, b.capture(), b.cfg, ohm, target, level, b.probe, **clock)
    s["verdicts"][f"chip temperature at 1/8 power into {ohm:g} Ω"] = (
        f"{s['soak']['verdict']}: {s['soak']['temp_end_c']} °C")


def cmd_sweep(a, b: Bench):
    dev = b.device()
    s = b.session(dev)
    _sweeps(b, dev, s, loads(a.loads), governors(a.governor))
    if a.soak:
        _soak(b, dev, s, loads(a.loads)[0])
    print("wrote", report.write(b.outdir(s["info"]), s))
    return 0 if all(not v.startswith("fail") for v in s["verdicts"].values()) else 1


def cmd_soak(a, b: Bench):
    dev = b.device()
    s = b.session(dev)
    _soak(b, dev, s, a.ohm)
    print("wrote", report.write(b.outdir(s["info"]), s))
    return 0 if s["soak"]["verdict"] == "pass" else 1


def cmd_bringup(a, b: Bench):
    """docs/plan.md item 10, in stages; each one stops the run if it fails."""
    s = None
    try:
        # 1. A laptop's USB port: 5 V, the console, the amp held off.
        b.rig.set_charger("laptop")
        dev = b.device("serial")
        if a.image:
            if b.sim:
                print("> (mock) flash skipped")
            elif flash.flash(b.a.port or b.via, a.image, a.baud, a.esptool) != 0:
                sys.exit("flash failed")
        s = b.session(dev)
        st = run.selftest(dev, expect_5v=True)
        st["stage"] = "laptop USB (5 V)"
        s["selftests"].append(st)
        s["verdicts"]["self-test on a laptop port, amp held off"] = "pass" if st["pass"] else "fail"
        if not st["pass"]:
            return _end(b, s, 1)
        if a.wifi:
            ssid, _, pw = a.wifi.partition(":")
            dev.wifi(ssid, pw)
        # 2. Each charger: a contract, a clean self-test, and the governor holding it at 4 ohm.
        chargers = a.chargers.split(",")
        for ch in chargers:
            b.rig.set_charger(ch)
            dev = b.device("net")
            st = run.selftest(dev)
            st["stage"] = ch
            s["selftests"].append(st)
            s["verdicts"][f"self-test on {ch}"] = "pass" if st["pass"] else "fail"
            if not st["pass"]:
                return _end(b, s, 1)
            if ch != chargers[-1]:
                _sweeps(b, dev, s, [loads(a.loads)[0]], [True])
        # 3. The full sweep on the biggest charger, then the 1/8-power soak.
        _sweeps(b, dev, s, loads(a.loads), governors(a.governor))
        if not a.no_soak:
            _soak(b, dev, s, loads(a.loads)[0])
        # 4. What only a person can hear and press.
        b.rig.wait("Now the streaming and controls checks (answer each).")
        s["checklist"] = {item: b.rig.check(item) for item in run.STREAMING}
    except ApiError as e:
        if s is None:
            raise
        s.setdefault("events", []).append(f"stopped on an API error: {e}")
        return _end(b, s, 1)
    return _end(b, s, 0 if all(not v.startswith("fail") for v in s["verdicts"].values()) else 1)


def _end(b: Bench, s: dict, code: int) -> int:
    if b.sim:
        s.setdefault("events", []).extend(b.sim.events)
    path = report.write(b.outdir(s["info"]), s)
    print("wrote", path)
    for k, v in s["verdicts"].items():
        print(f"  {k}: {v}")
    return code


def cmd_mock(a, b: Bench):
    print("mock HTTP:", b.mock_url)
    print("mock console:", mock.serve_pty(b.sim))
    print("Ctrl-C to stop")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="bench.py", description=__doc__)
    ap.add_argument("--url", help="the board's HTTP base, e.g. http://hifi.local")
    ap.add_argument("--port", help="the board's serial port (its CH340)")
    ap.add_argument("--board", help="lite-main or core (default from the board, or rig.json)")
    ap.add_argument("--capture", help="arecord:hw:1,0 | sounddevice[:NAME] | wav:FILE")
    ap.add_argument("--volts-per-fs", type=float, help="override rig.json's calibration")
    ap.add_argument("--temp-cmd", help="a command that prints the chip temperature in C")
    ap.add_argument("--temp-manual", action="store_true", help="type the chip temperature in when asked")
    ap.add_argument("--out", help="results folder (default bench/results/<board>-<mac>-<time>)")
    ap.add_argument("--yes", action="store_true", help="don't wait for the person between steps")
    ap.add_argument("--mock", action="store_true", help="a simulated board instead of a real one")
    ap.add_argument("--mock-charger", default="100w", choices=sorted(mock.CHARGERS))
    ap.add_argument("--mock-time-scale", type=float, default=1.0, help="thermal speed-up in the mock")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("find", help="list boards on USB and mDNS")
    p.add_argument("--timeout", type=float, default=2.0)
    sub.add_parser("info", help="the board's info and state")
    p = sub.add_parser("flash", help="flash an ESP-IDF build dir or merged .bin")
    p.add_argument("image")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--esptool", help="the esptool command (default: python -m esptool)")
    p.add_argument("--dry-run", action="store_true")
    sub.add_parser("selftest", help="run the board's self-test")
    p = sub.add_parser("tone", help="play a test tone")
    p.add_argument("--freq", type=float, default=1000)
    p.add_argument("--level", type=float, default=-20)
    p.add_argument("--channel", default="both", choices=["L", "R", "both", "sub"])
    p.add_argument("--duration", type=float, default=0)
    p.add_argument("--volume", type=float, default=-20)
    p.add_argument("--stop", action="store_true")
    p = sub.add_parser("calibrate", help="set volts_per_fs from a meter reading")
    p.add_argument("--vrms", type=float)
    p.add_argument("--level", type=float, default=-20)
    for name in ("sweep", "bringup"):
        p = sub.add_parser(name, help={"sweep": "power sweep into dummy loads",
                                       "bringup": "the staged bring-up, laptop to charger to full test"}[name])
        p.add_argument("--loads", default="4,6,8")
        p.add_argument("--governor", default="both", choices=["on", "off", "both"])
        p.add_argument("--soak-s", type=float)
        if name == "sweep":
            p.add_argument("--soak", action="store_true", help="then soak at 1/8 power into the first load")
        else:
            p.add_argument("--image", help="flash this first (stage 1)")
            p.add_argument("--baud", type=int, default=921600)
            p.add_argument("--esptool")
            p.add_argument("--wifi", help="SSID:PASSWORD, set over the console in stage 1")
            p.add_argument("--chargers", default="65w,100w")
            p.add_argument("--no-soak", action="store_true")
    p = sub.add_parser("soak", help="the chip temperature at 1/8 power")
    p.add_argument("--ohm", type=float, default=4)
    p.add_argument("--soak-s", type=float)
    sub.add_parser("mock", help="serve the mock board (HTTP and a pty console) for the app or by hand")
    a = ap.parse_args(argv)
    if a.cmd == "mock":
        a.mock = True
    b = Bench(a)
    return globals()["cmd_" + a.cmd](a, b)
