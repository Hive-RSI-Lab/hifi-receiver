"""R5-PA: TPA3220 PVDD HF-decoupling loop after the amp ECO (PR #142), read
from core.kicad_pcb, and the edge-spike estimate with review-3's own
convention (docs/review-3/sims/amp/pvdd_spike.py: 7 A commutated in 3 ns,
microstrip L' = mu0*h/w, 0.5 nH per via group) so the before/after numbers
compare like for like. Read-only. Exit 1 if the estimated peak leaves < 2 V
under the 37 V abs max (SLASEN3B 7.1), as review-3's script did.

The ECO put each pin group's 1 uF/100 V 1206 on B.Cu under the group. The
loop is now: PVDD pins -> F.Cu strip -> PVDD via column -> B.Cu 1.5 mm track
-> 1206 -> B.Cu GND -> EP thermal vias -> EP -> GND pins. Measured here:
strip length pin row -> nearest PVDD via, B.Cu run via -> cap pad edge,
GND pad edge -> nearest EP via."""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbq

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
b = pcbq.load(f'{ROOT}/boards/core/kicad/core.kicad_pcb')
fp = pcbq.footprints(b)
vias = []
for v in pcbq.find(b, 'via'):
    at = pcbq.first(v, 'at'); net = pcbq.first(v, 'net')
    vias.append((float(at[1]), float(at[2]), net[1] if net else None))

mu0 = 4e-7 * math.pi
h_out = 0.2444e-3          # F.Cu/B.Cu to In1/In2 (stackup)
h_core = 1.065e-3 + h_out  # B.Cu to In1 GND (In2 here is PVDD, the same net)
I, tr, pvdd_max = 7.0, 3e-9, 29.4
L_via = 0.5e-9             # per via group, review-3's figure
bad = 0
rows = []
for u, groups in (("U15", (("29", "30", "31", "C207"), ("36", "37", "38", "C209"))),
                  ("U16", (("29", "30", "31", "C307"), ("36", "37", "38", "C309")))):
    pads = {p['num']: p for p in fp[u]['pads']}
    ep = [p for p in fp[u]['pads'] if p['num'] == fp[u]['pads'][-1]['num'] and p['w'] < 1]
    for a, bb, c, cap in groups:
        pins = [pads[a], pads[bb], pads[c]]
        px = sum(p['x'] for p in pins) / 3; py = pins[0]['y']
        cp = {p['num']: p for p in fp[cap]['pads']}
        c1, c2 = cp['1'], cp['2']
        pv = [v for v in vias if v[2] == 'PVDD']
        nv = min(pv, key=lambda v: math.hypot(v[0] - c1['x'], v[1] - c1['y']))
        strip = abs(py - nv[1])                                   # F.Cu pin row -> via
        bcu = max(0.0, math.hypot(nv[0] - c1['x'], nv[1] - c1['y']) - c1['h'] / 2)
        epv = [p for p in fp[u]['pads'] if p['w'] < 1 and p['kind'] == 'thru_hole']
        ne = min(epv, key=lambda p: math.hypot(p['x'] - c2['x'], p['y'] - c2['y']))
        gnd = max(0.0, math.hypot(ne['x'] - c2['x'], ne['y'] - c2['y']) - c2['h'] / 2)
        L = (mu0 * h_out / 1.6e-3) * strip * 1e-3 \
            + (mu0 * h_core / 1.5e-3) * bcu * 1e-3 \
            + (mu0 * h_core / 1.5e-3) * gnd * 1e-3 + 2 * L_via
        v = L * I / tr
        left = 37.0 - pvdd_max - v
        rows.append((u, cap, strip, bcu, gnd, L, v, left))
        if left < 2.0:
            bad = 1
print("pre-ECO (review-3 pvdd_spike.py): 7.5 mm top-side run -> 3.3 nH, 7.7 V, 37.1 V peak")
for u, cap, s, bc, g, L, v, left in rows:
    print(f"{u} {cap}: F.Cu strip {s:.2f} mm, B.Cu via->pad {bc:.2f} mm, GND pad->EP via {g:.2f} mm "
          f"-> loop ~{L*1e9:.2f} nH, spike {v:.1f} V, peak {pvdd_max+v:.1f} V, {left:.1f} V under 37 V")
print("(1206 ESL ~1 nH not counted, as review-3 did not count the 0805's)")
sys.exit(bad)
