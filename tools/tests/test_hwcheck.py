"""The score's own logic: needed functions, the spec. No gates run (test_pieces.py has the pieces)."""
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import hwcheck  # noqa: E402

SPEC = hwcheck.SPEC


def found(board, **kw):
    return hwcheck.board_cost(hwcheck.BOARDS / board, **kw)["found"]


class Needs(unittest.TestCase):
    def check_tier(self, tier, lines):
        return [m for m in (hwcheck.lacks(n, lines) for n in SPEC["tiers"][tier]["needs"]) if m]

    def test_committed_boards_carry_their_tier_functions(self):
        for tier in ("lite", "plus"):
            lines = [x for b in SPEC["tiers"][tier]["builds"]
                     if (hwcheck.BOARDS / b["board"] / "state.json").is_file()
                     for x in found(b["board"], **{k: v for k, v in b.items() if k != "board"})]
            if lines:
                self.assertEqual(self.check_tier(tier, lines), [], tier)

    def test_dropping_a_function_is_caught(self):
        if not (hwcheck.BOARDS / "lite-main" / "state.json").is_file():
            self.skipTest("no lite-main")
        lines = found("lite-main")
        no_amp = [x for x in lines if "TPA3118" not in x["text"]["mpn"]]
        self.assertEqual(len(no_amp), len(lines) - 1)
        self.assertEqual(self.check_tier("lite", no_amp), ["stereo class-D amplifier (0 of 1 parts)"])
        fewer = [dict(x, refs=x["refs"][:2]) if "TS-1187" in x["text"]["mpn"] else x for x in lines]
        self.assertEqual(self.check_tier("lite", fewer), ["3 front buttons (2 of 3 parts)"])

    def test_pads_count_contacts(self):
        need = {"what": "terminals", "match": "KF301", "pads": 4}
        line = {"refs": ["J6"], "pads": 2, "text": {"comment": "", "mpn": "KF301-5.0-2P", "value": "",
                                                    "description": "", "role": ""}}
        self.assertIsNotNone(hwcheck.lacks(need, [line]))
        self.assertIsNone(hwcheck.lacks(need, [line, dict(line, refs=["J7"])]))

    def test_role_only_where_asked(self):
        line = {"refs": ["J2"], "pads": 14, "text": {"comment": "FPC", "mpn": "FPC-05FB-12PH20", "value": "",
                                                     "description": "FPC", "role": "for the LCD tail"}}
        self.assertIsNone(hwcheck.lacks({"what": "screen", "match": "LCD", "in": ["role"]}, [line]))
        self.assertIsNotNone(hwcheck.lacks({"what": "screen", "match": "LCD"}, [line]))


class Test(unittest.TestCase):
    """cmd_test counts each failing gate, not each gated board; what is
    unverified is neither a pass nor a fail."""

    def run_test(self, gates, smoke=(True, None), env=None):
        saved = {k: getattr(hwcheck, k) for k in ("check_product", "run_unittests", "firmware_tests",
                                                    "boards", "run_gates", "cant_run",
                                                    "firmware_pins", "firmware_smoke")}
        dirs = list(gates)
        hwcheck.check_product = lambda: ([], {})
        hwcheck.run_unittests = lambda: []
        hwcheck.firmware_tests = lambda: True
        hwcheck.firmware_pins = lambda: []
        hwcheck.boards = lambda: dirs
        hwcheck.run_gates = lambda todo: {d: gates[d] for d in todo}
        hwcheck.firmware_smoke = lambda: smoke
        self.cant = None

        def cant_run(tool, why):  # the real one execs the shell's not-found
            self.cant = (tool, why)
            raise SystemExit(127)
        hwcheck.cant_run = cant_run
        try:
            with contextlib.redirect_stdout(io.StringIO()), mock.patch.dict(os.environ, env or {}):
                try:
                    return hwcheck.cmd_test(all_boards=True)
                except SystemExit as e:
                    return e.code
        finally:
            for k, v in saved.items():
                setattr(hwcheck, k, v)

    def test_boards_that_pass_pass(self):
        self.assertEqual(self.run_test({Path("a"): [], Path("b"): []}), 0)
        self.assertIsNone(self.cant)

    def test_a_failing_gate_fails(self):
        self.assertEqual(self.run_test({Path("a"): [], Path("b"): [("b dfm", 1)]}), 1)

    def test_an_unverified_gate_cant_run(self):
        u = ("b dfm: placement unverified: U1, U2 (HTTP 403)", hwcheck.UNVERIFIED)
        self.assertEqual(self.run_test({Path("a"): [], Path("b"): [u]}), 127)
        self.assertEqual(self.cant[0], "easyeda-models")
        self.assertIn("U1, U2", self.cant[1][0])

    def test_a_real_failure_beside_an_unverified_one_fails(self):
        u = ("b dfm: placement unverified: U1", hwcheck.UNVERIFIED)
        self.assertEqual(self.run_test({Path("a"): [("a drc_routed", 1)], Path("b"): [u]}), 1)
        self.assertIsNone(self.cant)

    def test_the_firmware_smoke(self):
        a = {Path("a"): []}
        self.assertEqual(self.run_test(a, smoke=(False, None)), 1)
        self.assertEqual(self.run_test(a, smoke=(False, ("squeezelite-fetch", "HTTP 503"))), 127)
        self.assertEqual(self.cant[0], "squeezelite-fetch")

    def test_no_idf_or_qemu_yet_is_no_boot_unless_asked(self):
        a, absent = {Path("a"): []}, (None, ("idf.py", "idf MISSING (fw setup)"))
        self.assertEqual(self.run_test(a, smoke=absent), 0)
        self.assertIsNone(self.cant)
        self.assertEqual(self.run_test(a, smoke=absent, env={"HWCHECK_QEMU": "1"}), 127)
        self.assertEqual(self.cant[0], "idf.py")


class Smoke(unittest.TestCase):
    """firmware_smoke() reads hive-fw's doctor, build and qemu."""

    def smoke(self, doctor="idf       /hive/tools/fw/esp-idf-v4.4.8\nqemu      /q", build=(0, ""), qemu=(0, "ok")):
        def fake(args, **kw):
            out = {"doctor": (0 if "MISSING" not in doctor else 1, doctor), "build": build, "qemu": qemu}[args[1]]
            return subprocess.CompletedProcess(args, out[0], out[1], "")
        with mock.patch.object(hwcheck, "fw_bin", lambda: "fw"), mock.patch.object(subprocess, "run", fake), \
                contextlib.redirect_stdout(io.StringIO()):
            return hwcheck.firmware_smoke()

    def test_boots(self):
        self.assertEqual(self.smoke(), (True, None))
        self.assertEqual(self.smoke(qemu=(1, "fw qemu: no boot line")), (False, None))

    def test_no_fw_idf_or_qemu(self):
        with mock.patch.object(hwcheck, "fw_bin", lambda: None):
            self.assertEqual(hwcheck.firmware_smoke()[0], None)
        self.assertEqual(self.smoke(doctor="idf       MISSING (fw setup)\nqemu      MISSING (fw setup)")[1][0],
                         "idf.py")
        self.assertEqual(self.smoke(doctor="idf       /i\nqemu      MISSING (fw setup)")[1][0],
                         "qemu-system-xtensa")

    def test_a_build_fails_unless_the_fetch_did_on_the_network(self):
        self.assertEqual(self.smoke(build=(1, "error: hifi_main.c:3: expected ';'")), (False, None))
        self.assertEqual(self.smoke(build=(1, "error: patch failed: main/x.c:1\nfw: prepare failed (1)")),
                         (False, None))
        got = self.smoke(build=(128, "fatal: unable to access 'https://github.com/': Could not resolve host"
                                     "\nfw: prepare failed (128)"))
        self.assertEqual(got[1][0], "squeezelite-fetch")


class Pins(unittest.TestCase):
    """The test checks the firmware's pins against its board."""

    def test_lite_s_pins_land_on_lite_main(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(hwcheck.firmware_pins(), [])

    def test_a_moved_pin_fails(self):
        piece = next(p for p in SPEC["pieces"] if p["name"] == "Lite firmware")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for d in ("firmware", "boards/lite-main/kicad"):
                shutil.copytree(hwcheck.ROOT / d, root / d)
            h = root / "firmware/components/hifi/esp/hifi_board_lite.h"
            h.write_text(h.read_text().replace("ADC_PVDD ADC1_CHANNEL_3", "ADC_PVDD ADC1_CHANNEL_0"))
            bad = hwcheck.pieces.CHECKS["firmware"](root, piece)
        self.assertTrue(any("ADC_PVDD: GPIO36" in b for b in bad), bad)


class Unverified(unittest.TestCase):
    """unverified() tells a gate that couldn't fetch part models from one
    that failed or crashed."""

    def test_old_hwde_exit_1_with_only_fetch_failures(self):
        r = {"failing": [{"kind": "cpl_fetch_failed", "refs": ["U2", "U1"], "reason": "HTTP 403"}]}
        self.assertEqual(hwcheck.unverified(1, r, ""), "U1, U2 unverified (HTTP 403)")
        r["failing"].append({"kind": "cpl_rotation", "refs": ["U3"]})
        self.assertIsNone(hwcheck.unverified(1, r, ""))

    def test_exit_2_with_the_block(self):
        r = {"status": "error", "unverified": {"family": "placement", "refs": ["U5"], "reason": "rate limited"}}
        self.assertEqual(hwcheck.unverified(2, r, ""), "placement unverified: U5 (rate limited)")
        self.assertEqual(hwcheck.unverified(2, {"report": r}, ""), "placement unverified: U5 (rate limited)")

    def test_exit_2_with_only_the_gate_s_error_text(self):
        log = ('{"script": "gate", "status": "error", "error": "Traceback ...\\nRuntimeError: dfm report '
               'status is error: placement unverified, not failed: could not fetch the LCSC model for 3 '
               'part(s) (HTTP 403)\\n"}')
        self.assertTrue(hwcheck.unverified(2, {}, log).startswith("placement unverified, not failed: could not"))

    def test_a_crash_or_a_fail_is_not_unverified(self):
        self.assertIsNone(hwcheck.unverified(2, {}, "Traceback: KeyError"))
        self.assertIsNone(hwcheck.unverified(1, {"failing": [{"kind": "clearance"}]}, ""))
        self.assertIsNone(hwcheck.unverified(0, {}, ""))


class Spec(unittest.TestCase):
    def test_spec_is_whole(self):
        self.assertEqual(sorted(SPEC["tiers"]), sorted(hwcheck.TIERS))
        for name, tier in SPEC["tiers"].items():
            self.assertTrue(tier["builds"] and tier["needs"], name)
        # A missing tier outweighs any tier's real cost, so landing one is a gain.
        self.assertGreater(SPEC["tier_not_counted"], 200)


if __name__ == "__main__":
    unittest.main()
