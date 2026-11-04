"""Assert that every value and connection the core PVDD bench (pvdd_switch.py) uses
matches boards/core/kicad/core.net (copper as of the C43 ECO, 2026-11-03).
Prints each check; exit 1 on any mismatch. Read-only."""
import re, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "docs/review-5/sims/pa"))
from netlib import Net
N = Net(str(ROOT / "boards/core/kicad/core.net"))

def num(s):
    m = re.match(r"\s*([\d.]+)\s*([pnuµmkMGR]?)", s)
    return float(m.group(1)) * {"p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "m": 1e-3, "": 1, "R": 1,
                                "k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]

bad = []; ok = [0]
def chk(what, got, want):
    if got == want: ok[0] += 1
    else: bad.append(f"{what}: netlist={got!r} bench={want!r}")
def two(ref, a, b, want):
    chk(f"{ref} nets", sorted(N.pins_of(ref).values()), sorted([a, b]))
    g, w = num(N.value(ref)), num(want)
    chk(f"{ref} value {N.value(ref)}", abs(g - w) <= 1e-6 * w, True)

q1 = N.pins_of("Q1")
chk("Q1 part", N.value("Q1"), "AO4485")
for p in "123": chk(f"Q1.{p} source", q1[p], "VBUS")
chk("Q1.4 gate", q1["4"], "PVDD_G")
for p in "5678": chk(f"Q1.{p} drain", q1[p], "PVDD")
two("R1", "VBUS_IN", "VBUS", "10m")
two("R17", "VBUS", "VBUS_PD", "10")
two("C1", "VBUS_PD", "GND", "1u")
two("R4", "VBUS", "PVDD_G", "470k")
two("R5", "PVDD_G", "PVDD_PD", "470k")
two("C4", "PVDD_G", "PVDD", "220n")
two("C8", "VBUS", "PVDD_G", "4.7u")
two("R6", "PVDD_EN", "PVDD_ENG", "1k")
two("R7", "PVDD_ENG", "GND", "100k")
two("R43", "PVDD_EN", "GND", "100k")
two("R404", "VBUS", "PVDD_PD", "100k")
two("R8", "OV_Z", "OV_B", "10k")
two("R9", "OV_B", "GND", "10k")
two("R20", "PD_PG", "+3V3", "10k")
two("C10", "VBUS", "GND", "10u")
two("C7", "PVDD", "GND", "10u")
for r in ("C5", "C6", "C311"):
    chk(f"{r} nets", sorted(N.pins_of(r).values()), ["GND", "PVDD"]); chk(f"{r} value", N.value(r), "470uF/50V")
for r in ("C207", "C209", "C307", "C309"): two(r, "PVDD", "GND", "1u")
for r in ("C208", "C210", "C308", "C310"): two(r, "PVDD", "GND", "100n")
chk("D4 BZT52C15", (N.value("D4"), sorted(N.pins_of("D4").values())), ("BZT52C15", ["PVDD_G", "VBUS"]))
chk("D2 BZT52C33", (N.value("D2"), sorted(N.pins_of("D2").values())), ("BZT52C33", ["OV_Z", "VBUS"]))
chk("D5 BAS16L", (N.value("D5"), sorted(N.pins_of("D5").values())), ("BAS16L", ["PVDD_OFFB", "PVDD_PD"]))
chk("D1 SMBJ30A", (N.value("D1"), sorted(N.pins_of("D1").values())), ("SMBJ30A", ["GND", "VBUS_IN"]))
chk("Q2 2N7002", (N.value("Q2"), sorted(N.pins_of("Q2").values())), ("2N7002", ["PD_PG", "PVDD_ENG", "PVDD_PD"]))
chk("Q3 MMDT3904", (N.value("Q3"), sorted(N.pins_of("Q3").values())),
    ("MMDT3904", sorted(["PVDD_ENG", "OV_B", "GND", "VBUS", "PVDD_OFFB", "PVDD_G"])))
chk("PVDD loads", sorted(r for r in N.refs_on("PVDD") if r[0] == "U"), ["U15", "U16"])

for b in bad: print("MISMATCH", b)
print(f"core vals: {ok[0]} checks ok, {len(bad)} mismatches")
sys.exit(1 if bad else 0)
