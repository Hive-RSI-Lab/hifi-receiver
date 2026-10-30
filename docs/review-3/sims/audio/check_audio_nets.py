#!/usr/bin/env python3
"""AUDIO round-2 netlist rules + level budget (read-only on boards/).

Reads the KiCad netlists (.net) and the PCB (.kicad_pcb footprint values/nets) for
both boards, checks the small-signal chain's connections and values, and computes
the end-to-end levels from the values it finds. Exit 1 on any FAIL.
    python3 -I docs/review-3/sims/audio/check_audio_nets.py
"""
import math, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
FAILS, WARNS = [], []

def parse(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    st = [[]]
    for t in tok:
        if t == '(':
            st.append([])
        elif t == ')':
            x = st.pop(); st[-1].append(x)
        else:
            st[-1].append(t[1:-1] if t.startswith('"') else t)
    return st[0][0]

def kids(n, k): return [c for c in n if isinstance(c, list) and c and c[0] == k]
def kid(n, k):
    r = kids(n, k); return r[0] if r else None

def load_pcb(path):
    """ref -> (value, {pad: net}) from the board file (what is fabricated)."""
    b = parse(path.read_text(encoding='utf-8'))
    out = {}
    for fp in kids(b, 'footprint'):
        ref = val = None
        for p in kids(fp, 'property'):
            if p[1] == 'Reference': ref = p[2]
            if p[1] == 'Value': val = p[2]
        pads = {}
        for pd in kids(fp, 'pad'):
            net = kid(pd, 'net')
            if net and len(net) > 1:
                pads[pd[1]] = net[-1] if not net[-1].isdigit() else net[1]
        dnp = any(a == 'dnp' for a in (kid(fp, 'attr') or []))
        out[ref] = (val, pads, dnp)
    return out

def load_net(path):
    t = parse(path.read_text(encoding='utf-8'))
    comps = {kid(c, 'ref')[1]: kid(c, 'value')[1] for c in kids(kid(t, 'components'), 'comp')}
    nets = {}
    for n in kids(kid(t, 'nets'), 'net'):
        nets[kid(n, 'name')[1]] = {(kid(d, 'ref')[1], kid(d, 'pin')[1]) for d in kids(n, 'node')}
    return comps, nets

def val(s):
    s = s.replace('Ω', '').replace('R', '').strip()
    m = re.match(r'([\d.]+)\s*([pnumkM]?)', s)
    mult = {'p': 1e-12, 'n': 1e-9, 'u': 1e-6, 'm': 1e-3, 'k': 1e3, 'M': 1e6, '': 1}
    return float(m.group(1)) * mult[m.group(2)]

def check(cond, msg, warn=False):
    tag = 'PASS' if cond else ('WARN' if warn else 'FAIL')
    print(f'{tag}: {msg}')
    if not cond: (WARNS if warn else FAILS).append(msg)

def nets_of(pcb, ref):
    return set(n.lstrip('/').split('/')[-1] for n in pcb[ref][1].values())

def par(*r): return 1 / sum(1 / x for x in r)

# ------------------------------------------------------------------ core
core = load_pcb(ROOT / 'boards/core/kicad/core.kicad_pcb')
cv = lambda r: val(core[r][0])
print('== core ==')
for r, v in (('R73', 1.3e3), ('R75', 1.3e3), ('R81', 1.3e3), ('R83', 1.3e3), ('R72', 2e3), ('R74', 2e3),
             ('R80', 2e3), ('R82', 2e3), ('C101', 4.7e-6), ('C102', 4.7e-6), ('C111', 4.7e-6), ('C112', 4.7e-6)):
    check(abs(cv(r) - v) / v < 0.01, f'{r} = {core[r][0]} (expected {v:g}; round-1 AUDIO-1/5)')
check('49.152' in core['Y1'][0], f'Y1 = {core["Y1"][0]} (round-1 AUDIO-10: 49.152 MHz)')
check(cv('C56') >= 2.2e-6, f'ES9018K2M DVDD cap C56 {core["C56"][0]} >= 2.2 uF (datasheet v3.7 p.7)')
check({'VREF'} & nets_of(core, 'R73') and {'OPL_P'} & nets_of(core, 'R73'), 'R73 OPL_P-VREF')
# I/V stage levels (ES9018K2M v3.7 p.31: each leg 3.05 Vpp around AVCC/2, 806R)
v5a, avcc = 5.1, 3.3
r1, r2 = cv('R72') + 806, cv('R73')
g = r2 / r1
vref = v5a * cv('R91') / (cv('R90') + cv('R91'))
ileg = (vref - avcc / 2) / (r1 + r2)
vref -= 2 * ileg * par(cv('R90'), cv('R91'))
vout_pk = 2 * 1.525 * g
cm_hi = vref * r1 / (r1 + r2) + (avcc / 2 + 1.525) * r2 / (r1 + r2)
print(f'   VREF {vref:.2f} V, gain {g:.3f}, 0 dBFS MAIN = {vout_pk/math.sqrt(2):.3f} Vrms ({vout_pk:.2f} Vpk)')
check(vref + vout_pk <= v5a - 0.8 - 0.2 and vref - vout_pk >= 0.8 + 0.2, f'OPA1678 output {vref-vout_pk:.2f}..{vref+vout_pk:.2f} V inside 0.8..{v5a-0.8:.1f} V with 0.2 V margin')
check(cm_hi <= v5a - 2 - 0.2, f'OPA1678 + input peak {cm_hi:.2f} V <= V+ - 2 V - 0.2 V margin ({v5a-2.2:.2f})')
# VREF corner (R3-AUDIO-1)
fc = 1 / (2 * math.pi * par(cv('R90'), cv('R91')) * cv('C64'))
check(fc <= 10, f'VREF filter corner {fc:.1f} Hz <= 10 Hz (rail noise reaches MAIN at gain 1; R3-AUDIO-1)')
# amp: TPA3220 24 dB = 15.85, Rin 24k (SLASEN3B table 3); PVDD 28 V 4 ohm clip ~26.5 Vpk
spk = vout_pk / math.sqrt(2) * 15.85
print(f'   DAC 0 dBFS -> speaker {spk:.1f} Vrms ({spk*math.sqrt(2):.1f} Vpk) vs ~26.5 Vpk clip at 28 V / 4 ohm')
check(spk * math.sqrt(2) < 26.5, 'DAC path at 0 dBFS stays under clip at 28 V')
# DJ-direct / ADC (FLX4 2.1 Vrms, 100R source)
rth = par(cv('R100'), cv('R101'))
vth = 2.1 * cv('R101') / (cv('R100') + cv('R101') + 100)
adc = vth * par(100e3, 8e3) / (par(100e3, 8e3) + rth)
dj = vth * par(100e3, 8e3, 24e3) / (par(100e3, 8e3, 24e3) + rth)
print(f'   FLX4 2.1 Vrms: ADC {adc:.3f} Vrms ({20*math.log10(adc):.1f} dBFS, ES7243E FS 1 Vrms), DJ-direct amp in {dj:.3f} Vrms -> {dj*15.85:.1f} Vrms')
check(adc < 0.89, 'ES7243E input under -1 dBFS at FLX4 2.1 Vrms')
check(any(n in ('LIN_L', 'LIN_R') for r in core for n in nets_of(core, r)
              if r.startswith('C') and 'GND' in nets_of(core, r) and len(nets_of(core, r)) == 2),
      'core RCA line in has an RF shunt cap at J5 like Lite C42/C45 100 pF (R3-AUDIO-6)', warn=True)
# taps (loaded ratio)
rt = par(cv('R130'), cv('R131'))
zl = par(par(cv('R132'), cv('R149')), 8e3)
ratio = cv('R131') / (cv('R130') + cv('R131')) * zl / (zl + rt)
print(f'   tap ratio A_SPK_L+ -> ADC {ratio:.4f} (firmware-requirements C6 says 0.0276)')
check(abs(ratio - 0.0276) / 0.0276 < 0.05, 'tap ratio matches firmware-requirements C6')
# PCM5102A decoupling vs TI typical application (10 uF + 0.1 uF per supply)
for c in ('C68', 'C69', 'C70'):
    check(cv(c) >= 10e-6, f'PCM5102A supply cap {c} {core[c][0]} vs TI SLAS859C fig. (10 uF + 0.1 uF)', warn=True)
# +3V3A total cap on LP5907 (<= 10 uF)
tot = sum(cv(r) for r in core if r.startswith('C') and not core[r][2] and nets_of(core, r) == {'+3V3A', 'GND'})
check(tot <= 10e-6 * 1.05, f'core +3V3A caps {tot*1e6:.1f} uF <= LP5907 10 uF max')

# ------------------------------------------------------------------ lite
print('== lite-main ==')
lite = load_pcb(ROOT / 'boards/lite-main/kicad/lite-main.kicad_pcb')
lv = lambda r: val(lite[r][0])
for r, v in (('R30', 15e3), ('R33', 15e3), ('R31', 10e3), ('R34', 10e3), ('R54', 10e3)):
    check(abs(lv(r) - v) / v < 0.01, f'{r} = {lite[r][0]} (expected {v:g}; round-1 AUDIO-7/AUDIO-4)')
pin = 2.1 * par(lv('R31'), 20e3) / (par(lv('R31'), 20e3) + lv('R30'))
print(f'   FLX4 2.1 Vrms at LIN1 {pin:.3f} Vrms ({20*math.log10(pin):.1f} dBFS, ES8388 FS 1 Vrms, Rin 20k)')
dj_fw = pin * 10 ** (-3 / 20) * 10 ** (-4.5 / 20) * 20
clip = 19 / math.sqrt(2)
print(f'   DJ-direct with firmware caps (LI2LOVOL -3 dB, LOUT1 -4.5 dB): {dj_fw:.1f} Vrms vs clip {clip:.1f} Vrms ({20*math.log10(clip/dj_fw):.1f} dB short)')
check(20 * math.log10(clip / dj_fw) < 3, 'Lite DJ-direct reaches within 3 dB of clip (R3-AUDIO-3)', warn=True)
tl = lv('R67') / (lv('R66') + lv('R67')) if 'R67' in lite else 0
zl = par(lv('R67'), 20e3)
tr = zl / (zl + lv('R66'))
print(f'   Lite tap ratio loaded {tr:.4f} (firmware TAP_DIVIDER 11 -> {1/11:.4f})')
# sub out: DC on C47 at VMID 1.65 V, no mute (R3-AUDIO-2)
mute = [r for r in lite if r.startswith('Q') and {'SUB_L', 'SUB_R'} & nets_of(lite, r)]
check(bool(mute), 'Lite sub out has a mute/shunt on SUB_L/SUB_R (C47/C48 hold VMID 1.65 V: R3-AUDIO-2)')
tot = sum(lv(r) for r in lite if r.startswith('C') and nets_of(lite, r) == {'+3V3A', 'GND'})
check(tot <= 10e-6 * 1.05, f'Lite +3V3A caps {tot*1e6:.1f} uF <= LP5907 10 uF max')

print(f'\n{len(FAILS)} FAIL, {len(WARNS)} WARN')
sys.exit(1 if FAILS else 0)
