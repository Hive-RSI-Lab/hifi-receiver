"""Lite-main TPA3118D2 (U8) PVDD HF-decoupling loop: new check (review-3 amp.md only checked
cap-to-pin distance, 2.9-5.6 mm; nobody estimated loop inductance or the edge spike on lite).
Same convention as docs/review-5/sims/pa/pvdd_hf_loop.py (core): spike = L*I/tr, fail if the
peak leaves < 2 V under the abs max. Read-only on boards/.

Loop (read from lite-main.kicad_pcb, a 2-layer 1.53 mm board, everything on F.Cu, no inner plane):
  U8 PVDD pin group -> F.Cu to the 1 uF/100 nF/1 nF pad 1 -> cap body -> pad 2 -> F.Cu to the
  nearest GND via -> via (1.53 mm) -> B.Cu GND pour straight to the via 1.3 mm below the U8 GND
  pin (pins 28 left / 22 right, each has its own via) -> via up -> 1.3 mm F.Cu to the GND pin ->
  (inside the IC, short) -> PVDD pin group.
Two estimates of its inductance:
  A) microstrip convention as on core: L' = mu0*h/w per mm for the F.Cu PVDD strip and the B.Cu
     return, h = 1.53 mm, w = 1.5 mm (assumed finger/pour width), vias 1.1 nH each
     (0.2*h*(ln(4h/d)+1) nH, h 1.53 mm, d 0.3 mm), no cap ESL.
  B) filament (partial-inductance) sum over the same polyline: Neumann integrals between all
     straight segments, return filament on B.Cu directly under the F.Cu path (best case for a
     plane), regularised GMD 0.25 mm strips / 0.12 mm vias.
Assumptions, not measured: I = 7.5 A (TPA3118D2 OC limit, review-3 amp.md), tr = 3 ns (the
datasheet gives no output edge time; core's value reused), PVDD max = 20 V + 5 % (PD 20 V
profile; the 21 V is what the board sees in operation), abs max 30 V (SLOS708G, review-2 power.md
POWER-9). Exit 1 if the estimated peak (either method, worst single 1 uF cap) leaves < 2 V."""
import math, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "docs/review-3/sims/layout"))
from pcbgeo import load_board
b = load_board(str(ROOT / "boards/lite-main/kicad/lite-main.kicad_pcb"))
fp = b["footprints"]
mu0 = 4e-7 * math.pi
H = 1.53e-3
I, TR, PVDD_MAX, ABS = 7.5, 3e-9, 21.0, 30.0
u8 = fp["U8"]
pad = {p["num"]: p for p in u8["pads"] if p["num"] != "33"}
gvia = [v["xy"] for v in b["vias"] if v["net"] == "GND"]
d2 = math.dist

def partial(l_m, a_m):                      # analytic self term, regularised wire
    return mu0 / (4 * math.pi) * 2 * (l_m * math.asinh(l_m / a_m) - math.sqrt(l_m**2 + a_m**2) + a_m)

def mutual(s1, s2, a, n=24):
    import numpy as np
    x, w = np.polynomial.legendre.leggauss(n)
    (p0, p1), (q0, q1) = s1, s2
    p0, p1, q0, q1 = [np.array(v) * 1e-3 for v in (p0, p1, q0, q1)]
    d1, d2_ = p1 - p0, q1 - q0
    dot = float(d1 @ d2_)
    if abs(dot) < 1e-30: return 0.0
    t = (x + 1) / 2; wt = w / 2
    P = p0[None, :] + t[:, None] * d1[None, :]
    Q = q0[None, :] + t[:, None] * d2_[None, :]
    r = np.sqrt(((P[:, None, :] - Q[None, :, :]) ** 2).sum(-1) + a**2)
    return mu0 / (4 * math.pi) * dot * float((wt[:, None] * wt[None, :] / r).sum())

def loop_filament(segs):                    # segs: [(p0, p1, a_mm)] 3-D points in mm, directed
    L = 0.0
    for i, (p0, p1, ai) in enumerate(segs):
        l = d2(p0, p1) * 1e-3
        L += partial(l, ai * 1e-3)
        for j, (q0, q1, aj) in enumerate(segs):
            if i != j: L += mutual((p0, p1), (q0, q1), (ai + aj) / 2 * 1e-3)
    return L

caps = {"left": ["C55", "C56", "C57"], "right": ["C51", "C52", "C53", "C54"]}
pv_pins = {"left": ("31", "32"), "right": ("17", "18", "19")}
gnd_pin = {"left": "28", "right": "22"}
L_via = 0.2 * 1.53 * (math.log(4 * 1.53 / 0.3) + 1) * 1e-9
print(f"lite-main U8 TPA3118D2 PVDD HF loop (estimate), I {I} A, tr {TR*1e9:.0f} ns, "
      f"PVDD max {PVDD_MAX} V, abs max {ABS} V; single 0.6/0.3 via L {L_via*1e9:.2f} nH")
res = {}
for side, refs in caps.items():
    pp = [pad[n] for n in pv_pins[side]]
    pc = (sum(p["x"] for p in pp) / len(pp), pp[0]["y"])
    gp = (pad[gnd_pin[side]]["x"], pad[gnd_pin[side]]["y"])
    for ref in refs:
        c = fp[ref]; p1 = [p for p in c["pads"] if p["net"] == "PVDD"][0]; p2 = [p for p in c["pads"] if p["net"] == "GND"][0]
        c1, c2 = (p1["x"], p1["y"]), (p2["x"], p2["y"])
        v = min(gvia, key=lambda q: d2(q, c2))
        e = min([q for q in gvia if d2(q, gp) < 1.5], key=lambda q: d2(q, v))   # via 1.3 mm below the GND pin
        strip = max(0.0, d2(pc, c1) - p1["w"] / 2); gnd_f = max(0.0, d2(c2, v) - p2["w"] / 2)
        bcu = d2(v, e)
        # A: microstrip convention
        wA = 1.5e-3
        LA = mu0 * H / wA * (strip + gnd_f + bcu) * 1e-3 + 2 * L_via
        # B: filament sum
        z0, z1 = 0.0, -H * 1e3
        segs = [((*pc, z0), (*c1, z0), 0.25), ((*c1, z0), (*c2, z0), 0.25),
                ((*c2, z0), (*v, z0), 0.25), ((*v, z0), (*v, z1), 0.12), ((*v, z1), (*e, z1), 0.25),
                ((*e, z1), (*e, z0), 0.12), ((*e, z0), (*gp, z0), 0.25), ((*gp, z0), (*pc, z0), 0.25)]
        LB = loop_filament(segs)
        res[ref] = (side, c["props"].get("Value"), strip, gnd_f, bcu, LA, LB)
        print(f"{ref} {c['props'].get('Value'):>6s} ({side}): PVDD strip {strip:.2f} mm, cap GND pad -> via {gnd_f:.2f} mm, "
              f"B.Cu via -> GND-pin via {bcu:.2f} mm; loop A {LA*1e9:.1f} nH, B {LB*1e9:.1f} nH")
bad = 0
print("per-cap edge spike (L*I/tr) and peak vs 30 V abs max:")
for ref, (side, val, s, g, bc, LA, LB) in res.items():
    for tag, L in (("A", LA), ("B", LB)):
        v = L * I / TR; left = ABS - PVDD_MAX - v
        print(f"  {ref} {tag}: {v:5.1f} V spike, peak {PVDD_MAX+v:5.1f} V, {left:+5.1f} V under {ABS:.0f} V")
        if val == "1uF" and left < 2.0: bad = 1
for side in caps:
    for tag, k in (("A", 5), ("B", 6)):
        Ls = [res[r][k] for r in caps[side]]
        Lp = 1 / sum(1 / x for x in Ls)
        v = Lp * I / TR
        print(f"side {side} {tag}: all caps in parallel (no mutual, optimistic) {Lp*1e9:.1f} nH -> {v:.1f} V spike, peak {PVDD_MAX+v:.1f} V")
room = ABS - 2.0 - PVDD_MAX
print(f"allowed spike {room:.1f} V -> loop must be <= {room*TR/I*1e9:.1f} nH at tr {TR*1e9:.0f} ns, "
      f"or the edge must be >= L*I/{room:.0f} V (e.g. best 1 uF cap, B: {min(r[6] for r in res.values() if r[1]=='1uF')*I/room*1e9:.1f} ns)")
print("RESULT:", "FAIL (worst single 1 uF cap leaves < 2 V under abs max)" if bad else "PASS (every 1 uF cap leaves >= 2 V under abs max)")
sys.exit(bad)
