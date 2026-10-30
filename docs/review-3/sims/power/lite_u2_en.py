"""Lite U2 (TPS54302) EN pin 5: confirm it has no net and no copper on the PCB, and
list nearby copper of other nets (SW/BOOT coupling into a high-impedance pin).
Exit 1 if pad 5 carries a real net or a track/via lands on it. Read-only.
Run: python3 -I docs/review-3/sims/power/lite_u2_en.py [board.kicad_pcb]"""
import sys, math, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcblib import Pcb, seg_dist
b = Pcb(sys.argv[1] if len(sys.argv) > 1 else "/work/boards/lite-main/kicad/lite-main.kicad_pcb")
u = b.fps["U2"]
p5 = u['pads']['5'][0]; c = p5['xy']
print(f"U2 {u['layer']} rot {u['rot']}; pad 5 net {p5['net']!r} at {c[0]:.2f},{c[1]:.2f}")
fail = bool(p5['net']) and not p5['net'].startswith('unconnected')
for n, ps in sorted(u['pads'].items()):
    print(f"  pad {n} {ps[0]['net']:28s} {math.dist(ps[0]['xy'], c):5.2f} mm from pad 5")
near = {}
for s in b.segs:
    d = seg_dist(c, s['a'], s['b']) - s['w'] / 2
    if d < 0.3 and s['layer'] == u['layer']:
        print("  track over pad 5:", s['net']); fail = True
    if d < 1.5:
        near[(s['net'], s['layer'])] = min(near.get((s['net'], s['layer']), 9), round(d, 2))
for v in b.vias:
    if math.dist(v['xy'], c) < 0.4: print("  via on pad 5:", v['net']); fail = True
for (nm, ly), d in sorted(near.items(), key=lambda x: x[1]):
    print(f"  track {nm} on {ly}: {d} mm edge-to-pad-centre")
print("FAIL" if fail else "PASS: EN floats on the internal pull-up; no copper lands on it")
sys.exit(1 if fail else 0)
