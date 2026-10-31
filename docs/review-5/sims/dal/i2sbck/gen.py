"""I2S_BCK as routed on core (netpath.py on core.kicad_pcb): ESP32 GPIO2 ->
61.9 mm -> X1 (1.5 mm stub to U13 ES7243E SCLK) -> 27.7 mm -> X2 (4.9 mm to
U12 PCM5102A BCK, 8.9 mm to U10 ES9018K2M) with no series resistor. Z0 73
ohm, 5.77 ps/mm (hwde check_si). ESP32 pad driver (drive cap 2, ~20 mA)
modelled as 30 ohm (and 45 ohm) behind a 2 ns / 1.5 ns ramp. 3.072 MHz
(64 fs at 48 kHz): bench one edge each way, 150 ns half period.
Bounds: no ring-back through a CMOS 0.7/0.3 VDD (2.31/0.99 V) threshold after the edge."""
import json
PS = 5.77e-12
def T(n, a, b, mm): return f"T{n} {a} 0 {b} 0 Z0=73 TD={mm*PS:.4e}\n"
cases = {"tr2_r30": (2.0, 30), "tr1p5_r30": (1.5, 30), "tr2_r45": (2.0, 45), "fix_r33_tr1p5": (1.5, 30)}
for name, (tr, rd) in cases.items():
    t0 = 5e-9; e = tr * 1.25e-9
    c = f"* I2S_BCK bench {name}\nVs s 0 PULSE(0 3.3 {t0} {e} {e} {160e-9 - e} 320e-9)\nRd s d {rd}\nCd d 0 3p\n"
    if name.startswith("fix"):
        c += "Rser d d2 33\n" + T("a", "d2", "x1", 61.9)
    else:
        c += T("a", "d", "x1", 61.9)
    c += T("adc", "x1", "adc", 1.5) + "Cadc adc 0 5p\n" + T("b", "x1", "x2", 27.7)
    c += T("pcm", "x2", "pcm", 4.9) + "Cpcm pcm 0 5p\n" + T("sab", "x2", "sab", 8.9) + "Csab sab 0 6p\n"
    c += ".tran 10p 330n 0 10p\n"
    for n in ("adc", "pcm", "sab"):
        c += f".measure tran {n}_min_hi MIN v({n}) FROM={t0+e+2.5e-9:.3e} TO=160n\n"
        c += f".measure tran {n}_max_lo MAX v({n}) FROM={t0+160e-9+e+2.5e-9:.3e} TO=320n\n"
        c += f".measure tran {n}_max MAX v({n})\n.measure tran {n}_min MIN v({n})\n"
    c += ".end\n"
    open(f"tb_{name}.cir", "w").write(c)
    bd = []
    for n, part in (("adc", "ES7243E SCLK"), ("pcm", "PCM5102A BCK"), ("sab", "ES9018K2M BCK")):
        bd += [{"measure": f"{n}_min_hi", "min": 2.31, "severity": "error", "msg": f"{part}: ring-back under 0.7 VDD after the rising edge"},
               {"measure": f"{n}_max_lo", "max": 0.99, "severity": "error", "msg": f"{part}: ring-back over 0.3 VDD after the falling edge"},
               {"measure": f"{n}_max", "max": 3.9, "severity": "warning", "msg": f"{part}: overshoot over VDD+0.6"},
               {"measure": f"{n}_min", "min": -0.6, "severity": "warning", "msg": f"{part}: undershoot under -0.6 V"}]
    json.dump(bd, open(f"tb_{name}.bounds.json", "w"), indent=1)
