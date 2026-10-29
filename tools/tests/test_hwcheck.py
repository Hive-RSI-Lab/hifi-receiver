"""The score's own logic: needed functions, the spec. No gates run (test_pieces.py has the pieces)."""
import contextlib
import io
import sys
import unittest
from pathlib import Path

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
    """cmd_test counts each failing gate, not each gated board."""

    def run_test(self, gates):
        saved = {k: getattr(hwcheck, k) for k in ("check_product", "run_unittests", "firmware_tests",
                                                    "boards", "run_gates")}
        dirs = list(gates)
        hwcheck.check_product = lambda: ([], {})
        hwcheck.run_unittests = lambda: []
        hwcheck.firmware_tests = lambda: True
        hwcheck.boards = lambda: dirs
        hwcheck.run_gates = lambda todo: {d: gates[d] for d in todo}
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                return hwcheck.cmd_test(all_boards=True)
        finally:
            for k, v in saved.items():
                setattr(hwcheck, k, v)

    def test_boards_that_pass_pass(self):
        self.assertEqual(self.run_test({Path("a"): [], Path("b"): []}), 0)

    def test_a_failing_gate_fails(self):
        self.assertEqual(self.run_test({Path("a"): [], Path("b"): [("b dfm", 1)]}), 1)


class Spec(unittest.TestCase):
    def test_spec_is_whole(self):
        self.assertEqual(sorted(SPEC["tiers"]), sorted(hwcheck.TIERS))
        for name, tier in SPEC["tiers"].items():
            self.assertTrue(tier["builds"] and tier["needs"], name)
        # A missing tier outweighs any tier's real cost, so landing one is a gain.
        self.assertGreater(SPEC["tier_not_counted"], 200)


if __name__ == "__main__":
    unittest.main()
