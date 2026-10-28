#!/usr/bin/env python3
"""The project's test and score.

    python3 tools/hwcheck.py test    the check: docs/product.md, the
                                     firmware's host tests, then every
                                     board's hwde gates
    python3 tools/hwcheck.py score   the first-built tier's unit cost at 100,
                                     in USD, from its board(s)' BOM, printed
                                     only when their gates pass (lower is better)
    python3 tools/hwcheck.py cost [BOARD ...]   the cost alone, no gates

docs/checks.md says what each part checks and how the cost is worked out.

Gates run through hive-hwde's bin/hwde, found as $HWDE_BIN, `hwde` on PATH,
or bin/hwde in the installed module ($HIVE_MODULES, else /hive/modules). Each
board runs in a scratch copy of its workspace, so the checkout stays as it is.
With boards to check and no hwde, the command execs `sh -c hwde` so the
shell's own "hwde: not found" and exit 127 say it couldn't run: that is no
verdict on the commit, not a red.
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

PRODUCT = ROOT / "docs" / "product.md"
BOARDS = ROOT / "boards"
TIERS = ("lite", "plus", "ultra")
GATES = ("erc", "drc_routed", "verify", "dfm")
QTY = 100
# The score when the first tier has no board yet: above any real unit cost,
# so the first board that passes its gates reads as a gain (docs/checks.md).
NO_BOARD = 1000.0


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
        if boards is not None and not re.findall(r"`([^`]+)`", boards[i]):
            bad.append(f"docs/product.md: {tier} names no board (`name`) in the Boards row")
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
    """bin/hwde, or exec the shell's own not-found (exit 127)."""
    found = os.environ.get("HWDE_BIN") or shutil.which("hwde")
    if not found:
        mods = Path(os.environ.get("HIVE_MODULES") or "/hive/modules")
        found = next((str(p) for p in (mods / "hive-hwde" / "bin" / "hwde",
                                       mods / "hwde" / "bin" / "hwde") if p.is_file()), None)
    if not found:
        sys.stdout.flush()
        os.execvp("sh", ["sh", "-c", "hwde"])
    return found


def gate_board(ws: Path, name: str, bin_: str, logs: Path) -> list[str]:
    """Each gate on one board's scratch workspace; the failures, explained."""
    bad = []
    for g in GATES:
        inp = ws / "kicad" / f"{name}.{'kicad_sch' if g == 'erc' else 'kicad_pcb'}"
        if not inp.is_file():
            bad.append(f"{name} {g}: no {inp.relative_to(ws)}")
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
        why = ""
        try:
            r = json.loads(out.read_text())
            why = "; ".join(f"{v.get('check') or v.get('kind', '?')}: {v.get('msg') or v.get('message', '')}"[:160]
                            for v in r.get("failing", [])[:6])
        except (OSError, ValueError):
            pass
        if not why:
            why = " | ".join(log.read_text(errors="replace").strip().splitlines()[-6:])
        bad.append(f"{name} {g}")
        print(f"  {g}: FAIL (exit {rc}) {why}", flush=True)
    return bad


def run_gates(dirs: list[Path]) -> list[str]:
    bin_ = hwde()
    scratch = Path(tempfile.mkdtemp(prefix="hwcheck-"))
    try:
        (scratch / "logs").mkdir()
        bad = []
        for d in dirs:
            name = board_name(d)
            ws = scratch / "boards" / d.name
            shutil.copytree(d, ws, ignore=shutil.ignore_patterns("__pycache__"))
            print(f"{d.name}: gates {', '.join(GATES)}", flush=True)
            bad += gate_board(ws, name, bin_, scratch / "logs")
        return bad
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


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


def board_cost(d: Path, qty: int = QTY) -> dict:
    """One board's cost per unit at qty boards, from fab/BOM.csv priced from
    parts/parts.json, with tools/bom_estimate.py's assembly and PCB model."""
    name = board_name(d)
    bom, parts = d / "fab" / "BOM.csv", d / "parts" / "parts.json"
    for f in (bom, parts):
        if not f.is_file():
            raise ValueError(f"{d.name}: no {f.relative_to(d)}")
    by_lcsc = {p["lcsc"]: p for p in json.loads(parts.read_text())["parts"] if p.get("lcsc")}
    pads, layers, w, h = pcb_facts(d / "kicad" / f"{name}.kicad_pcb")
    if layers not in est.PCB_BASE:
        raise ValueError(f"{d.name}: {layers} copper layers; tools/bom_estimate.py prices "
                         f"{', '.join(map(str, est.PCB_BASE))}")
    part_cost = smt = tht = 0.0
    extended, lines = set(), 0
    for r in csv.DictReader(bom.open(encoding="utf-8")):
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
    asm = est.SETUP + est.STENCIL + len(extended) * est.FEEDER + qty * (smt * est.SMT_JOINT + tht * est.THT_JOINT)
    pcb = est.pcb(layers, w, h, qty) * est.PCB_CAL
    return {"board": d.name, "lines": lines, "extended": len(extended), "smt": smt, "tht": tht,
            "layers": layers, "w": w, "h": h, "parts": part_cost, "assembly": asm / qty,
            "pcb": pcb / qty, "unit": part_cost + (asm + pcb) / qty}


def say_cost(c: dict) -> None:
    print(f"{c['board']}: {c['lines']} BOM lines, {c['extended']} extended, "
          f"{c['smt']:.0f} SMT / {c['tht']:.0f} THT joints, {c['layers']}-layer "
          f"{c['w']:.1f} x {c['h']:.1f} mm; at {QTY}: parts {c['parts']:.2f} + assembly "
          f"{c['assembly']:.2f} + PCB {c['pcb']:.2f} = {c['unit']:.2f} USD per board")


# ---- commands -----------------------------------------------------------------

def check_firmware() -> list[str]:
    """firmware/test: the firmware core's host unit tests (plain cc, no ESP-IDF)."""
    tests = ROOT / "firmware" / "test"
    if not (tests / "Makefile").exists():
        return []
    if not shutil.which("make") or not shutil.which(os.environ.get("CC", "cc")):
        print("firmware/test: no make or cc here, host tests not run")
        return []
    with tempfile.TemporaryDirectory() as tmp:  # build outside the checkout
        r = subprocess.run(["make", "-s", "-C", str(tests), f"BIN={tmp}"],
                           capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    print(out)
    return [] if r.returncode == 0 else ["firmware host tests"]


def cmd_test() -> int:
    bad, _ = check_product()
    for b in bad:
        print(f"FAIL {b}")
    if not bad:
        print("docs/product.md: every tier has its target price and BOM total; one tier is first")
    bad += check_firmware()
    dirs = boards()
    if not dirs:
        print("boards/: no boards yet, no gates to run")
    else:
        bad += run_gates(dirs)
    if bad:
        print(f"test: FAIL ({len(bad)}): " + "; ".join(bad[:8]))
        return 1
    print(f"test: pass ({len(dirs)} board(s))")
    return 0


def cmd_score() -> int:
    bad, info = check_product()
    if bad or not info:
        for b in bad or ["docs/product.md names no first tier"]:
            print(f"FAIL {b}")
        print("score: no number (docs/product.md)")
        return 1
    dirs = [BOARDS / b for b in info["boards"]]
    have = [d for d in dirs if (d / "state.json").is_file()]
    if not have:
        print(f"{info['tier']} (built first): no board yet ({', '.join(info['boards'])} under boards/), "
              f"so the score is the no-board value")
        print(f"{NO_BOARD:.2f}")
        return 0
    if len(have) < len(dirs):
        missing = [d.name for d in dirs if d not in have]
        print(f"score: no number ({info['tier']} needs {', '.join(missing)} under boards/ as well)")
        return 1
    failed = run_gates(have)
    if failed:
        print(f"score: no number: gates fail ({'; '.join(failed)}); a board that breaks a gate never counts")
        return 1
    try:
        costs = [board_cost(d) for d in have]
    except (ValueError, KeyError, OSError) as e:
        print(f"score: no number: {e}")
        return 1
    for c in costs:
        say_cost(c)
    unit = sum(c["unit"] for c in costs)
    if info.get("target"):
        print(f"{info['tier']}: {unit:.2f} USD per unit at {QTY}, against a target retail of "
              f"${info['target']:g} ({unit / info['target']:.0%} of it)")
    print(f"{unit:.2f}")
    return 0


def cmd_cost(names: list[str]) -> int:
    dirs = [BOARDS / n for n in names] if names else boards()
    for d in dirs:
        say_cost(board_cost(d))
    return 0


def main(argv: list[str]) -> int:
    if argv[:1] == ["test"]:
        return cmd_test()
    if argv[:1] == ["score"]:
        return cmd_score()
    if argv[:1] == ["cost"]:
        return cmd_cost(argv[1:])
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
