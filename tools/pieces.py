"""The checks that let a piece of the product count in the score: each one
reads the piece against something else in the repo (the board it runs on,
the firmware it talks to, the files its facts come from), so a stub file
in the right place doesn't pass. docs/checks.md says what each one checks.

Every check is check(root, piece) -> list of problems, empty when it passes.
An evaluator file, like hwcheck.py.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def need_tool(tool: str) -> None:
    """Exec the shell's own not-found (exit 127) when a tool isn't here."""
    if not shutil.which(tool):
        sys.stdout.flush()
        os.execvp("sh", ["sh", "-c", tool])


# ---- the board ----------------------------------------------------------------

def footprints(pcb: Path) -> list[dict]:
    """Each footprint on a .kicad_pcb: its library name, reference, value and
    pads (number -> net name, None when the pad has no net)."""
    out = []
    for fp in re.split(r"\n\t\(footprint ", pcb.read_text(encoding="utf-8"))[1:]:
        ref = re.search(r'\(property "Reference" "([^"]+)"', fp)
        val = re.search(r'\(property "Value" "([^"]*)"', fp)
        pads = {}
        for blk in re.split(r"\n\t\t\(pad ", fp)[1:]:
            num = re.match(r'"([^"]*)"', blk)
            net = re.search(r'\(net (?:\d+ )?"([^"]*)"\)', blk.split("\n\t\t)")[0])
            if num:
                pads.setdefault(num.group(1), net.group(1) if net else None)
        out.append({"lib": fp.split("\n", 1)[0].strip().strip('"'), "ref": ref.group(1) if ref else "",
                    "value": val.group(1) if val else "", "pads": pads})
    return out


def board_pcb(root: Path, board: str) -> Path:
    d = root / "boards" / board
    try:
        name = json.loads((d / "state.json").read_text()).get("board") or board
    except (OSError, ValueError):
        name = board
    return d / "kicad" / f"{name}.kicad_pcb"


# ESP32-WROVER-E / WROOM-32E module pads (Espressif's datasheets) -> GPIO.
# The WROVER's pads 27 and 28 (GPIO16/17) go to its PSRAM, so they're NC.
MODULE_GPIO = {4: 36, 5: 39, 6: 34, 7: 35, 8: 32, 9: 33, 10: 25, 11: 26, 12: 27, 13: 14, 14: 12,
               16: 13, 23: 15, 24: 2, 25: 0, 26: 4, 27: 16, 28: 17, 29: 5, 30: 18, 31: 19,
               33: 21, 34: 3, 35: 1, 36: 22, 37: 23}
ADC1_GPIO = {0: 36, 1: 37, 2: 38, 3: 39, 4: 32, 5: 33, 6: 34, 7: 35}
# What a net has to be called for a pin the firmware gives a job.
ROLES = {"bck": r"BCLK|BCK|SCLK|SCK", "ws": r"LRCK|LRCLK|WS|FS", "do": r"DOUT|DSDIN|SDIN|SDATA",
         "di": r"DIN|ADC|SDOUT", "sda": r"SDA", "scl": r"SCL", "mck": r"MCLK|MCK", "mute": r"MUTE",
         "button": r"BTN|SW|KEY|BUTTON|ENC", "ir": r"IR", "led": r"LED"}


def module(fps: list[dict]) -> dict | None:
    return next((f for f in fps if re.search(r"ESP32-(WROVER|WROOM)", f["lib"] + " " + f["value"], re.I)), None)


def gpio_nets(fps: list[dict]) -> tuple[dict[int, str | None], str]:
    """Each module GPIO's net on the board, and the module's value."""
    m = module(fps)
    if not m:
        return {}, ""
    psram = "WROVER" in (m["lib"] + m["value"]).upper()
    nets = {}
    for pad, gpio in MODULE_GPIO.items():
        if psram and gpio in (16, 17):
            continue
        nets[gpio] = m["pads"].get(str(pad))
    return nets, m["value"]


def wired(net: str | None, fps: list[dict]) -> bool:
    """A real net that reaches at least one pad off the module."""
    if not net or net.startswith("unconnected-"):
        return False
    return sum(1 for f in fps for n in f["pads"].values() if n == net) >= 2


# ---- the firmware's pin map --------------------------------------------------

def kconfig_defaults(patch_text: str, symbol: str) -> dict[str, str]:
    """The Kconfig string defaults a patch adds for a board symbol, from its
    '+ default "..." if SYMBOL' lines, by what each one is: the hunks don't
    always show the config they sit under, so it's told by its content."""
    out = {}
    for m in re.finditer(rf'^\+\s*default\s+"(.*)"\s+if\s+{symbol}\s*$', patch_text, re.M):
        v = m.group(1).encode().decode("unicode_escape")
        if re.search(r"(^|,)model=", v):
            out["DAC_CONFIG"] = v
        elif v.lstrip().startswith("[") and '"gpio"' in v:
            out["AUDIO_CONTROLS"] = v
        elif re.fullmatch(r"\d+=\w+(:\w+)?(,\d+=\w+(:\w+)?)*", v):
            out["SET_GPIO"] = v
    return out


def pin_map(root: Path, fw: str, board: str, symbol: str) -> tuple[list[tuple[str, int, str]], list[str]]:
    """The GPIOs the tier's firmware gives a job: (where, gpio, role regex)."""
    text = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                     for p in sorted((root / "firmware" / "patches").glob("*.patch")))
    bad = []
    if not re.search(rf"^\+\s*config\s+{symbol}\s*$", text, re.M):
        bad.append(f"no firmware/patches adds the Kconfig board {symbol}")
    d = kconfig_defaults(text, symbol)
    pins = []
    dac = d.get("DAC_CONFIG", "")
    keys = dict(kv.split("=", 1) for kv in dac.split(",") if "=" in kv)
    for k in ("bck", "ws", "do"):
        if k not in keys:
            bad.append(f"{symbol}'s DAC_CONFIG has no {k}= (the I2S pins)")
    for k, v in keys.items():
        if k in ROLES and re.fullmatch(r"\d+(:\d)?", v):
            pins.append((f"DAC_CONFIG {k}", int(v.split(":")[0]), ROLES[k]))
    try:
        for c in json.loads(d["AUDIO_CONTROLS"]) if d.get("AUDIO_CONTROLS") else []:
            pins.append(("AUDIO_CONTROLS button", int(c["gpio"]), ROLES["button"]))
    except (ValueError, KeyError, TypeError):
        bad.append(f"{symbol}'s AUDIO_CONTROLS isn't a JSON list of {{gpio: ...}}")
    for item in filter(None, d.get("SET_GPIO", "").split(",")):
        n, _, job = item.partition("=")
        job = job.split(":")[0].lower()
        role = ROLES["ir"] if job == "ir" else ROLES["led"] if job in ("green", "red", "blue", "led") else None
        if n.strip().isdigit() and role:
            pins.append((f"SET_GPIO {job}", int(n), role))
    # The component's own pins, from a header naming this board.
    for h in sorted([*(root / "firmware" / "components").glob("*/esp/*.h"),
                     *(root / "firmware" / "board" / fw).glob("*.h")]):
        t = h.read_text(encoding="utf-8", errors="replace")
        if not re.search(rf'#define\s+HIFI_BOARD\s+"{re.escape(board)}"', t):
            continue
        rel = h.relative_to(root).as_posix()
        for name, n in re.findall(r"#define\s+PIN_(\w+)\s+(\d+)\b", t):
            pins.append((f"{rel} PIN_{name}", int(n), re.escape(name)))
        for name, n in re.findall(r"#define\s+I2C_(SDA|SCL)\s+(\d+)\b", t):
            pins.append((f"{rel} I2C_{name}", int(n), name))
        for name, ch in re.findall(r"#define\s+ADC_(\w+)\s+ADC1_CHANNEL_(\d)\b", t):
            pins.append((f"{rel} ADC_{name}", ADC1_GPIO[int(ch)], re.escape(name)))
    return pins, bad


def flash_mb(cfg: str) -> int | None:
    m = re.search(r'^CONFIG_ESPTOOLPY_FLASHSIZE="(\d+)MB"', cfg, re.M)
    return int(m.group(1)) if m else None


def partitions(csv_text: str) -> list[tuple[str, str, int, int]]:
    rows = []
    for line in csv_text.splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        cells = [c.strip() for c in line.split(",")]
        size = cells[4].upper() if len(cells) > 4 else ""
        num = (lambda s: int(s[:-1]) * 1024 if s.endswith("K") else int(s[:-1]) << 20 if s.endswith("M")
               else int(s, 0))
        rows.append((cells[0], cells[1], num(cells[3]), num(size)))
    return rows


def firmware(root: Path, piece: dict) -> list[str]:
    """A tier's firmware: its board folder selects its own Kconfig board,
    whose pins (codec I2S and I2C, buttons, IR, LED, and the component's
    power and amp pins) each land on a module pad the tier's board wires to
    a net named for that job; its partitions fit the flash it asks for, and
    the module on the board has that flash. The host tests run beside it."""
    fw, board, symbol = piece["fw"], piece["board"], piece["kconfig"]
    d = root / "firmware" / "board" / fw
    bad = [f"no firmware/board/{fw}/{f}" for f in ("sdkconfig.defaults", "partitions.csv") if not (d / f).is_file()]
    if bad:
        return bad
    cfg = (d / "sdkconfig.defaults").read_text(encoding="utf-8", errors="replace")
    if not re.search(rf"^CONFIG_{symbol}=y\s*$", cfg, re.M):
        bad.append(f"firmware/board/{fw}/sdkconfig.defaults doesn't select CONFIG_{symbol}=y")
    pcb = board_pcb(root, board)
    if not pcb.is_file():
        return bad + [f"no {pcb.relative_to(root)}"]
    fps = footprints(pcb)
    nets, value = gpio_nets(fps)
    if not nets:
        return bad + [f"boards/{board} has no ESP32 module footprint"]
    pins, why = pin_map(root, fw, board, symbol)
    bad += why
    for where, gpio, role in pins:
        if gpio not in nets:
            bad.append(f"{where}: GPIO{gpio} isn't a usable pad of {value}")
        elif not wired(nets[gpio], fps):
            bad.append(f"{where}: GPIO{gpio} isn't wired on boards/{board} ({nets[gpio]})")
        elif not re.search(role, nets[gpio], re.I):
            bad.append(f"{where}: GPIO{gpio} is {nets[gpio]} on boards/{board}, not a /{role}/ net")
    mb = flash_mb(cfg)
    if mb is None:
        bad.append(f"firmware/board/{fw}/sdkconfig.defaults sets no CONFIG_ESPTOOLPY_FLASHSIZE")
    else:
        has = re.search(r"-N(\d+)", value)
        if has and int(has.group(1)) < mb:
            bad.append(f"the firmware asks for {mb} MB of flash; boards/{board}'s {value} has {has.group(1)} MB")
        try:
            rows = partitions((d / "partitions.csv").read_text(encoding="utf-8"))
        except (ValueError, IndexError) as e:
            return bad + [f"firmware/board/{fw}/partitions.csv: {e}"]
        end = 0
        for name, _, off, size in sorted(rows, key=lambda r: r[2]):
            if off < end:
                bad.append(f"partitions.csv: {name} overlaps the one before it")
            end = off + size
        if end > mb << 20:
            bad.append(f"partitions.csv ends at {end:#x}, past {mb} MB of flash")
        if not any(t == "app" for _, t, _, _ in rows):
            bad.append("partitions.csv has no app partition")
    return bad


# ---- the web app -------------------------------------------------------------

def firmware_routes(root: Path) -> set[str]:
    """The test API's routes the firmware's portable core answers."""
    out = set()
    for c in (root / "firmware" / "components").glob("*/core/*.c"):
        out |= set(re.findall(r'!strcmp\(route,\s*"([^"]+)"\)', c.read_text(encoding="utf-8", errors="replace")))
    return out


def app(root: Path, piece: dict) -> list[str]:
    """The web app packs (app/build.py, under its budget) into a C table that
    compiles; index.html loads only files in the pack, with a manifest whose
    icons are in it; and the app reads the firmware's info and state."""
    web, bad = root / "app" / "web", []
    need_tool(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run([sys.executable, "-B", str(root / "app" / "build.py"), "--out", tmp],
                           capture_output=True, text=True)
        if r.returncode:
            return [f"app/build.py fails: {(r.stderr or r.stdout).strip()[-300:]}"]
        c = subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Werror", "-c", "-o",
                            str(Path(tmp) / "w.o"), str(Path(tmp) / "webapp_files.c")],
                           capture_output=True, text=True)
        if c.returncode:
            bad.append(f"app/build.py's webapp_files.c doesn't compile: {c.stderr.strip()[-300:]}")
        packed = {p.name[:-3] for p in (Path(tmp) / "www").glob("*.gz")}
    if "index.html" not in packed:
        return bad + ["the pack has no index.html"]
    html = (web / "index.html").read_text(encoding="utf-8", errors="replace")
    refs = [x for x in re.findall(r'(?:href|src)="([^"#?]+)', html) if "://" not in x]
    for x in refs:
        if x.lstrip("./") not in packed:
            bad.append(f"index.html loads {x}, which isn't in the pack")
    scripts = re.findall(r'<script[^>]*src="([^"]+)"', html)
    manifest = re.search(r'<link[^>]*rel="manifest"[^>]*href="([^"]+)"', html)
    if not scripts:
        bad.append("index.html loads no script")
    if not manifest:
        bad.append("index.html links no manifest")
    else:
        try:
            m = json.loads((web / manifest.group(1)).read_text(encoding="utf-8"))
            if not m.get("name") or not m.get("start_url"):
                bad.append("the manifest has no name or start_url")
            for i in m.get("icons") or [{}]:
                if i.get("src", "").lstrip("./") not in packed:
                    bad.append(f"the manifest's icon {i.get('src')} isn't in the pack")
        except (OSError, ValueError) as e:
            bad.append(f"the manifest doesn't parse: {e}")
    js = "\n".join((web / s).read_text(encoding="utf-8", errors="replace")
                   for s in scripts if (web / s).is_file())
    calls = set(re.findall(r"\b(?:api|set)\('([a-z/]+)'", js))
    routes = firmware_routes(root)
    if "api/v1/" not in js:
        bad.append("the app doesn't call the firmware's api/v1/")
    for need in ("info", "state"):
        if need not in calls:
            bad.append(f"the app never reads {need}")
        elif need not in routes:
            bad.append(f"the firmware doesn't answer {need}")
    if calls - routes:
        print(f"  app: calls the firmware's core doesn't answer yet: {', '.join(sorted(calls - routes))}")
    return bad


# ---- the bench -----------------------------------------------------------------

def bench(root: Path, piece: dict) -> list[str]:
    """The bench's staged bring-up runs end to end against its mock board and
    writes its report, and it asks the firmware's own routes."""
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run([sys.executable, "-B", str(root / "bench" / "bench.py"), "--mock",
                            "--mock-time-scale", "1000", "--yes", "--out", tmp, "bringup", "--soak-s", "30"],
                           capture_output=True, text=True, timeout=600)
        report = Path(tmp) / "report.md"
        bad = [] if r.returncode == 0 else [f"bench.py --mock bringup exits {r.returncode}: "
                                            f"{(r.stderr or r.stdout).strip()[-300:]}"]
        if not report.is_file() or "pass" not in report.read_text(encoding="utf-8", errors="replace"):
            bad.append("bench.py --mock bringup writes no report with a pass in it")
    text = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in (root / "bench").rglob("*.py")
                     if "tests" not in p.parts)
    calls, routes = set(re.findall(r'\.call\("([a-z/]+)"', text)), firmware_routes(root)
    for need in ("info", "state", "selftest", "tone"):
        if need not in calls:
            bad.append(f"the bench never calls {need}")
        elif need not in routes:
            bad.append(f"the firmware doesn't answer {need}")
    return bad


# ---- the enclosures -------------------------------------------------------------

def stl_triangles(p: Path) -> list[tuple[tuple[float, ...], ...]]:
    data = p.read_bytes()
    if len(data) >= 84:
        n = struct.unpack_from("<I", data, 80)[0]
        if 84 + 50 * n == len(data):
            return [tuple(struct.unpack_from("<3f", data, 84 + 50 * i + 12 + 12 * k) for k in range(3))
                    for i in range(n)]
    v = [tuple(map(float, m)) for m in re.findall(rb"vertex\s+(\S+)\s+(\S+)\s+(\S+)", data)]
    if not data.lstrip().startswith(b"solid") or len(v) % 3:
        raise ValueError("not an STL")
    return [tuple(v[i:i + 3]) for i in range(0, len(v), 3)]


def closed(tris) -> bool:
    """Every edge of the mesh is shared by two triangles, or is a T-junction:
    a lone edge that lies along other lone edges, end to end (OCC's meshes
    split a face's edge where the next face has a vertex)."""
    edges = {}
    for t in tris:
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            k = (a, b) if a < b else (b, a)
            edges[k] = edges.get(k, 0) + 1
    if not edges or any(n > 2 for n in edges.values()):
        return False
    lone = [k for k, n in edges.items() if n == 1]

    def along(e, f) -> float:
        """f's length if it lies on segment e, else 0."""
        (a, b), ln = e, dist(*e)
        if all(abs(dist(a, p) + dist(p, b) - ln) < 1e-4 * max(ln, 1) for p in f):
            return dist(*f)
        return 0.0

    for e in lone:
        inside = sum(along(e, f) for f in lone if f != e)
        if abs(inside - dist(*e)) > 1e-4 * max(dist(*e), 1) and not any(along(f, e) for f in lone if f != e):
            return False
    return True


def dist(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def outline(pcb: Path) -> tuple[float, float]:
    text = pcb.read_text(encoding="utf-8")
    xs, ys = [], []
    for item in re.split(r"\n\t\(gr_", text)[1:]:
        item = re.split(r"\n\t\(", item, maxsplit=1)[0]
        if '(layer "Edge.Cuts")' in item:
            for x, y in re.findall(r"\((?:start|end|mid|center|xy) (-?[\d.]+) (-?[\d.]+)\)", item):
                xs.append(float(x))
                ys.append(float(y))
    return (max(xs) - min(xs), max(ys) - min(ys)) if xs else (0.0, 0.0)


def enclosure(root: Path, piece: dict) -> list[str]:
    """An enclosure's output folder: hive-mech's fit report with every check
    passing and naming each of the board's mounting holes; every STL a
    closed mesh; the largest one's footprint around the board's outline;
    every STEP a solid."""
    out, board = root / piece["out"], piece["board"]
    fit = out / "fit.md"
    if not fit.is_file():
        return [f"no {fit.relative_to(root)}"]
    text = fit.read_text(encoding="utf-8")
    results = [c[1].strip().lower() for c in (line.strip().strip("|").split("|") for line in text.splitlines()
                                             if line.startswith("|")) if len(c) > 1]
    bad = []
    if "pass" not in results or "fail" in results:
        bad.append(f"{fit.relative_to(root)}: {results.count('fail')} fail, {results.count('pass')} pass")
    pcb = board_pcb(root, board)
    if not pcb.is_file():
        return bad + [f"no {pcb.relative_to(root)}"]
    holes = [f["ref"] for f in footprints(pcb) if re.search(r"MountingHole", f["lib"], re.I)]
    for h in holes:
        if not re.search(rf"(?<![A-Za-z0-9]){re.escape(h)}(?![0-9])", text):
            bad.append(f"{fit.relative_to(root)} checks nothing at {board}'s mounting hole {h}")
    if not holes:
        bad.append(f"boards/{board} has no mounting holes to fit against")
    stls, steps = sorted(out.glob("*.stl")), sorted(out.glob("*.step"))
    if not stls or not steps:
        bad.append(f"{out.relative_to(root)} has {len(stls)} STL and {len(steps)} STEP files")
    big = (0.0, 0.0, 0.0)
    for s in stls:
        try:
            tris = stl_triangles(s)
        except (ValueError, struct.error) as e:
            bad.append(f"{s.relative_to(root)}: {e}")
            continue
        if not closed(tris):
            bad.append(f"{s.relative_to(root)} isn't a closed mesh")
            continue
        pts = [p for t in tris for p in t]
        size = tuple(max(p[i] for p in pts) - min(p[i] for p in pts) for i in range(3))
        if size[0] * size[1] > big[0] * big[1]:
            big = size
    w, h = outline(pcb)
    if stls and not (sorted(big[:2]) >= sorted((w, h)) and min(big[:2]) >= min(w, h)
                     and max(big[:2]) >= max(w, h)):
        bad.append(f"the largest part ({big[0]:.1f} x {big[1]:.1f} mm) can't hold {board} ({w:.1f} x {h:.1f} mm)")
    for s in steps:
        t = s.read_text(encoding="utf-8", errors="replace")
        if not t.startswith("ISO-10303-21;") or "CLOSED_SHELL" not in t:
            bad.append(f"{s.relative_to(root)} isn't a STEP file with a solid")
    return bad


# ---- the manual ------------------------------------------------------------------

TOP = ("app", "bench", "boards", "docs", "enclosure", "firmware", "tools")


def manual(root: Path, piece: dict) -> list[str]:
    """The manual's facts traced: docs/manual/sources.md lists them in tables,
    some taken from a committed file; every repo file it names is there;
    every section it cites (§N, QS N) is a heading in the manual or quick
    start; and every numbered section of both is cited by some fact."""
    m = root / "docs" / "manual"
    files = {"§": m / "user-manual.md", "QS": m / "quick-start.md"}
    src = m / "sources.md"
    bad = [f"no {p.relative_to(root)}" for p in (*files.values(), src) if not p.is_file()]
    if bad:
        return bad
    s = src.read_text(encoding="utf-8")
    rows = [line for line in s.splitlines() if line.startswith("|") and not set(line) <= set("|-: ")]
    facts = [r for r in rows if not re.match(r"\|\s*Fact\s*\|", r)]
    if not facts or not re.search(r"\|\s*source\b|^##.*\(source\)", s, re.M):
        bad.append("docs/manual/sources.md has no fact taken from a source")
    for p in sorted(set(re.findall(r"`([^`\s]+)`", s))):
        if p.split("/")[0] in TOP and "/" in p and not (root / p).exists():
            bad.append(f"docs/manual/sources.md names {p}, which isn't in the repo")
        elif p.endswith(".md") and "/" not in p and not (m / p).exists():
            bad.append(f"docs/manual/sources.md names {p}, which isn't in docs/manual")
    heads = {k: set(re.findall(r"^##\s+(\d+)\.", f.read_text(encoding="utf-8"), re.M)) for k, f in files.items()}
    cited = {"§": set(re.findall(r"§\s*(\d+)", s)), "QS": set(re.findall(r"\bQS\s*(\d+)", s))}
    for k, f in files.items():
        if not heads[k]:
            bad.append(f"{f.relative_to(root)} has no numbered sections (## 1. ...)")
        for n in sorted(cited[k] - heads[k], key=int):
            bad.append(f"docs/manual/sources.md cites {k} {n}, which {f.name} doesn't have")
        for n in sorted(heads[k] - cited[k], key=int):
            bad.append(f"{f.name}'s section {n} traces to no fact in sources.md ({k} {n})")
    return bad


CHECKS = {"firmware": firmware, "app": app, "bench": bench, "enclosure": enclosure, "manual": manual}
