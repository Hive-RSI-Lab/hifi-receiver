"""ETH_CLK (50 MHz RMII REF_CLK) as routed on core: Y2 -> 19 mm ETH_OSC ->
R60 33R -> 2.6 mm -> T: 36.5 mm to U9 CLKIN (LAN8720A), 56.6 mm to U7 GPIO0
(ESP32), with Q5's collector and R31 near the T. Z0 73 ohm, 5.77 ps/mm
(hwde check_si field solve, coated microstrip, w 0.2 mm). Lengths from the
segments in core.kicad_pcb (DAL review-5). Writes tb_<case>.cir + bounds."""
import json, sys
PS = 5.77e-12
def line(name, a, b, mm):
    return f"T{name} {a} 0 {b} 0 Z0=73 TD={mm*PS:.4e}\n"
cases = {  # name: (rise 10-90 ns, driver ohm)
    "tr1_rdrv20": (1.0, 20), "tr2_rdrv20": (2.0, 20), "tr1_rdrv10": (1.0, 10),
}
for name, (tr, rd) in cases.items():
    t0 = 2e-9
    c = f"* ETH_CLK branch bench {name}: one 50 MHz period from Y2\n"
    c += f"Vs src 0 PULSE(0 3.3 {t0} {tr*1.25e-9} {tr*1.25e-9} {10e-9 - tr*1.25e-9} 20e-9)\n"
    c += f"Rdrv src y2 {rd}\nCy2 y2 0 1p\n"
    c += line("osc", "y2", "r60a", 19.0)
    c += "R60 r60a r60b 33\n" + line("t", "r60b", "tj", 2.6)
    c += "Cq5 tj 0 3p\nR31 tj vdd 10k\nVdd vdd 0 3.3\n"
    c += line("phy", "tj", "phy", 36.5) + "Cphy phy 0 4p\n"
    c += line("esp", "tj", "esp", 56.6) + "Cesp esp 0 6p\n"
    c += ".tran 5p 22n 0 5p\n"
    # first half-period (high): any dip below the 2.0 V upper threshold after
    # the edge is a ring-back; window 1.5 ns after the edge to 11 ns
    for n in ("phy", "esp"):
        c += f".measure tran {n}_max MAX v({n}) FROM=0 TO=12n\n"
        c += f".measure tran {n}_min_hi MIN v({n}) FROM={t0+tr*1.25e-9+1.0e-9:.3e} TO=11n\n"
        c += f".measure tran {n}_min MIN v({n}) FROM=12n TO=22n\n"
        c += f".measure tran {n}_max_lo MAX v({n}) FROM={12e-9+tr*1.25e-9+1.0e-9:.3e} TO=21n\n"
    c += ".end\n"
    open(f"tb_{name}.cir", "w").write(c)
    b = []
    for n, part, vih in (("phy", "LAN8720A CLKIN", 2.0), ("esp", "ESP32 GPIO0", 2.48)):
        b += [{"measure": f"{n}_min_hi", "min": vih, "severity": "error", "msg": f"{part}: ring-back under VIH {vih} V in the high half (double clock)"},
              {"measure": f"{n}_max_lo", "max": 0.8, "severity": "error", "msg": f"{part}: ring-back over VIL 0.8 V in the low half"},
              {"measure": f"{n}_max", "max": 3.3 + 0.6, "severity": "warning", "msg": f"{part}: overshoot past VDD+0.6 V (clamp current)"},
              {"measure": f"{n}_min", "min": -0.6, "severity": "warning", "msg": f"{part}: undershoot under -0.6 V"}]
    json.dump(b, open(f"tb_{name}.bounds.json", "w"), indent=1)
