"""R3-AMP placement check: distance from each amp's PVDD/BST/supply pin to
its capacitor's matching pad (centre to centre, mm), and each output filter
cap's distance to its inductor. Bounds from TI's layout notes: PVDD HF caps
"as close as possible" (SLASEN3B 12.1, SLOS708G 10.1), taken here as <= 3 mm
pad-to-pad for the 1 uF / 100 nF parts, BST caps <= 4 mm (SLASEN3B 11.1.4).
Exits 1 on a fail. python3 check_amp_layout.py"""
import math, os, sys
sys.path.insert(0, os.path.dirname(__file__))
import pcbq
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
fails = []
def chk(ok, msg):
    print(('PASS ' if ok else 'FAIL ') + msg)
    if not ok: fails.append(msg)

def near(fp, ic, icpins, cap, lim, label):
    best = None
    for p in fp[ic]['pads']:
        if p['num'] not in icpins: continue
        for q in fp[cap]['pads']:
            if q['net'] != p['net']: continue
            d = math.hypot(p['x'] - q['x'], p['y'] - q['y'])
            if best is None or d < best[0]: best = (d, p['num'], q['num'])
    if best is None:
        chk(False, f'{label}: {cap} shares no net with {ic} pins {icpins}'); return
    chk(best[0] <= lim, f'{label}: {cap} pad {best[2]} to {ic} pin {best[1]} {best[0]:.2f} mm (<= {lim})')

b = pcbq.footprints(pcbq.load(f'{ROOT}/boards/lite-main/kicad/lite-main.kicad_pcb'))
# Lite (TPA3118D2, 21 V on a 30 V part; TPA3118D2EVM SLOU342A fig 4 has its
# HF caps ~3-5 mm from the pins): 5 mm for PVDD/BST, 6 mm for GVDD/AVCC
for c in ('C52', 'C53', 'C54'): near(b, 'U8', {'18', '19', '17'}, c, 5.0, 'Lite PVDD left')
for c in ('C55', 'C56', 'C57'): near(b, 'U8', {'31', '32'}, c, 5.0, 'Lite PVDD right')
near(b, 'U8', {'17'}, 'C51', 6.0, 'Lite AVCC')
near(b, 'U8', {'7'}, 'C50', 6.0, 'Lite GVDD')
for c in ('C64', 'C65', 'C66', 'C67'): near(b, 'U8', {'20', '24', '26', '30'}, c, 5.0, 'Lite BST')
for c in ('C58', 'C59'): near(b, 'U8', {'17', '18', '19', '31', '32'}, c, 15.0, 'Lite PVDD bulk')

b = pcbq.footprints(pcbq.load(f'{ROOT}/boards/core/kicad/core.kicad_pcb'))
for u, base in (('U15', 200), ('U16', 300)):
    # C(base+7..10): 1 uF + 100 nF at pin 29 and at pin 36 (blocks_amp.py order)
    for c, pins in ((f'C{base+7}', {'29', '30', '31'}), (f'C{base+8}', {'29', '30', '31'}),
                    (f'C{base+9}', {'36', '37', '38'}), (f'C{base+10}', {'36', '37', '38'})):
        if c in b: near(b, u, pins, c, 3.0, f'{u} PVDD')
    # BST caps found by net (SLASEN3B fig 53: at the row ends, ~1-2 mm)
    for pin in ('43', '44', '23', '24'):
        net = [p['net'] for p in b[u]['pads'] if p['num'] == pin][0]
        caps = [r for r, f in b.items() if r.startswith('C') and any(q['net'] == net for q in f['pads'])]
        for c in caps: near(b, u, {pin}, c, 4.0, f'{u} BST pin {pin}')
    near(b, u, {'21'}, f'C{base+4}', 3.0, f'{u} AVDD')
    near(b, u, {'22'}, f'C{base+6}', 3.0, f'{u} GVDD')
    near(b, u, {'1'}, f'C{base+1}', 4.0, f'{u} VDD')
for c in ('C5', 'C6', 'C311'):
    for u in ('U15', 'U16'):
        d = min(math.hypot(p['x'] - q['x'], p['y'] - q['y']) for p in b[u]['pads'] if p['net'] == 'PVDD'
                for q in b[c]['pads'] if q['net'] == 'PVDD')
        print(f'INFO bulk {c} PVDD pad to {u} nearest PVDD pin: {d:.1f} mm')
print(f'\n{len(fails)} fail(s)')
sys.exit(1 if fails else 0)
