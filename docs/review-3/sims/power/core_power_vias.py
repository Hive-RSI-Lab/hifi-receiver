"""core: count vias on the 5 A nets within 4 mm of R1 and Q1 pads, and at each TPA3220 PVDD pin group.
Gate: >= 5 vias (5 x ~1.2 A per 0.3 mm via at 10 K = 6 A against 5 A) at R1 in/out, Q1 S, Q1 D and each amp; exit 1 otherwise. Read-only."""
import sys, os, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcblib import Pcb
b = Pcb("/work/boards/core/kicad/core.kicad_pcb")
def vias_near(ref, net, r=4.0):
    pads = [p for ps in b.fps[ref]['pads'].values() for p in ps if p['net'].split('/')[-1] == net]
    return sum(1 for v in b.vias if v['net'].split('/')[-1] == net and any(math.dist(v['xy'], p['xy']) < r for p in pads))
fail = False
for ref, net in (("R1", "VBUS_IN"), ("R1", "VBUS"), ("Q1", "VBUS"), ("Q1", "PVDD"), ("U15", "PVDD"), ("U16", "PVDD")):
    n = vias_near(ref, net); ok = n >= 5
    fail |= not ok
    print(f"{ref} {net:8s} vias within 4 mm: {n:3d} {'ok' if ok else 'LOW'}")
print("FAIL" if fail else "PASS"); sys.exit(1 if fail else 0)
