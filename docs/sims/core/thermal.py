"""core thermal: TPA3220 (U15/U16) Tj at rated and 1/8 power, 4 and 8 ohm, in the vented
Plus/Ultra box; plus Q1 AO4485, shunt R1, output inductors L1-L4. Box air comes from
hwde check_thermal on boards/core/kicad/core.kicad_pcb + constraints.json (the enclosure
block: 166.8 x 125.4 x 39.8 mm PETG, room 30 C): the script reads its wall+vent
conductance G and re-solves air = room + heat / G with this script's heat list.
Chip loss: SLASEE9C fig 6-9 (2-ch, 30 V, incl. TI inductors), split per channel and
scaled as in docs/review-2/thermal.md 1.2. RthJA 23.5 C/W (JEDEC) and 26 C/W (board,
as firmware hifi_gov.c assumes). All ESTIMATES (hand calc on datasheet curves, no
measurement). Limits: OTW 115 C min (governor acts), OTE 145 C min (shutdown).
Exit 1 if the governed steady state crosses OTW."""
import json, os, subprocess, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SK = os.environ["HWDE_SKILL"]
out = Path(os.environ.get("TMPDIR", "/tmp")) / "core_check_thermal.json"
subprocess.run([sys.executable, f"{SK}/scripts/check_thermal.py", "--pcb", str(ROOT / "boards/core/kicad/core.kicad_pcb"),
                "--constraints", str(ROOT / "boards/core/kicad/constraints.json"), "--out", str(out)],
               capture_output=True, text=True)
ct = json.loads(out.read_text())
enc = ct["enclosure"]
G = enc["heat_w"] / enc["air_rise_c"]; ROOM = enc["room_c"]
print(f"check_thermal: status {ct['status']}, box heat {enc['heat_w']} W -> air {enc['air_c']:.1f} C, "
      f"G = {G:.3f} W/K (walls {enc['g_walls_w_k']:.3f} + vents {enc['g_vents_w_k']:.3f}), PETG Tg margin {enc['tg_margin_c']:.0f} K")
for v in ct["violations"]: print(f"  check_thermal {v['severity']}: {v['msg'][:150]}")

IDLE = 0.45                                  # per chip, AD mode, 28 V (review-2 1.2)
def chip_w(p_ch, n_ch, load):
    """per-chip loss for n_ch channels each at p_ch W. Per-channel increment from the
    review-2 table: 4 ohm 1/8 P (9.6 W/ch) 3.5 W/2ch -> 1.53 W/ch; full 77 W/ch -> 22 W/2ch;
    8 ohm 1/8 P (5.4 W/ch) 1.5 W/2ch; full 43 W/ch -> 9 W/2ch. Linear between points."""
    pts = {4: [(0, 0), (9.6, 1.525), (77, 10.78)], 8: [(0, 0), (5.4, 0.525), (43, 4.28)]}[load]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if p_ch <= x1: return IDLE + n_ch * (y0 + (y1 - y0) * (p_ch - x0) / (x1 - x0))
    return IDLE + n_ch * pts[-1][1]
def ind_w(p_ch, load, core=0.3):            # per inductor: DCR 30 mohm + core loss (0.2-0.4 W, unverified)
    return (p_ch / load) * 0.030 + core
OTHER = 3.0 + 0.34 + 0.16 + 0.09             # constraints other_w + U4/U5/U6 (rails.py)

bad = []
print("\nTPA3220 Tj (per chip), box air re-solved with chip + inductor + Q1 + other heat")
print(f"{'case':58s} {'chip W':>6s} {'air C':>6s} {'Tj@23.5':>7s} {'Tj@26':>6s}  vs OTW 115 / OTE 145")
CASES = [  # label, W per channel, channels per chip, chips, load, governed?
    ("Plus rated 77 W/ch 4 ohm, 1 ch/chip, continuous", 77, 1, 2, 4, False),
    ("Plus rated 43 W/ch 8 ohm, 1 ch/chip, continuous", 43, 1, 2, 8, False),
    ("Plus 1/8 P 9.6 W/ch 4 ohm, 1 ch/chip (= governor 19 W)", 9.6, 1, 2, 4, True),
    ("Plus 1/8 P 5.4 W/ch 8 ohm, 1 ch/chip", 5.4, 1, 2, 8, True),
    ("Ultra 1/8 P 9.6 W/ch 4 ohm, 2 ch/chip, both zones", 9.6, 2, 2, 4, False),
    ("Ultra governed 4.25 W/ch 4 ohm (17 W total), 2 ch/chip", 4.25, 2, 2, 4, True),
    ("Ultra 1/8 P 5.4 W/ch 8 ohm, 2 ch/chip, both zones", 5.4, 2, 2, 8, True),
]
govmax = 0
for label, pch, nch, nchip, load, gov in CASES:
    pc = chip_w(pch, nch, load)
    pin = (pch * nch * nchip) / 0.88 + 4.0
    iq1 = pin / 28.0
    heat = nchip * pc + 4 * ind_w(pch, load) * (nch * nchip / 2) + iq1**2 * (0.023 + 0.010) + OTHER
    air = ROOM + heat / G
    tj = [air + r * pc for r in (23.5, 26.0)]
    flag = "FAIL (OTE)" if tj[1] > 145 else "over OTW" if tj[1] > 115 else f"margin {115 - tj[1]:.0f} K"
    print(f"{label:58s} {pc:6.1f} {air:6.1f} {tj[0]:7.0f} {tj[1]:6.0f}  {flag}{'  [governed]' if gov else ''}")
    if gov:
        govmax = max(govmax, tj[1])
        if tj[1] > 115: bad.append(f"{label}: Tj {tj[1]:.0f} C")
print(f"governed worst Tj {govmax:.0f} C, margin to OTW {115 - govmax:.0f} K")

print("\nQ1 AO4485 conduction (RthJA 75 C/W AOS 1 in2; ~90 C/W on this copper, review-2), R1 10 mohm 2512 3 W")
for label, i in (("supply-limited 140 W in, 5.0 A", 5.0), ("governed Plus 19 W out, ~0.92 A", (19 / 0.88 + 4) / 28)):
    p = i * i * 0.023; tj = 55 + 90 * p
    print(f"  {label}: Q1 {p:.2f} W -> Tj {tj:.0f} C vs 150 C (margin {150 - tj:.0f} K); R1 {i*i*0.010:.2f} W of 3 W")
    if tj > 150: bad.append("Q1 Tj")
print("\nOutput inductors L1-L4 PSPMAA0805-100M (Irms 6 A at dT 40 K, Isat 10 A, DCR 30 mohm; core loss unverified)")
for label, pch in (("1/8 P 4 ohm", 9.6), ("rated 77 W/ch 4 ohm", 77)):
    irms = (pch / 4) ** 0.5; dt = 40 * (irms / 6) ** 2; ipk = (2 * pch * 4) ** 0.5 / 4 + 0.6
    print(f"  {label}: Irms {irms:.2f} A, DCR rise {dt:.0f} K (+ core loss), Ipk {ipk:.1f} A vs Isat 10 A (margin {100*(1-ipk/10):.0f} %)")
print("FAIL: " + "; ".join(bad) if bad else "governed cases PASS (ungoverned rated continuous is over OTE: design relies on governor + OTW)")
sys.exit(1 if bad else 0)
