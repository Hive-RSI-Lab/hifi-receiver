"""The bench against itself and the mock: python3 -m unittest discover -s bench/tests"""
import io
import json
import math
import os
import random
import socket
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from hifibench import api, cli, discover, dsp, flash, mock, predict, run  # noqa: E402
from hifibench.capture import TempProbe  # noqa: E402
from hifibench.serialport import open_port  # noqa: E402


def sine(n, fs, f, amp=1.0, h3=0.0, noise=0.0, seed=1):
    r = random.Random(seed)
    return [amp * math.sin(2 * math.pi * f * i / fs) + h3 * amp * math.sin(3 * 2 * math.pi * f * i / fs)
            + r.gauss(0, noise) for i in range(n)]


class Dsp(unittest.TestCase):
    def test_clean_tone_off_nominal_clock(self):
        f = 1000 * (1 + 150e-6)  # the board's clock 150 ppm off the interface's
        t = dsp.analyse(sine(19200, 48000, f, 0.5, noise=1e-6), 48000, 1000)
        self.assertAlmostEqual(t.freq_hz, f, delta=0.01)
        self.assertAlmostEqual(t.amp, 0.5, delta=1e-4)
        self.assertLess(t.thdn, 1e-4)

    def test_third_harmonic(self):
        t = dsp.analyse(sine(19200, 48000, 997, 0.3, h3=0.01), 48000, 1000)
        self.assertAlmostEqual(t.thd * 100, 1.0, delta=0.02)
        self.assertAlmostEqual(t.thdn * 100, 1.0, delta=0.05)

    def test_hard_clip_raises_thd(self):
        x = [max(-0.9, min(0.9, v)) for v in sine(19200, 48000, 1000, 1.0)]
        self.assertGreater(dsp.analyse(x, 48000).thd, 0.02)

    def test_silence_is_an_error(self):
        with self.assertRaises(ValueError):
            dsp.analyse([0.0] * 4800, 48000)

    def test_wrong_tone_is_an_error(self):
        with self.assertRaises(ValueError):
            dsp.analyse(sine(9600, 48000, 440), 48000, 1000)


class Predict(unittest.TestCase):
    def test_model_matches_product_table(self):
        for board, t in predict.TIERS.items():
            for ohm, w in t["p1"].items():
                self.assertAlmostEqual(predict.model_watts(t["pvdd"], ohm, t["rs"]), w, delta=1.0, msg=(board, ohm))

    def test_at_thdn_interpolates(self):
        pts = [run.Point(4, False, l, 1000, 0, p, 0, t, None, None, None, True, False, 0)
               for l, p, t in ((-3, 30, 0.01), (-2, 38, 0.5), (-1, 42, 2.0), (0, 45, 12.0))]
        self.assertTrue(38 < run.at_thdn(pts, 1.0) < 42)
        self.assertTrue(42 < run.at_thdn(pts, 10.0) < 45)
        self.assertIsNone(run.at_thdn(pts[:2], 1.0))


class Mdns(unittest.TestCase):
    def test_parse_answer(self):
        def name(n):
            return discover._qname(n)
        inst = "hifi-a1b2c3._hifi._tcp.local."
        recs = b""
        recs += name(discover.SERVICE) + struct.pack("!HHIH", 12, 1, 120, len(name(inst))) + name(inst)
        srv = struct.pack("!HHH", 0, 0, 80) + name("hifi-a1b2c3.local.")
        recs += name(inst) + struct.pack("!HHIH", 33, 0x8001, 120, len(srv)) + srv
        txt = b"".join(bytes([len(s)]) + s for s in (b"board=lite-main", b"api=1"))
        recs += name(inst) + struct.pack("!HHIH", 16, 0x8001, 120, len(txt)) + txt
        recs += name("hifi-a1b2c3.local.") + struct.pack("!HHIH", 1, 0x8001, 120, 4) + socket.inet_aton("192.168.1.42")
        pkt = struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 3) + recs
        found = {}
        discover.parse(pkt, found)
        f = found[inst]
        self.assertEqual(f.url, "http://192.168.1.42:80")
        self.assertEqual(f.txt["board"], "lite-main")

    def test_query_asks_for_ptr(self):
        q = discover.query()
        self.assertTrue(q.endswith(struct.pack("!HH", 12, 0x8001)))


class Flash(unittest.TestCase):
    def test_build_dir_and_bin(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "flash_args").write_text("--flash_mode dio\n0x1000 bootloader/bootloader.bin\n")
            cmd, cwd = flash.command("/dev/ttyUSB0", d, esptool="esptool.py")
            self.assertEqual(cwd, d)
            self.assertEqual(cmd[-1], "@flash_args")
            self.assertIn("default_reset", cmd)
            b = Path(d, "merged.bin")
            b.write_bytes(b"\0")
            cmd, cwd = flash.command("/dev/ttyUSB0", str(b))
            self.assertEqual(cmd[-2:], ["0x0", str(b.resolve())])
            with self.assertRaises(FileNotFoundError):
                flash.command("/dev/ttyUSB0", str(Path(d, "bootloader")))


class Transports(unittest.TestCase):
    def test_console_over_a_pty_skips_log_lines(self):
        sim = mock.Sim(charger="laptop")
        dev = api.Device(api.Console(open_port(mock.serve_pty(sim)), timeout=5))
        self.assertEqual(dev.info()["board"], "lite-main")
        with self.assertRaises(api.ApiError) as e:
            dev.amp(True)
        self.assertEqual(e.exception.status, 409)

    def test_http_errors(self):
        sim = mock.Sim()
        dev = api.Device(api.Http(f"http://127.0.0.1:{mock.serve(sim).server_port}"))
        with self.assertRaises(api.ApiError) as e:
            dev.tone(1000, -20, "X")
        self.assertEqual(e.exception.status, 400)


class MockBoard(unittest.TestCase):
    def setUp(self):
        self.sim = mock.Sim(charger="100w")
        self.dev = api.Device(api.Http(f"http://127.0.0.1:{mock.serve(self.sim).server_port}"))
        self.cfg = run.Config(settle_s=0, rest_s=0, record_s=0.2)
        self.cap = mock.MockCapture(self.sim, self.cfg.volts_per_fs)

    def test_selftest_on_5v_expects_no_contract(self):
        self.sim.set_charger("laptop")
        r = run.selftest(self.dev, expect_5v=True)
        self.assertTrue(r["pass"])
        self.assertTrue(any(i["name"] == "bench: amp refused on 5 V" and i["pass"] for i in r["items"]))

    def test_selftest_reports_a_fault(self):
        self.sim.faults = {"codec"}
        self.assertFalse(run.selftest(self.dev)["pass"])

    def test_sweep_finds_the_predicted_power(self):
        for ohm in (4, 8):
            self.sim.load = ohm
            r = run.sweep(self.dev, self.cap, self.cfg, ohm, False, say=lambda *a: None)
            self.assertEqual(r["verdict"], "pass")
            self.assertAlmostEqual(r["p1_w"], predict.predicted("lite-main", ohm), delta=0.1 * r["predicted_w"])

    def test_small_charger_trips_without_the_governor(self):
        self.sim.set_charger("65w")
        self.sim.load = 4
        r = run.sweep(self.dev, self.cap, self.cfg, 4, False, say=lambda *a: None)
        self.assertEqual(r["driven"], "L")  # 2 x 43 W can't come from 65 W: one channel
        self.assertNotIn("FAULTZ", r["stop"])
        # Both channels at full level, ungoverned, do trip it.
        self.dev.governor(False)
        self.dev.amp(True)
        self.dev.tone(1000, 0, "both")
        self.assertFalse(self.dev.state()["amp"]["faultz"])
        self.dev.tone_stop()
        r = run.sweep(self.dev, self.cap, self.cfg, 4, True, say=lambda *a: None)
        self.assertNotIn("FAULTZ", r["stop"])
        self.assertTrue(r["gov_engaged"])

    def test_plus_measures_one_channel_when_the_budget_cant_feed_two(self):
        sim = mock.Sim("core", charger="140w")
        sim.load = 4
        dev = api.Device(api.Http(f"http://127.0.0.1:{mock.serve(sim).server_port}"))
        cfg = run.Config(board="core", settle_s=0, rest_s=0, record_s=0.2)
        r = run.sweep(dev, mock.MockCapture(sim, cfg.volts_per_fs), cfg, 4, False, say=lambda *a: None)
        self.assertEqual(r["driven"], "L")
        self.assertEqual(r["verdict"], "pass")
        self.assertAlmostEqual(r["p1_w"], 77, delta=8)

    def test_soak_logs_temperature(self):
        self.sim.load = 4
        probe = TempProbe(fn=lambda: self.sim.temp)
        r = run.soak(self.dev, self.cap, self.cfg, 4, 43 / 8, -10.2, probe, say=lambda *a: None,
                     clock=self.sim.now, sleep=self.sim.advance)
        self.assertEqual(r["verdict"], "pass")
        self.assertGreater(r["temp_end_c"], r["temp_start_c"] + 10)


class FirmwareCore:
    """A core (Plus/Ultra) on a laptop port, shaped like the firmware's own
    /state and /selftest (hifi_api.c, hifi_main.c op_selftest): the amp
    request answers 200 and PVDD stays off without power-good."""
    def __init__(self, contract=None, vbus=5.02, pvdd_follows=False, model="hifi-plus"):
        self.contract, self.vbus, self.pvdd_follows, self.model = contract, vbus, pvdd_follows, model
        self.enabled = False
        self.calls = []

    def selftest(self):
        hv = self.contract is not None and self.contract["v"] >= 9
        names = {"pd_sink_i2c": True, "pd_contract": hv, "expander_i2c": True, "dac_i2c": True,
                 "adc_i2c": True, "amp_otw": True, "pvdd_on": hv, "amp_no_fault": hv, "psram": True,
                 "heap": True, "network": False}
        if self.model == "hifi-ultra":
            names["front_i2c"] = True
        items = [{"name": n, "pass": ok, "value": None, "limit": None} for n, ok in names.items()]
        return {"pass": all(i["pass"] for i in items), "items": items}

    def state(self):
        c = self.contract or {"protocol": "none", "epr": False, "v": None, "i_max_a": None, "w": None}
        on = self.enabled and self.pvdd_follows
        return {"power": {"contract": c, "vbus_v": self.vbus, "pg": False, "pvdd_on": on},
                "amp": {"enabled": on, "fault": False, "faults": 0, "latched": False, "temp_c": 31.0}}

    def amp(self, on):
        self.calls.append(on)
        self.enabled = on
        return {"ok": True}


class FirmwareShape(unittest.TestCase):
    def test_core_on_5v_passes_stage_1(self):
        for model in ("hifi-plus", "hifi-ultra"):
            dev = FirmwareCore(model=model)
            r = run.selftest(dev, expect_5v=True, expect_offline=True, settle_s=0)
            self.assertTrue(r["pass"], [i for i in r["items"] if not i["pass"] and not i.get("expected")])
            exp = {i["name"] for i in r["items"] if i.get("expected")}
            self.assertEqual(exp, {"pd_contract", "pvdd_on", "amp_no_fault", "network"})
            self.assertEqual(r["source"], "none")
            self.assertEqual(dev.calls, [True, False])  # asked, then left off

    def test_5v_is_seen_without_expect_5v(self):
        r = run.selftest(FirmwareCore(contract={"protocol": "bc", "v": 5.0, "i_max_a": 1.5}),
                         expect_offline=True, settle_s=0)
        self.assertTrue(r["pass"])
        self.assertEqual(r["contract_v"], 5.0)

    def test_amp_that_powers_on_5v_fails(self):
        r = run.selftest(FirmwareCore(pvdd_follows=True), expect_5v=True, expect_offline=True, settle_s=0)
        self.assertFalse(r["pass"])
        self.assertFalse(next(i for i in r["items"] if i["name"] == "bench: amp refused on 5 V")["pass"])

    def test_network_still_counts_after_wifi(self):
        self.assertFalse(run.selftest(FirmwareCore(), expect_5v=True, settle_s=0)["pass"])

    def test_on_a_contract_nothing_is_excused(self):
        dev = FirmwareCore(contract={"protocol": "pd", "epr": False, "v": 20.0, "i_max_a": 5.0, "w": 100},
                           vbus=20.1)
        r = run.selftest(dev, settle_s=0)
        self.assertEqual(dev.calls, [])
        self.assertFalse(any(i.get("expected") for i in r["items"]))

    def test_supply_reads_both_shapes(self):
        fw = run.supply({"power": {"contract": {"protocol": "pd", "v": 20.0, "i_max_a": 5.0}, "vbus_v": 20.0}})
        self.assertEqual((fw["source"], fw["contract_v"], fw["contract_a"], fw["low"]), ("pd", 20.0, 5.0, False))
        old = run.supply({"pd": {"source": "5v", "contract_v": None}})
        self.assertTrue(old["low"])

    def test_controls_item_per_model(self):
        self.assertIn("vol-", run.streaming("hifi-lite")[7])
        self.assertIn("BTN3", run.streaming("hifi-ultra")[7])
        self.assertIn("knob", run.streaming(None, "core")[7])
        self.assertEqual(len(run.streaming("hifi-plus")), len(run.STREAMING))


class Cli(unittest.TestCase):
    def test_bringup_end_to_end(self):
        with tempfile.TemporaryDirectory() as d:
            with redirect_stdout(io.StringIO()):
                code = cli.main(["--mock", "--out", d, "bringup", "--loads", "4,8", "--governor", "off",
                                 "--soak-s", "120", "--wifi", "bench:pw"])
            self.assertEqual(code, 0)
            s = json.loads(Path(d, "session.json").read_text())
            self.assertTrue(s["mock"])
            self.assertEqual(len(s["selftests"]), 3)
            self.assertTrue(all(not v.startswith("fail") for v in s["verdicts"].values()))
            for f in ("report.md", "sweep.csv", "soak.csv", "thd.svg"):
                self.assertTrue(Path(d, f).is_file(), f)
            self.assertIn("MOCK", Path(d, "report.md").read_text())

    def test_bringup_on_core_uses_its_controls(self):
        with tempfile.TemporaryDirectory() as d:
            with redirect_stdout(io.StringIO()):
                code = cli.main(["--mock", "--board", "core", "--mock-charger", "140w", "--out", d, "bringup",
                                 "--loads", "8", "--governor", "off", "--soak-s", "60", "--chargers", "140w",
                                 "--wifi", "bench:pw"])
            s = json.loads(Path(d, "session.json").read_text())
            self.assertEqual(s["verdicts"]["self-test on a laptop port, amp held off"], "pass")
            self.assertEqual(code, 0, s["verdicts"])
            self.assertTrue(any(k.startswith("Controls: knob") for k in s["checklist"]))
            self.assertFalse(any(k.startswith("Buttons: vol-") for k in s["checklist"]))

    def test_bringup_stops_on_a_failed_selftest(self):
        with tempfile.TemporaryDirectory() as d:
            argv = ["--mock", "--out", d, "bringup", "--loads", "4"]
            real = mock.Sim.__init__

            def broken(self, *a, **k):
                real(self, *a, **k)
                self.faults = {"amp_on_5v"}
            mock.Sim.__init__ = broken
            try:
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(cli.main(argv), 1)
            finally:
                mock.Sim.__init__ = real
            s = json.loads(Path(d, "session.json").read_text())
            self.assertEqual(s["sweeps"], [])


if __name__ == "__main__":
    unittest.main()
