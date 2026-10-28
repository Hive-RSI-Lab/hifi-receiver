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
