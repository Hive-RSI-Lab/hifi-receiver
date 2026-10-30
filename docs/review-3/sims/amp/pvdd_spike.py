"""R3-AMP-2: switching-edge overshoot on core PVDD from the HF-decoupling
loop length (estimate). Loop = PVDD copper from the 1 uF pad to the PVDD pin
(measured by check_amp_layout.py) over In1 GND (0.244 mm dielectric), plus
the cap's GND return through vias and In1 to the GND pins.
Assumptions (stated, not measured): finger width 1.0 mm, microstrip
L' = mu0*h/w; 0.5 nH per via pair + pad; 7 A commutated in 3 ns (TPA3220
edges are not specified; TPA32xx class parts switch in ~2-5 ns).
Fails if PVDD_max + spike exceeds 37 V (SLASEN3B 7.1) with < 2 V left."""
import sys
mu0 = 4e-7 * 3.14159
h, w = 0.244e-3, 1.0e-3
Lp = mu0 * h / w            # H per m
pvdd_max = 29.4             # EPR 28 V +5 %
I, tr = 7.0, 3e-9
bad = 0
for name, d_mm in (("ours (C207-C210, 6.5-8.1 mm)", 7.5), ("TI fig 53 'T2' (~2 mm)", 2.0)):
    L = Lp * d_mm * 1e-3 + 0.5e-9 + 0.5e-9
    v = L * I / tr
    left = 37.0 - pvdd_max - v
    print(f"{name}: loop ~{L*1e9:.1f} nH -> {v:.1f} V spike, peak {pvdd_max+v:.1f} V, {left:.1f} V under 37 V abs max")
    if name.startswith("ours") and left < 2.0: bad = 1
sys.exit(bad)
