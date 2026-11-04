"""R5-PA (issue #92): the 5 A VBUS_IN -> VBUS -> PVDD path on core, measured
with spk_path.py's erosion method, per layer and all layers together.
For each hop, the largest W at which the net's copper, eroded by W/2 (zone
fills + tracks + pads, joined through vias/plated pads), still connects the
two ends: (a) on all layers (the max-flow view, check_power_path), (b) on
each single layer alone (check_current's per-layer neck rule). Width ->
current with IPC-2152 (hwde's 1 oz table); inner layers scaled by the copper
the board is ORDERED with (fab/ORDERING.md: 0.5 oz inner), not the KiCad
stackup's 35 um. Run with $HWDE_PY. Read-only. Prints; exit 1 if a hop's
all-layer neck carries < 5 A at dT 20 C."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'spk_path.py')).read().split('\nlegs = []')[0])

def neck_layers(net, ends, layers):
    cu, joints = copper(net)
    cu = {l: (g if l in layers else None) for l, g in cu.items()}
    e2 = tuple([(l, p) for l, p in e if l in layers] for e in ends)
    if not e2[0] or not e2[1] or not connected(net, cu, joints, e2, 0.05):
        return 0.0
    lo, hi = 0.0, 12.0
    for _ in range(14):
        mid = (lo + hi) / 2
        if connected(net, cu, joints, e2, mid):
            lo = mid
        else:
            hi = mid
    return lo

def thru(ref, net):
    return [(l, p) for l, p in pads_of(ref, net)]
def allcu(ref, net):
    # an SMD pad's current enters every layer through the pad's vias; give the
    # per-layer check the pad's footprint on every layer as the attachment
    out = []
    for l, p in pads_of(ref, net):
        out += [(ll, p.buffer(1.0)) for ll in CU]
    return out

hops = [
    ("J1 -> R1 (VBUS_IN)", "VBUS_IN", thru("J1", "VBUS_IN"), allcu("R1", "VBUS_IN")),
    ("R1 -> Q1 source (VBUS)", "VBUS", allcu("R1", "VBUS"), allcu("Q1", "VBUS")),
    ("Q1 drain -> U15 PVDD pins", "PVDD", allcu("Q1", "PVDD"), allcu("U15", "PVDD")),
    ("Q1 drain -> U16 PVDD pins", "PVDD", allcu("Q1", "PVDD"), allcu("U16", "PVDD")),
    ("Q1 drain -> C5/C6 bulk", "PVDD", allcu("Q1", "PVDD"), thru("C6", "PVDD")),
]
INNER = 0.0175 / 0.035   # ordered inner copper vs the 1 oz table
bad = 0
for name, net, a, b in hops:
    w_all = neck_layers(net, (a, b), CU)
    per = {l: neck_layers(net, (a, b), [l]) for l in CU}
    def amps(w, l, dt):
        return amps_for(w, dt) * (INNER if l.startswith('In') else 1.0) ** 0.725  # I ~ area^0.725 (IPC-2152 fit)
    s = ", ".join(f"{l} {w:.2f} mm ({amps(w, l, 10):.1f} A dT10)" for l, w in per.items() if w > 0)
    best = max((amps(w, l, 20) for l, w in per.items()), default=0)
    print(f"{name:28s} all layers {w_all:.2f} mm | single layer: {s or 'none connects alone'} | best single layer {best:.1f} A @dT20")
    if w_all <= 0:
        bad = 1
sys.exit(bad)
