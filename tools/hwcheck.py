#!/usr/bin/env python3
"""The project's test and score.

    python3 tools/hwcheck.py test [--all]
                                     the check: docs/product.md, the software's
                                     unit tests, the firmware's host tests, the
                                     firmware's QEMU smoke (hive-fw's fw build
                                     and fw qemu: it boots and answers GET
                                     /api/v1/info) when firmware/ changed, each
                                     tier's firmware pins against its board's
                                     ESP32 pads (tools/pieces.py), then
                                     the hwde gates of each board whose files
                                     changed since the merge base with main
                                     (every board with --all or HWCHECK_ALL=1)
    python3 tools/hwcheck.py score   the whole product, lower is better: each
                                     tier's unit cost at 100 in USD from its
                                     boards' BOMs, counted only when its boards
                                     pass their gates and carry every function
                                     tools/product.json names (else a flat
                                     1000), plus 100 for each piece (firmware,
                                     app, bench, enclosures, manual) whose check
                                     (tools/pieces.py) doesn't pass
    python3 tools/hwcheck.py cost [BOARD ...]   the cost alone, no gates

docs/checks.md says what each part checks and how the score is worked out.

Gates run through hive-hwde's bin/hwde, found as $HWDE_BIN,
${HIVE_TOOLS:-/hive/tools}/hwde/bin/hwde, `hwde` on PATH, or bin/hwde in the
installed module ($HIVE_MODULES, else /hive/modules). Each
board runs in a scratch copy of its workspace, so the checkout stays as it is.
With boards to check and no hwde, the command execs `sh -c hwde` so the
shell's own "hwde: not found" and exit 127 say it couldn't run: that is no
verdict on the commit, not a red. The same goes for make and cc when the
firmware's host tests are there to run.

What ran but couldn't be verified is neither a pass nor a fail: a gate that
couldn't fetch part models (hwde's dfm `unverified`: EasyEDA's rate limit,
the network) and a QEMU smoke with no fw, IDF or QEMU here. With nothing
failing, the test then exits 127 too, naming what is unverified; any real
failure still makes it exit 1. That 127 comes from the shell's own "not
found" for a tool named for what is missing (easyeda-models, idf.py, fw),
the only 127 the Hive reads as no verdict rather than a red.
"""
from __future__ import annotations

import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True  # no tools/__pycache__ in the checkout
sys.path.insert(0, str(ROOT / "tools"))
import bom_estimate as est  # noqa: E402  (one cost model for estimate and board)
import pieces  # noqa: E402  (what lets each piece count)
from pieces import need_tool  # noqa: E402

PRODUCT = ROOT / "docs" / "product.md"
# What the score counts: each tier's boards and needed functions, and the
# pieces beside them (docs/checks.md). An evaluator file, like this one.
SPEC = json.loads((ROOT / "tools" / "product.json").read_text(encoding="utf-8"))
BOARDS = ROOT / "boards"
TIERS = ("lite", "plus", "ultra")
GATES = ("erc", "drc_routed", "verify", "dfm")
# Gate findings that are hwde failing to fetch, not the board failing.
UNRUN = ("cpl_fetch_failed",)
# A gate's code for "ran, but couldn't verify" (gate_board): the score has no
# number then, and the test exits as couldn't-run.
UNVERIFIED = 127
# Software whose stdlib unittest suite (<dir>/tests/test_*.py) the test runs.
SOFTWARE = ("tools", "bench", "app")
QTY = SPEC["qty"]


# ---- docs/product.md ------------------------------------------------------

def product_table(text: str) -> dict[str, list[str]]:
    """The tier table's rows by their first cell, lower-cased, each with its
    Lite, Plus and Ultra cells (the table whose header names the three)."""
    rows, inside = {}, False
    for line in text.splitlines():
        if not line.startswith("|"):
            if inside:
                break
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not inside:
            inside = [(re.findall(r"[a-z]+", c.lower()) or [""])[0] for c in cells[1:4]] == list(TIERS)
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        rows[cells[0].lower()] = (cells[1:] + ["", "", ""])[:3]
    return rows


def row(rows: dict[str, list[str]], start: str) -> list[str] | None:
    return next((v for k, v in rows.items() if k.startswith(start)), None)


def dollars(cell: str) -> list[float]:
    return [float(x) for x in re.findall(r"\$\s*([0-9]+(?:\.[0-9]+)?)", cell)]


def check_product() -> tuple[list[str], dict]:
    """Problems with docs/product.md, and what the score needs from it: the
    first tier, its boards and its target retail price."""
    if not PRODUCT.is_file():
        return ["docs/product.md is missing"], {}
    rows = product_table(PRODUCT.read_text(encoding="utf-8"))
    if not rows:
        return ["docs/product.md has no tier table (a header row naming Lite, Plus and Ultra)"], {}
    bad, info = [], {}
    target, total = row(rows, "target retail"), row(rows, "unit cost at 100")
    boards, order = row(rows, "boards"), row(rows, "build order")
    for name, r in (("Target retail", target), ("Unit cost at 100", total),
                    ("Boards", boards), ("Build order", order)):
        if r is None:
            bad.append(f"docs/product.md: the tier table has no '{name}' row")
    for i, tier in enumerate(TIERS):
        if target is not None and not dollars(target[i]):
            bad.append(f"docs/product.md: {tier} has no target price")
        if total is not None:
            got = dollars(total[i])
            if not got:
                bad.append(f"docs/product.md: {tier} has no BOM total")
            else:
                # The page's figure is bom_estimate.py's, rounded to the cent
                # or the dime: a page that drifts from the CSV is stale.
                want = estimate(tier)
                if abs(got[0] - want) > 0.05 + 1e-9:
                    bad.append(f"docs/product.md: {tier}'s unit cost at 100 says ${got[0]:g}, "
                               f"but tools/bom_estimate.py --unit {tier} 100 gives ${want:.2f}")
        if boards is not None:
            named = re.findall(r"`([^`]+)`", boards[i])
            counted = [b["board"] for b in SPEC["tiers"][tier]["builds"]]
            if not named:
                bad.append(f"docs/product.md: {tier} names no board (`name`) in the Boards row")
            elif sorted(set(named)) != sorted(set(counted)):
                bad.append(f"docs/product.md: {tier}'s Boards row names {', '.join(named)}, but the "
                           f"score counts {', '.join(counted)} (tools/product.json)")
    if order is not None:
        first = [t for t, c in zip(TIERS, order) if "first" in c.lower()]
        if len(first) != 1:
            bad.append(f"docs/product.md: the Build order row marks {len(first)} tiers 'first', not one")
        elif boards is not None and target is not None:
            i = TIERS.index(first[0])
            info = {"tier": first[0], "boards": re.findall(r"`([^`]+)`", boards[i]),
                    "target": (dollars(target[i]) or [None])[0]}
    return bad, info


def estimate(tier: str) -> float:
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "bom_estimate.py"),
                          "--unit", tier, str(QTY)], capture_output=True, text=True, check=True)
    return float(out.stdout.split()[-1])


# ---- gates ------------------------------------------------------------------

def boards() -> list[Path]:
    return sorted(p.parent for p in BOARDS.glob("*/state.json"))


def hwde() -> str:
    """bin/hwde, or exec the shell's own not-found (exit 127).

    Looks, in order: $HWDE_BIN; ${HIVE_TOOLS:-/hive/tools}/hwde/bin/hwde (the
    path a check sandbox has, per hive-hwde's README); `hwde` on PATH; then
    bin/hwde in the installed module ($HIVE_MODULES, else /hive/modules).
    """
    tools = Path(os.environ.get("HIVE_TOOLS") or "/hive/tools")
    mods = Path(os.environ.get("HIVE_MODULES") or "/hive/modules")
    tried = []
    env = os.environ.get("HWDE_BIN")
    tried.append(f"$HWDE_BIN ({env or 'unset'})")
    if env:
        return env
    for p in [tools / "hwde" / "bin" / "hwde"]:
        tried.append(str(p))
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)
    tried.append("hwde on PATH")
    on_path = shutil.which("hwde")
    if on_path:
        return on_path
    for p in (mods / "hive-hwde" / "bin" / "hwde", mods / "hwde" / "bin" / "hwde"):
        tried.append(str(p))
        if p.is_file():
            return str(p)
    print("hwde not found; looked at: " + ", ".join(tried))
    sys.stdout.flush()
    os.execvp("sh", ["sh", "-c", "hwde"])


def unverified(rc: int, r: dict, log: str) -> str | None:
    """What a gate run left unverified, or None: the parts whose models hwde
    couldn't fetch. hwde's dfm says so as exit 2 with an `unverified` block
    (refs, reason), which the gate may hand on in its result or only in its
    error text; an older hwde as exit 1 whose only findings are UNRUN."""
    if rc == 1:
        failing = r.get("failing") or []
        if failing and all(v.get("kind") in UNRUN for v in failing):
            refs = sorted({x for v in failing for x in v.get("refs") or []})
            why = next((v.get("reason") for v in failing if v.get("reason")), "")
            return f"{', '.join(refs) or 'parts'} unverified" + (f" ({why})" if why else "")
        return None
    if rc < 2:
        return None
    for b in (r.get("unverified"), (r.get("report") or {}).get("unverified")):
        if isinstance(b, dict):
            refs = b.get("refs") or []
            return (f"{b.get('family') or 'some'} unverified: {', '.join(refs) or 'parts'}"
                    + (f" ({b['reason']})" if b.get("reason") else ""))
    m = re.search(r"(\w+ unverified, not failed: [^\n\"\\]*)", log)
    return m.group(1)[:240] if m else None


def gate_board(ws: Path, name: str, bin_: str, logs: Path) -> list[tuple[str, int]]:
    """Each gate on one board's scratch workspace; the failures, explained,
    each with the gate's exit: 1 is a verdict, 2 or more couldn't run, and
    UNVERIFIED ran but couldn't verify (unverified())."""
    bad = []
    for g in GATES:
        inp = ws / "kicad" / f"{name}.{'kicad_sch' if g == 'erc' else 'kicad_pcb'}"
        if not inp.is_file():
            bad.append((f"{name} {g}: no {inp.relative_to(ws)}", 1))
            print(f"  {g}: FAIL (no {inp.relative_to(ws)})", flush=True)
            continue
        out, log = logs / f"{name}-{g}.json", logs / f"{name}-{g}.log"
        with log.open("w") as f:
            rc = subprocess.run([bin_, "gate", "--gate", g, str(inp.relative_to(ws)),
                                 "--workspace", ".", "--out", str(out)], cwd=ws,
                                stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                env=dict(os.environ, HWDE_BOARDS_ROOT=str(ws.parent))).returncode
        if rc == 0:
            print(f"  {g}: pass", flush=True)
            continue
        why, r = "", {}
        try:
            r = json.loads(out.read_text())
            failing = r.get("failing", [])
            why = "; ".join(f"{v.get('check') or v.get('kind', '?')}: {v.get('msg') or v.get('message', '')}"[:160]
                            for v in failing[:6])
        except (OSError, ValueError, AttributeError):
            r = r if isinstance(r, dict) else {}
        text = log.read_text(errors="replace")
        # A part model EasyEDA wouldn't serve (a rate limit, the network)
        # says nothing of the board: it is unverified, not failed.
        unv = unverified(rc, r, text)
        if unv:
            bad.append((f"{name} {g}: {unv}", UNVERIFIED))
            print(f"  {g}: UNVERIFIED (exit {rc}) {unv}", flush=True)
            continue
        if not why:
            why = " | ".join(text.strip().splitlines()[-6:])
        bad.append((f"{name} {g}" + (f" (couldn't run, exit {rc})" if rc > 1 else ""), rc))
        print(f"  {g}: FAIL (exit {rc}) {why}", flush=True)
    return bad


def run_gates(dirs: list[Path]) -> dict[Path, list[tuple[str, int]]]:
    """Each board's gate failures (see gate_board), by its folder."""
    bin_ = hwde()
    scratch = Path(tempfile.mkdtemp(prefix="hwcheck-"))
    try:
        (scratch / "logs").mkdir()
        bad = {}
        for d in dirs:
            name = board_name(d)
            ws = scratch / "boards" / d.name
            shutil.copytree(d, ws, ignore=shutil.ignore_patterns("__pycache__"))
            print(f"{d.name}: gates {', '.join(GATES)}", flush=True)
            bad[d] = gate_board(ws, name, bin_, scratch / "logs")
        return bad
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# What always re-gates every board when it changes: the gate runner itself.
GATE_ALL = ("tools/hwcheck.py",)


def git(*args: str) -> str | None:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def changed_files() -> tuple[set[str] | None, str]:
    """The files that differ from the merge base with main (commits,
    uncommitted edits and new files alike), and against what; None and why
    when git can't say."""
    bases = [os.environ["HWCHECK_BASE"]] if os.environ.get("HWCHECK_BASE") else ["origin/main", "main"]
    ref = next((b for b in bases if git("rev-parse", "-q", "--verify", b + "^{commit}")), None)
    base = ref and git("merge-base", ref, "HEAD")
    if not base:
        return None, f"no merge base with {' or '.join(bases)}"
    tracked = git("diff", "--name-only", "--no-renames", "-z", base.strip())
    untracked = git("ls-files", "--others", "--exclude-standard", "-z")
    if tracked is None or untracked is None:
        return None, "git diff failed"
    return {f for f in (tracked + untracked).split("\0") if f}, f"changed since {ref} ({base.strip()[:9]})"


def changed_boards(dirs: list[Path]) -> tuple[list[Path], str]:
    """The boards whose files differ from the merge base with main, and why.
    Every board when git can't say, when the gate runner changed, or with
    HWCHECK_ALL=1."""
    if os.environ.get("HWCHECK_ALL"):
        return dirs, "HWCHECK_ALL is set"
    changed, since = changed_files()
    if changed is None:
        return dirs, since
    if hit := sorted(changed & set(GATE_ALL)):
        return dirs, f"{', '.join(hit)} changed"
    top = ROOT.resolve()
    rels = {d: d.resolve().relative_to(top).as_posix() + "/" for d in dirs}
    return [d for d in dirs if any(f.startswith(rels[d]) for f in changed)], since


def board_name(d: Path) -> str:
    try:
        return json.loads((d / "state.json").read_text()).get("board") or d.name
    except (OSError, ValueError):
        return d.name


# ---- cost -------------------------------------------------------------------

def price_at(breaks: list[dict], qty: float) -> float:
    """LCSC's unit price for an order of qty: the highest break at or below it."""
    bs = sorted((b["qty"], b["price"]) for b in breaks)
    got = [p for q, p in bs if q <= qty]
    return got[-1] if got else bs[0][1]


def pcb_facts(pcb: Path) -> tuple[dict[str, tuple[int, int]], int, float, float]:
    """Pads per footprint reference (SMT, through-hole), copper layers, and the
    Edge.Cuts outline's width and height in mm."""
    text = pcb.read_text(encoding="utf-8")
    pads = {}
    for fp in re.split(r"\n\t\(footprint ", text)[1:]:
        fp = re.split(r"\n\t\(", fp, maxsplit=1)[0]
        ref = re.search(r'\(property "Reference" "([^"]+)"', fp)
        if ref:
            pads[ref.group(1)] = (len(re.findall(r'\(pad "[^"]*" smd\b', fp)),
                                  len(re.findall(r'\(pad "[^"]*" thru_hole\b', fp)))
    m = re.search(r"\n\t\(layers\n(.*?)\n\t\)", text, re.S)
    layers = len(re.findall(r'"[^"]*\.Cu" (?:signal|power|mixed|jumper)', m.group(1))) if m else 0
    xs, ys = [], []
    for item in re.split(r"\n\t\(gr_", text)[1:]:
        item = re.split(r"\n\t\(", item, maxsplit=1)[0]
        if '(layer "Edge.Cuts")' not in item:
            continue
        for x, y in re.findall(r"\((?:start|end|mid|center|xy) (-?[\d.]+) (-?[\d.]+)\)", item):
            xs.append(float(x))
            ys.append(float(y))
    if not xs:
        raise ValueError(f"{pcb}: no Edge.Cuts outline")
    return pads, layers, max(xs) - min(xs), max(ys) - min(ys)


def board_cost(d: Path, qty: int = QTY, bom: str = "fab/BOM.csv",
               parts: str = "parts/parts.json") -> dict:
    """One board's cost per unit at qty boards, from its BOM (fab/BOM.csv, or
    a population's) priced from its parts.json, with tools/bom_estimate.py's
    assembly and PCB model. Its "found" are the BOM's lines as the function
    check reads them: refs, pads, and the text each field holds."""
    name = board_name(d)
    bom, parts = d / bom, d / parts
    for f in (bom, parts):
        if not f.is_file():
            raise ValueError(f"{d.name}: no {f.relative_to(d)}")
    by_lcsc = {p["lcsc"]: p for p in json.loads(parts.read_text())["parts"] if p.get("lcsc")}
    pads, layers, w, h = pcb_facts(d / "kicad" / f"{name}.kicad_pcb")
    if layers not in est.PCB_BASE:
        raise ValueError(f"{d.name}: {layers} copper layers; tools/bom_estimate.py prices "
                         f"{', '.join(map(str, est.PCB_BASE))}")
    part_cost = smt = tht = 0.0
    extended, lines, found = set(), 0, []
    with bom.open(encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        refs = [x.strip() for x in r["Designator"].split(",") if x.strip()]
        lcsc = (r.get("LCSC Part #") or r.get("LCSC") or "").strip()
        p = by_lcsc.get(lcsc)
        if not p or not (p.get("price_breaks") or p.get("price") is not None):
            raise ValueError(f"{d.name}: BOM line {r['Designator']} ({lcsc or 'no LCSC'}) "
                             f"has no price in parts/parts.json")
        breaks = p.get("price_breaks") or [{"qty": 1, "price": p["price"]}]
        part_cost += len(refs) * price_at(breaks, len(refs) * qty)
        if not p.get("basic"):
            extended.add(lcsc)
        for ref in refs:
            if ref not in pads:
                raise ValueError(f"{d.name}: {ref} is on the BOM but not on the board")
            smt += pads[ref][0]
            tht += pads[ref][1]
        lines += 1
        found.append({"refs": refs, "pads": sum(sum(pads[x]) for x in refs),
                      "text": {"comment": r.get("Comment") or "", **{k: str(p.get(k) or "") for k in
                                                                    ("mpn", "value", "description", "role")}}})
    asm = est.SETUP + est.STENCIL + len(extended) * est.FEEDER + qty * (smt * est.SMT_JOINT + tht * est.THT_JOINT)
    pcb = est.pcb(layers, w, h, qty) * est.PCB_CAL
    return {"board": d.name, "lines": lines, "extended": len(extended), "smt": smt, "tht": tht,
            "layers": layers, "w": w, "h": h, "parts": part_cost, "assembly": asm / qty,
            "pcb": pcb / qty, "unit": part_cost + (asm + pcb) / qty, "found": found}


def say_cost(c: dict) -> None:
    print(f"{c['board']}: {c['lines']} BOM lines, {c['extended']} extended, "
          f"{c['smt']:.0f} SMT / {c['tht']:.0f} THT joints, {c['layers']}-layer "
          f"{c['w']:.1f} x {c['h']:.1f} mm; at {QTY}: parts {c['parts']:.2f} + assembly "
          f"{c['assembly']:.2f} + PCB {c['pcb']:.2f} = {c['unit']:.2f} USD per board")


# ---- commands -----------------------------------------------------------------

def unittest_dir(tests: Path) -> bool:
    """A stdlib unittest suite (tests/test_*.py), run from the repo root."""
    r = subprocess.run([sys.executable, "-B", "-m", "unittest", "discover", "-s", str(tests), "-t", str(tests)],
                       cwd=ROOT, capture_output=True, text=True)
    last = (r.stderr.strip().splitlines() or ["?"])[-1]
    if r.returncode:
        print(r.stderr[-4000:])
    print(f"{tests.relative_to(ROOT)}: {last}", flush=True)
    return r.returncode == 0 and not last.startswith("NO TESTS")


def firmware_tests() -> bool:
    """firmware/test: the firmware's host unit tests (make and cc, no ESP-IDF),
    built outside the checkout."""
    tests = ROOT / "firmware" / "test"
    need_tool("make")
    need_tool(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run(["make", "-s", "-C", str(tests), f"BIN={tmp}"], capture_output=True, text=True)
    print((r.stdout + r.stderr).strip()[-4000:])
    print(f"firmware/test: {'pass' if r.returncode == 0 else f'FAIL (exit {r.returncode})'}", flush=True)
    return r.returncode == 0


def fw_bin() -> str | None:
    """hive-fw's bin/fw: $FW_BIN, `fw` on PATH, or the installed module's
    ($HIVE_MODULES, else /hive/modules)."""
    mods = Path(os.environ.get("HIVE_MODULES") or "/hive/modules")
    for c in (os.environ.get("FW_BIN"), shutil.which("fw"), mods / "hive-fw" / "bin" / "fw", mods / "fw" / "bin" / "fw"):
        if c and Path(c).is_file():
            return str(c)
    return None


def cant_run(tool: str, why: list[str]) -> None:
    """Exit 127 as the shell's own "<tool>: not found", after saying why: the
    Hive reads only that as couldn't-run (core.MissingTool), never a red."""
    print("couldn't run, unverified: " + "; ".join(why))
    sys.stdout.flush()
    os.execvp("sh", ["sh", "-c", tool])


def firmware_smoke() -> tuple[bool, tuple[str, str] | None]:
    """firmware/ built for QEMU and booted there by hive-fw (fw build --qemu,
    fw qemu): the boot line, then firmware/fw.json's qemu.http probes over
    QEMU's Ethernet (GET /api/v1/info first). (passed, None), or (False,
    (the missing tool, why) it couldn't run): no fw, or no IDF or QEMU for it
    (fw doctor)."""
    fw = fw_bin()
    if not fw:
        return False, ("fw", "no hive-fw (looked at $FW_BIN, fw on PATH, $HIVE_MODULES/hive-fw/bin/fw)")
    proj = str(ROOT / "firmware")

    def run(*args: str, timeout: int) -> tuple[int, str]:
        try:
            r = subprocess.run([fw, *args], capture_output=True, text=True, timeout=timeout,
                               stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return -1, f"fw {args[0]}: no answer in {timeout} s"
        return r.returncode, (r.stdout + r.stderr).strip()

    rc, out = run("doctor", proj, timeout=120)
    missing = [ln.strip() for ln in out.splitlines() if "MISSING" in ln]
    if rc or missing:
        tool = "qemu-system-xtensa" if missing and all(m.startswith("qemu") for m in missing) else "idf.py"
        return False, (tool, "fw can't build or boot it here: " + ("; ".join(missing) or out[-300:]))
    rc, out = run("build", proj, "--qemu", timeout=3600)
    if rc:
        print(out[-4000:])
        print(f"firmware (QEMU): FAIL (fw build --qemu exit {rc})", flush=True)
        return False, None
    rc, out = run("qemu", proj, timeout=900)
    print(out[-2000:])
    if rc not in (0, 1):
        return False, ("qemu-system-xtensa", f"fw qemu exit {rc}: {out.splitlines()[-1] if out else ''}"[:300])
    print(f"firmware (QEMU): {'pass' if rc == 0 else 'FAIL'}", flush=True)
    return rc == 0, None


def firmware_pins() -> list[str]:
    """Each tier's firmware that exists (firmware/board/<fw>) against its
    board: every pin it gives a job lands on its board's ESP32 on a net named
    for that job, its partitions fit (the firmware piece's check). A tier
    with no firmware yet is the score's to count, not a test failure."""
    bad = []
    for piece in SPEC["pieces"]:
        if piece["check"] != "firmware" or not (ROOT / "firmware" / "board" / piece["fw"]).is_dir():
            continue
        got = pieces.CHECKS["firmware"](ROOT, piece)
        for b in got:
            print(f"FAIL {piece['name']}: {b}")
        if not got:
            print(f"{piece['name']}: its pins land on {piece['board']}'s ESP32, on nets named for them", flush=True)
        bad += [f"{piece['name']}: {b}" for b in got]
    return bad


def run_unittests() -> list[str]:
    return [f"{d}/tests" for d in SOFTWARE if (ROOT / d / "tests").is_dir() and not unittest_dir(ROOT / d / "tests")]


def cmd_test(all_boards: bool = False) -> int:
    bad, _ = check_product()
    for b in bad:
        print(f"FAIL {b}")
    if not bad:
        print("docs/product.md: every tier has its target price and BOM total; one tier is first")
    bad += run_unittests()
    if (ROOT / "firmware" / "test" / "Makefile").is_file() and not firmware_tests():
        bad.append("firmware/test")
    bad += firmware_pins()
    unv = []
    changed, _ = changed_files()
    if (ROOT / "firmware" / "fw.json").is_file() and (
            all_boards or os.environ.get("HWCHECK_ALL") or changed is None
            or any(f.startswith("firmware/") for f in changed)):
        ok, why = firmware_smoke()
        if why:
            print(f"firmware (QEMU): couldn't run: {why[1]}", flush=True)
            unv.append((why[0], f"firmware QEMU smoke: {why[1]}"))
        elif not ok:
            bad.append("firmware (QEMU)")
    else:
        print("firmware (QEMU): firmware/ unchanged, not booted", flush=True)
    dirs = boards()
    if not dirs:
        print("boards/: no boards yet, no gates to run")
    else:
        todo, why = (dirs, "--all") if all_boards else changed_boards(dirs)
        skip = [d.name for d in dirs if d not in todo]
        print(f"boards/: gating {', '.join(d.name for d in todo) or 'none'} ({why})"
              + (f"; {', '.join(skip)} unchanged, not re-gated" if skip else ""), flush=True)
        if todo:
            fails = [x for fs in run_gates(todo).values() for x in fs]
            bad += [f for f, rc in fails if rc != UNVERIFIED]
            unv += [("easyeda-models", f) for f, rc in fails if rc == UNVERIFIED]
        dirs = todo
    if bad:
        print(f"test: FAIL ({len(bad)}): " + "; ".join(bad[:8])
              + (f"; and unverified: {'; '.join(w for _, w in unv)}" if unv else ""))
        return 1
    if unv:
        # neither a pass nor a fail
        cant_run(unv[0][0], [w for _, w in unv])
    print(f"test: pass ({len(dirs)} board(s) gated)")
    return 0


# ---- score --------------------------------------------------------------------

def lacks(need: dict, found: list[dict]) -> str | None:
    """What a tier's boards lack of one needed function, or None: the BOM
    lines whose text matches, counted by refs (or by pads, for contacts)."""
    rx = re.compile(need["match"], re.I)
    fields = need.get("in") or ["comment", "mpn", "value", "description"]
    hits = [x for x in found if any(rx.search(x["text"][f]) for f in fields)]
    got = sum(x["pads"] if "pads" in need else len(x["refs"]) for x in hits)
    want = need.get("pads") or need.get("count", 1)
    return None if got >= want else f"{need['what']} ({got} of {want} {'pads' if 'pads' in need else 'parts'})"


def tier_cost(tier: dict, gates: dict[Path, list]) -> tuple[float | None, list[str]]:
    """A tier's unit cost from its boards, or None and why it doesn't count."""
    costs, why = [], []
    for b in tier["builds"]:
        d = BOARDS / b["board"]
        if d not in gates:
            why.append(f"no boards/{b['board']}")
        elif gates[d]:
            why.append(f"{b['board']} fails {', '.join(f for f, _ in gates[d])}")
        else:
            try:
                costs.append(board_cost(d, bom=b.get("bom", "fab/BOM.csv"),
                                        parts=b.get("parts", "parts/parts.json")))
            except (ValueError, KeyError, OSError) as e:
                why.append(str(e))
    if why:
        return None, why
    for c in costs:
        say_cost(c)
    found = [x for c in costs for x in c["found"]]
    why = [f"lacks {m}" for m in (lacks(n, found) for n in tier["needs"]) if m]
    return (None, why) if why else (sum(c["unit"] for c in costs), [])


def piece_missing(piece: dict, ran: dict) -> str | None:
    """Why a piece doesn't count yet, or None: its check (tools/pieces.py)
    finds a problem, or a suite it needs fails. Each suite runs once per score."""
    bad = pieces.CHECKS[piece["check"]](ROOT, piece)
    if bad:
        return "; ".join(bad[:4]) + (f"; and {len(bad) - 4} more" if len(bad) > 4 else "")
    for test in piece.get("tests", []):
        if test not in ran:
            kind, _, arg = test.partition(" ")
            ran[test] = {"firmware": lambda: firmware_tests(),
                         "unittest": lambda: unittest_dir(ROOT / arg)}[kind]()
        if not ran[test]:
            return f"{test}: fails"
    return None


def cmd_score() -> int:
    tiers = SPEC["tiers"]
    want = sorted({b["board"] for t in tiers.values() for b in t["builds"]})
    have = [BOARDS / b for b in want if (BOARDS / b / "state.json").is_file()]
    gates = run_gates(have) if have else {}
    broken = [f for fs in gates.values() for f, rc in fs if rc > 1]
    if broken:
        print(f"score: no number: gates couldn't run ({'; '.join(broken)})")
        return 1
    total, lines = 0.0, []
    for name, tier in tiers.items():
        unit, why = tier_cost(tier, gates)
        if unit is None:
            unit = SPEC["tier_not_counted"]
            lines.append(f"{name}: {unit:.2f} (not counted: {'; '.join(why)})")
        else:
            lines.append(f"{name}: {unit:.2f} USD per unit at {QTY}, every needed function on the board")
        total += unit
    ran = {}
    for piece in SPEC["pieces"]:
        why = piece_missing(piece, ran)
        if why:
            total += SPEC["piece_missing"]
            lines.append(f"{piece['name']}: {SPEC['piece_missing']:.2f} (doesn't count yet: {why})")
        else:
            lines.append(f"{piece['name']}: 0 (its check and tests pass)")
    print("\n".join(lines))
    print(f"{total:.2f}")
    return 0


def cmd_cost(names: list[str]) -> int:
    dirs = [BOARDS / n for n in names] if names else boards()
    for d in dirs:
        say_cost(board_cost(d))
    return 0


def main(argv: list[str]) -> int:
    if argv[:1] == ["test"]:
        return cmd_test("--all" in argv[1:])
    if argv[:1] == ["score"]:
        return cmd_score()
    if argv[:1] == ["cost"]:
        return cmd_cost(argv[1:])
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
