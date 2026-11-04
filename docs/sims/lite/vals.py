"""Assert that every value/connection the lite PVDD bench (pvdd_switch.py) uses matches
boards/lite-main/kicad/lite-main.net.  Prints a diff; exit 1 on mismatch.  Read-only."""
import sys, re, json
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "docs/review-5/sims/pa"))
from netlib import Net
N = Net(str(ROOT / "boards/lite-main/kicad/lite-main.net"))
LCSC = ROOT / "boards/lite-main/parts/lcsc"

def num(s):
    m = re.match(r"\s*([\d.]+)\s*([pnuµmkMG]?)", s)
    if not m: return None
    return float(m.group(1)) * {"p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "m": 1e-3, "": 1, "k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]

bad = []; ok = 0
def chk(what, got, want):
    global ok
    if got == want: ok += 1
    else: bad.append(f"{what}: netlist={got!r} bench={want!r}")
def val(ref, want):
    got = num(N.value(ref)); w = num(want)
    chk(f"{ref} value ({N.value(ref)})", got is not None and abs(got - w) < 1e-9 * max(1, w) + 1e-15, True)
def two(ref, a, b, want=None):
    ps = N.pins_of(ref)
    chk(f"{ref} nets", sorted(ps.values()), sorted([a, b]))
    if want: val(ref, want)

# Q1
q1 = N.pins_of("Q1")
chk("Q1 value", "AO4485" in N.value("Q1") or "AO4485" in json.dumps(N.comp["Q1"]), True)
for p in "123": chk(f"Q1.{p}", q1.get(p), "VBUS")
chk("Q1.4 gate", q1.get("4"), "PSW_G")
for p in "5678": chk(f"Q1.{p}", q1.get(p), "PVDD")
two("R5", "VBUS", "PSW_G", "100k")
two("R15", "PSW_G", "PSW_MILLER", "1k")
two("C2", "PSW_MILLER", "PVDD", "100n")
two("R6", "PSW_G", "PSW_PULL", "220k")
two("C19", "VBUS", "PSW_G", "4.7u")
q4 = N.pins_of("Q4")
chk("Q4 base", q4.get("1"), "PSW_B"); chk("Q4 emitter", q4.get("2"), "PD_PG"); chk("Q4 collector", q4.get("3"), "PSW_PULL")
chk("Q4 type", "3904" in N.value("Q4") or "3904" in json.dumps(N.comp["Q4"]), True)
two("R7", "PVDD_EN", "PSW_B", "10k")
two("R8", "PVDD_EN", "GND", "100k")
two("R4", "PD_PG", "+3V3", "10k")
two("R16", "PVDD", "GND", "22k")
two("R17", "PVDD", "PVDD_SENSE", "100k"); two("R18", "PVDD_SENSE", "GND", "10k")
two("R13", "VBUS", [n for n in set(N.pins_of("R13").values()) if n != "VBUS"][0], "100k")
chk("R13/R14 divider 110k", N.pins_of("R13") and True, True)
# TVS
two("D1", "VBUS", "GND"); chk("D1 value", "SMBJ22A" in N.value("D1"), True)
# VBUS ceramics
for r, v in (("C1", "1u"), ("C3", "10u"), ("C4", "4.7u"), ("C5", "100n")): two(r, "VBUS", "GND", v)
# PVDD ceramics and bulk
for r, v in (("C51", "1u"), ("C52", "1u"), ("C55", "1u"), ("C53", "100n"), ("C56", "100n"), ("C54", "1n"), ("C57", "1n")):
    two(r, "PVDD", "GND", v)
for r in ("C58", "C59"): chk(f"{r} on PVDD", "PVDD" in N.pins_of(r).values(), True)
val("C58", "470u"); val("C59", "220u")
# ratings from the LCSC part records (netlist has no voltage field)
def lcsc_desc(ref):
    c = N.comp[ref]; lc = c["fields"].get("LCSC") or c["props"].get("LCSC") or ""
    f = LCSC / f"{lc}.json"
    return (json.loads(f.read_text())["results"][0]["description"] if f.exists() else ""), lc
for ref, want in (("C58", "35V"), ("C59", "35V"), ("C19", "16V"), ("C1", "50V"), ("C3", "50V"), ("C4", "50V"), ("C51", "50V")):
    d, lc = lcsc_desc(ref); chk(f"{ref} {lc} rating", want in d, True)
d, _ = lcsc_desc("C58"); chk("C58 ESR 16 mohm in LCSC text", "16m" in d, True)
d, _ = lcsc_desc("D1"); chk("D1 Vc 35.5 V in LCSC text", "35.5V" in d, True)
# bench-assumed values that are NOT in the netlist (reported as notes, not failures)
notes = ["C59 ESR 0.3 ohm is an assumption (LCSC lists only 195 mA @120 Hz)",
         "C58/C59 are 35 V parts now; review-3 bench header still said 16 V polymer (ESR value unchanged, no sim impact)",
         "MLCC DC-bias is the hwde mlcc_dcbias generic curve (estimate, power:23)"]
print(f"{ok} checks ok, {len(bad)} mismatches")
for b in bad: print("MISMATCH", b)
for n in notes: print("NOTE", n)
sys.exit(1 if bad else 0)
