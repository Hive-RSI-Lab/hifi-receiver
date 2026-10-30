"""Each piece's check passes on the committed piece and fails on a stub of it
in the same place: the score can't be lowered by an empty file."""
import re
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import hwcheck  # noqa: E402
import pieces  # noqa: E402

ROOT = hwcheck.ROOT
PIECE = {p["name"]: p for p in hwcheck.SPEC["pieces"]}


class Tree:
    """A scratch copy of the repo's files a check reads (linked, not copied),
    where a test swaps one for a stub."""

    def __init__(self, *dirs):
        self.root = Path(tempfile.mkdtemp(prefix="pieces-"))
        for d in dirs:
            for f in (ROOT / d).rglob("*") if (ROOT / d).is_dir() else [ROOT / d]:
                if f.is_file() and "__pycache__" not in f.parts:
                    to = self.root / f.relative_to(ROOT)
                    to.parent.mkdir(parents=True, exist_ok=True)
                    to.symlink_to(f)

    def put(self, rel, data):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if p.is_symlink():
            p.unlink()
        (p.write_bytes if isinstance(data, bytes) else p.write_text)(data)

    def drop(self, rel):
        (self.root / rel).unlink()

    def text(self, rel):
        return (self.root / rel).read_text(encoding="utf-8")

    def check(self, name):
        p = PIECE[name]
        return pieces.CHECKS[p["check"]](self.root, p)

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


def board(name):
    return [f"boards/{name}/state.json", f"boards/{name}/kicad/{name}.kicad_pcb"]


def has(*paths):
    return all((ROOT / p).exists() for p in paths)


class Base(unittest.TestCase):
    dirs = ()

    def setUp(self):
        missing = [d for d in self.dirs if not (ROOT / d).exists()]
        if missing:
            self.skipTest(f"no {', '.join(missing)}")
        self.t = Tree(*self.dirs)

    def tearDown(self):
        self.t.close()


class Firmware(Base):
    dirs = ("firmware/board", "firmware/patches", "firmware/components", *board("lite-main"), *board("core"))
    FW = "firmware/board/lite-main/"

    def test_lite_passes_and_checks_its_pins(self):
        self.assertEqual(self.t.check("Lite firmware"), [])
        pins, _ = pieces.pin_map(self.t.root, "lite-main", "lite-main", "HIFI_LITE")
        self.assertGreaterEqual(len(pins), 15)  # I2S, I2C, 3 buttons, IR, LED, power and amp

    def test_an_empty_config_fails(self):
        self.t.put(self.FW + "sdkconfig.defaults", "")
        self.assertTrue(self.t.check("Lite firmware"))

    def test_a_pin_on_the_wrong_net_fails(self):
        for p in (self.t.root / "firmware" / "patches").glob("*.patch"):
            text = p.read_text()
            if "bck=27" in text:
                self.t.put(p.relative_to(self.t.root), text.replace("bck=27", "bck=12"))
        bad = self.t.check("Lite firmware")
        self.assertTrue(any("GPIO12" in b for b in bad), bad)

    def test_plus_passes_and_checks_its_pins(self):
        self.assertEqual(self.t.check("Plus firmware"), [])
        pins, _ = pieces.pin_map(self.t.root, "core", "core", "HIFI_PLUS")
        self.assertGreaterEqual(len(pins), 15)  # I2S, IR, I2C, the expander's INT, ISENSE, RMII, the sub DAC

    def _patch(self, edit):
        for p in (self.t.root / "firmware" / "patches").glob("*.patch"):
            self.t.put(p.relative_to(self.t.root), edit(p.read_text()))

    def test_the_lite_config_copied_for_plus_fails(self):
        lite = re.search(r'default "(model=ES8388[^"]*)" if HIFI_LITE', self.t.text(
            next(p.relative_to(self.t.root) for p in (self.t.root / "firmware/patches").glob("*.patch")))).group(1)
        self._patch(lambda t: re.sub(r'default "model=I2S[^"]*" if HIFI_PLUS', f'default "{lite}" if HIFI_PLUS', t))
        bad = self.t.check("Plus firmware")
        self.assertTrue(any("on boards/core, not a /" in b for b in bad), bad)

    def test_a_board_no_patch_adds_fails(self):
        self._patch(lambda t: "\n".join(l for l in t.split("\n") if "HIFI_PLUS" not in l))
        bad = self.t.check("Plus firmware")
        self.assertIn("no firmware/patches adds the Kconfig board HIFI_PLUS", bad)

    def test_partitions_past_the_flash_fail(self):
        self.t.put(self.FW + "partitions.csv", self.t.text(self.FW + "partitions.csv")
                   + "big, data, spiffs, 0x620000, 0x400000,\n")
        self.assertTrue(any("past 8 MB" in b for b in self.t.check("Lite firmware")))


@unittest.skipUnless(shutil.which("cc"), "no cc")
class App(Base):
    dirs = ("app", "firmware/components")

    def test_the_app_passes(self):
        self.assertEqual(self.t.check("web app"), [])

    def test_a_stub_app_fails(self):
        self.t.put("app/web/index.html", "<!doctype html><html></html>\n")
        self.t.put("app/web/manifest.webmanifest", "{}\n")
        self.t.put("app/web/app.js", "\n")
        bad = self.t.check("web app")
        self.assertIn("index.html loads no script", bad)
        self.assertIn("index.html links no manifest", bad)

    def test_an_app_that_never_reads_the_firmware_fails(self):
        self.t.put("app/web/app.js", "console.log('hi');\n")
        self.assertIn("the app never reads state", self.t.check("web app"))


class Bench(Base):
    dirs = ("bench", "firmware/components")

    def test_the_bench_passes(self):
        self.assertEqual(self.t.check("bench scripts"), [])

    def test_a_stub_bench_fails(self):
        for f in (self.t.root / "bench").rglob("*.py"):
            self.t.put(f.relative_to(self.t.root), "print('pass')\n")
        bad = self.t.check("bench scripts")
        self.assertTrue(any("report" in b for b in bad), bad)
        self.assertIn("the bench never calls selftest", bad)


def stl(tris):
    return b"\0" * 80 + struct.pack("<I", len(tris)) + b"".join(
        struct.pack("<12fH", 0, 0, 0, *[c for v in t for c in v], 0) for t in tris)


class Enclosure(Base):
    dirs = ("enclosure/out/lite", *board("lite-main"))
    OUT = "enclosure/out/lite/"

    def test_the_lite_box_passes(self):
        self.assertEqual(self.t.check("Lite enclosure"), [])

    def test_a_stub_box_fails(self):
        for f in (self.t.root / "enclosure" / "out" / "lite").iterdir():
            f.unlink()
        self.t.put(self.OUT + "fit.md", "| check | result |\n|---|---|\n| fit | pass |\n")
        self.t.put(self.OUT + "base.stl", stl([((0, 0, 0), (1, 0, 0), (0, 1, 0))]))
        self.t.put(self.OUT + "base.step", "")
        bad = self.t.check("Lite enclosure")
        self.assertTrue(any("mounting hole" in b for b in bad), bad)
        self.assertTrue(any("closed mesh" in b for b in bad), bad)
        self.assertTrue(any("STEP" in b for b in bad), bad)

    def test_an_open_mesh_fails(self):
        f = self.OUT + "base.stl"
        tris = pieces.stl_triangles(self.t.root / f)
        self.t.put(f, stl(tris[1:]))
        self.assertIn(f"{f} isn't a closed mesh", self.t.check("Lite enclosure"))

    def test_a_failing_fit_fails(self):
        self.t.put(self.OUT + "fit.md", self.t.text(self.OUT + "fit.md").replace("| pass |", "| fail |", 1))
        self.assertTrue(self.t.check("Lite enclosure"))

    def test_a_small_box_fails(self):
        cube = [((0, 0, 0), (1, 1, 0), (1, 0, 0)), ((0, 0, 0), (0, 1, 0), (1, 1, 0)),
                ((0, 0, 1), (1, 0, 1), (1, 1, 1)), ((0, 0, 1), (1, 1, 1), (0, 1, 1)),
                ((0, 0, 0), (1, 0, 0), (1, 0, 1)), ((0, 0, 0), (1, 0, 1), (0, 0, 1)),
                ((0, 1, 0), (1, 1, 1), (1, 1, 0)), ((0, 1, 0), (0, 1, 1), (1, 1, 1)),
                ((0, 0, 0), (0, 0, 1), (0, 1, 1)), ((0, 0, 0), (0, 1, 1), (0, 1, 0)),
                ((1, 0, 0), (1, 1, 1), (1, 0, 1)), ((1, 0, 0), (1, 1, 0), (1, 1, 1))]
        self.assertTrue(pieces.closed([tuple(map(lambda v: tuple(map(float, v)), t)) for t in cube]))
        for f in (self.t.root / "enclosure" / "out" / "lite").glob("*.stl"):
            self.t.put(f.relative_to(self.t.root), stl(cube))
        self.assertTrue(any("can't hold" in b for b in self.t.check("Lite enclosure")))


class Manual(Base):
    dirs = ("docs/manual", "docs/product.md", "docs/app.md", "docs/bench.md", "docs/firmware.md",
            "docs/test-api.md", "enclosure/README.md", "boards/lite-main/README.md", "boards/core/README.md",
            "firmware/board", "firmware/patches", "firmware/upstream.lock", "firmware/components",
            "app/web/app.js", "bench/README.md")

    def test_the_manual_passes(self):
        self.assertEqual(self.t.check("user manual and quick start"), [])

    def test_a_stub_manual_fails(self):
        self.t.put("docs/manual/user-manual.md", "# Manual\n")
        self.t.put("docs/manual/quick-start.md", "# Quick start\n")
        self.t.put("docs/manual/sources.md", "# Sources\n")
        bad = self.t.check("user manual and quick start")
        self.assertIn("docs/manual/sources.md has no fact taken from a source", bad)
        self.assertTrue(any("no numbered sections" in b for b in bad), bad)

    def test_a_section_with_no_source_fails(self):
        self.t.put("docs/manual/user-manual.md", self.t.text("docs/manual/user-manual.md")
                   + "\n## 99. Something new\n\nUntraced.\n")
        self.assertIn("user-manual.md's section 99 traces to no fact in sources.md (§ 99)",
                      self.t.check("user manual and quick start"))

    def test_a_source_that_went_away_fails(self):
        self.t.drop("enclosure/README.md")
        self.assertIn("docs/manual/sources.md names enclosure/README.md, which isn't in the repo",
                      self.t.check("user manual and quick start"))


class Spec(unittest.TestCase):
    def test_every_piece_has_a_check(self):
        for p in hwcheck.SPEC["pieces"]:
            self.assertIn(p["check"], pieces.CHECKS, p["name"])
            for t in p.get("tests", []):
                self.assertIn(t.split()[0], ("firmware", "unittest"), p["name"])


if __name__ == "__main__":
    unittest.main()
