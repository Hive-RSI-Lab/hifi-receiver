"""The score's own logic: needed functions, fit reports, the spec. No gates run."""
import sys
import tempfile
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


class Fit(unittest.TestCase):
    def fit(self, text):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "fit.md"
            p.write_text(text)
            return hwcheck.fit_passes(p)

    def test_fit_report(self):
        head = "| Check | Result | Detail |\n|---|---|---|\n"
        self.assertTrue(self.fit(head + "| a | pass | |\n| b | pass | x |\n"))
        self.assertFalse(self.fit(head + "| a | pass | |\n| b | FAIL | x |\n"))
        self.assertFalse(self.fit(head))


class Spec(unittest.TestCase):
    def test_spec_is_whole(self):
        self.assertEqual(sorted(SPEC["tiers"]), sorted(hwcheck.TIERS))
        for name, tier in SPEC["tiers"].items():
            self.assertTrue(tier["builds"] and tier["needs"], name)
        # A missing tier outweighs any tier's real cost, so landing one is a gain.
        self.assertGreater(SPEC["tier_not_counted"], 200)
        for p in SPEC["pieces"]:
            self.assertIn((p.get("test") or "fit").split()[0], ("firmware", "unittest", "fit"), p["name"])


if __name__ == "__main__":
    unittest.main()
