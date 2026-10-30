"""I2C bus load check (review-3 DIGITAL): device count, addresses, routed length
and rise time for each board's I2C bus, Ultra = core + FFC + ultra-front.
Exits 1 if the fast-mode (400 kHz) rise time (300 ns, 0.3->0.7 VDD) or the
3 mA sink limit fails, or if two parts share an address.
Inputs that are estimates are named below; the routed lengths are measured.
"""
import math, re, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import netlib
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))

def routed_len(pcb, names):
    t = open(pcb, encoding='utf-8').read()
    # net numbers -> names (KiCad 8/9 'net N "name"' or name-only)
    tot = {n: 0.0 for n in names}
    for m in re.finditer(r'\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\).*?\(net (?:\d+ )?"?([^")]*)"?\)', t, re.S):
        x1, y1, x2, y2, net = m.groups()
        short = net.rsplit('/', 1)[-1]
        if short in tot:
            tot[short] += math.hypot(float(x2) - float(x1), float(y2) - float(y1))
    return tot

# per-pin input capacitance, pF (datasheet Ci/Cio; ESP32 not specced, 5 pF taken)
CIN = {'ESP32': 5, 'TCA9539': 7, 'TCA9535': 7, 'ES9018K2M': 5, 'ES7243E': 5,
       'CH224': 5, 'ES8388': 5}
PF_PER_MM_PCB = 0.12   # ~0.1-0.15 pF/mm for a 0.2 mm trace over a plane
PF_PER_MM_FFC = 0.08   # 0.5 mm pitch FFC, signal beside GND: ~0.6-1 pF/cm
FFC_MM = 150           # estimate: core J3 to front J1 (enclosure not drawn)
VDD, RPU = 3.3, 2200.0

def bus(label, parts, lengths_mm, ffc_mm=0):
    c_dev = sum(CIN[p] for p in parts)
    c_tr = sum(lengths_mm) * PF_PER_MM_PCB + ffc_mm * PF_PER_MM_FFC
    c = c_dev + c_tr
    tr = 0.8473 * RPU * c * 1e-12 * 1e9
    isink = (VDD - 0.4) / RPU * 1e3
    ok = tr <= 300 and isink <= 3.0
    print(f"{label}: parts {len(parts)}, C_dev {c_dev} pF, C_trace {c_tr:.0f} pF, "
          f"C_bus {c:.0f} pF, tr {tr:.0f} ns (limit 300), Isink {isink:.2f} mA (limit 3) -> {'pass' if ok else 'FAIL'}")
    return ok, c

ok = True
lite = routed_len(f"{ROOT}/boards/lite-main/kicad/lite-main.kicad_pcb", ['I2C_SCL', 'I2C_SDA', 'PD_SCL', 'PD_SDA'])
core = routed_len(f"{ROOT}/boards/core/kicad/core.kicad_pcb", ['I2C_SCL', 'I2C_SDA'])
front = routed_len(f"{ROOT}/boards/ultra-front/kicad/ultra-front.kicad_pcb", ['I2C_SCL', 'I2C_SDA'])
print('routed mm', {k: round(v, 1) for k, v in {**{'lite_' + k: v for k, v in lite.items()}, **{'core_' + k: v for k, v in core.items()}, **{'front_' + k: v for k, v in front.items()}}.items()})
r, _ = bus('lite SCL', ['ESP32', 'ES8388', 'CH224'], [lite['I2C_SCL'], lite['PD_SCL']]); ok &= r
r, _ = bus('core (Plus) SCL', ['ESP32', 'TCA9539', 'ES9018K2M', 'ES7243E', 'CH224'], [core['I2C_SCL']]); ok &= r
r, _ = bus('core+front (Ultra) SCL', ['ESP32', 'TCA9539', 'ES9018K2M', 'ES7243E', 'CH224', 'TCA9535'], [core['I2C_SCL'], front['I2C_SCL']], FFC_MM); ok &= r
r, _ = bus('core+front (Ultra) SDA', ['ESP32', 'TCA9539', 'ES9018K2M', 'ES7243E', 'CH224', 'TCA9535'], [core['I2C_SDA'], front['I2C_SDA']], FFC_MM); ok &= r

# addresses (7-bit), from the netlist straps / datasheets
ADDR = {'lite-main': {'ES8388 (CE=GND)': 0x10, 'CH224A': 0x22},
        'ultra': {'CH224Q': 0x22, 'ES7243E (AD2..0=0)': 0x10, 'ES9018K2M': 0x48,
                  'TCA9539 (A1A0=00)': 0x74, 'TCA9535 front (A2..0=111)': 0x27}}
# verify the straps the addresses rest on
_, ln = netlib.load(f"{ROOT}/boards/lite-main/kicad/lite-main.net")
pn = netlib.pin_net(ln)
assert netlib.short(pn[('U7', '26')]) == 'GND', 'ES8388 CE not on GND'
_, cn = netlib.load(f"{ROOT}/boards/core/kicad/core.net")
pc = netlib.pin_net(cn)
assert netlib.short(pc[('U17', '21')]) == 'GND' and netlib.short(pc[('U17', '2')]) == 'GND', 'TCA9539 A0/A1'
_, fn = netlib.load(f"{ROOT}/boards/ultra-front/kicad/ultra-front.net")
pf = netlib.pin_net(fn)
assert all(netlib.short(pf[('U1', p)]) == '+3V3' for p in ('2', '3', '21')), 'TCA9535 A0-A2'
for b, m in ADDR.items():
    vals = list(m.values())
    dup = len(vals) != len(set(vals))
    print(b, {k: hex(v) for k, v in m.items()}, 'DUPLICATE' if dup else 'unique')
    ok &= not dup
sys.exit(0 if ok else 1)
