"""Lite-main I2S_BCLK bench (same pattern as core_i2sbck/gen.py). Geometry from
netpath.py on lite-main.kicad_pcb: ESP32-WROVER U5 pin 12 (IO27) -> U7 ES8388
SCLK pin 5, one track 45.6 mm (9.9 mm F.Cu + 35.7 mm B.Cu, 0.2 mm wide, 2 vias),
NO series resistor, no stub. Receiver is an ES8388 input (CMOS, DVDD 3.3 V),
thresholds 0.7/0.3 VDD = 2.31/0.99 V (datasheet VIH/VIL not re-read; assumption).
ASSUMPTIONS: 2-layer 1.6 mm board, 1.53 mm core, 0.2 mm track -> Hammerstad
microstrip over the opposite layer is ~145 ohm, 5.7 ps/mm; but the opposite layer is
a partial pour not a plane, so bench Z0 100 and 145 ohm (bracket). ESP32 driver
30/45 ohm behind a 1.5/2 ns (10-90 label, ramp 1.25x) edge, 5 pF receiver pin.
3.072 MHz, one edge each way 40 ns apart. fix_* = add a 33 ohm series R at U5."""
import json
PS = 5.7e-12; LEN = 45.6
cases = {}
for z in (100, 145):
    for tr, rd in ((2.0, 30), (1.5, 30), (1.5, 45), (1.0, 30)):
        cases[f"z{z}_tr{str(tr).replace('.','p')}_r{rd}"] = (z, tr, rd, False)
    cases[f"z{z}_fix_r33_tr1p0"] = (z, 1.0, 30, True)
for name, (z, tr, rd, fix) in cases.items():
    t0 = 5e-9; e = tr * 1.25e-9
    c = f"* lite I2S_BCLK bench {name}\nVs s 0 PULSE(0 3.3 {t0} {e} {e} {40e-9 - e} 80e-9)\nRd s d {rd}\nCd d 0 3p\n"
    src = "d"
    if fix: c += "Rser d d2 33\n"; src = "d2"
    c += f"Ta {src} 0 es 0 Z0={z} TD={LEN*PS:.4e}\nCes es 0 5p\n.tran 20p 90n 0 20p\n"
    c += f".measure tran es_min_hi MIN v(es) FROM={t0+e+2.5e-9:.3e} TO=45n\n"
    c += f".measure tran es_max_lo MAX v(es) FROM={t0+40e-9+e+2.5e-9:.3e} TO=85n\n"
    c += ".measure tran es_max MAX v(es)\n.measure tran es_min MIN v(es)\n.end\n"
    open(f"tb_{name}.cir", "w").write(c)
    json.dump([{"measure": "es_min_hi", "min": 2.31, "severity": "error", "msg": "ES8388 SCLK ring-back under 0.7 VDD after rising edge"},
               {"measure": "es_max_lo", "max": 0.99, "severity": "error", "msg": "ES8388 SCLK ring-back over 0.3 VDD after falling edge"},
               {"measure": "es_max", "max": 3.9, "severity": "warning", "msg": "overshoot over VDD+0.6"},
               {"measure": "es_min", "min": -0.6, "severity": "warning", "msg": "undershoot under -0.6 V"}],
              open(f"tb_{name}.bounds.json", "w"), indent=1)
