#!/usr/bin/env python3
"""PAD (Lite power/amp/digital) netlist + firmware cross-check, review round 3 (docs/review-4).

Reads boards/lite-main/kicad/lite-main.net and .kicad_pcb (pad nets) and the Lite
firmware header; exits 1 on any FAIL. Run: python3 -I check_pad_nets.py [repo root]
"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import netlib

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(os.path.join(HERE, '../../../..'))
NET = os.path.join(ROOT, 'boards/lite-main/kicad/lite-main.net')
PCB = os.path.join(ROOT, 'boards/lite-main/kicad/lite-main.kicad_pcb')
HDR = os.path.join(ROOT, 'firmware/components/hifi/esp/hifi_board_lite.h')
PATCH = os.path.join(ROOT, 'firmware/patches/0001-hifi-receiver-hooks.patch')

comps, nets = netlib.load(NET)
pn = {k: netlib.short(v) for k, v in netlib.pin_net(nets).items()}
fails = []

def check(ok, msg):
    print(('PASS ' if ok else 'FAIL ') + msg)
    if not ok:
        fails.append(msg)

def pins(ref):
    return {p: v for (r, p), v in pn.items() if r == ref}

def expect(ref, want, why):
    got = pins(ref)
    bad = {p: (got.get(p), n) for p, n in want.items() if got.get(p) != n}
    check(not bad, f'{ref} {why}' + (f' -- got/want {bad}' if bad else ''))

def val(ref):
    return comps[ref]['value']

def members(net):
    return sorted({r for (r, p), v in pn.items() if v == net})

# ---- sub-out power-fail mute (R3-AUDIO-2 fix): SOT-23 1=B/G 2=E/S 3=C/D
expect('Q11', {'1': '+5V', '3': '+5V', '2': 'MUTE_RES'}, 'B-C to +5V, E to MUTE_RES (diode into the reservoir)')
expect('Q12', {'1': 'MUTE_DET', '2': 'GND', '3': 'SUB_MUTE'}, 'senses +5V, holds SUB_MUTE low')
expect('Q13', {'1': 'SUB_MUTE', '2': 'MUTE_S', '3': 'SUB_L'}, 'SUB_L mute FET')
expect('Q15', {'1': 'SUB_MUTE', '2': 'MUTE_S', '3': 'SUB_R'}, 'SUB_R mute FET')
expect('Q14', {'1': 'SUB_MUTE', '2': 'MUTE_S', '3': 'GND'}, 'ground-side mute FET')
expect('R80', {'1': '+5V', '2': 'MUTE_DET'}, 'sense divider top')
expect('R81', {'1': 'MUTE_DET', '2': 'GND'}, 'sense divider bottom')
expect('R82', {'1': 'MUTE_RES', '2': 'SUB_MUTE'}, 'mute pull-up from the reservoir')
expect('C90', {'1': 'MUTE_RES', '2': 'GND'}, 'mute reservoir')
check((val('R80'), val('R81'), val('R82'), val('C90')) == ('120k', '22k', '47k', '22uF'),
      f"mute values R80/R81/R82/C90 = {val('R80')}/{val('R81')}/{val('R82')}/{val('C90')}")
check(members('MUTE_RES') == ['C90', 'Q11', 'R82'], f"nothing else draws on MUTE_RES: {members('MUTE_RES')}")
check(members('MUTE_S') == ['Q13', 'Q14', 'Q15'], f"MUTE_S only the three FET sources: {members('MUTE_S')}")
check(set(members('SUB_L')) >= {'Q13', 'R36', 'R37', 'J3'}, f"SUB_L at the jack side of R36: {members('SUB_L')}")

# ---- PVDD switch and PG interlock
expect('Q1', {'1': 'VBUS', '2': 'VBUS', '3': 'VBUS', '4': 'PSW_G', '5': 'PVDD', '6': 'PVDD', '7': 'PVDD', '8': 'PVDD'},
       'AO4485 S=VBUS, D=PVDD')
expect('Q4', {'1': 'PSW_B', '2': 'PD_PG', '3': 'PSW_PULL'}, 'interlock: emitter on CH224A PG (AND with PVDD_EN)')
expect('R5', {'1': 'VBUS', '2': 'PSW_G'}, 'gate-source 100k')
expect('R6', {'1': 'PSW_G', '2': 'PSW_PULL'}, 'gate pull 220k')
check(val('R5') == '100k' and val('R6') == '220k', f"gate divider {val('R5')}/{val('R6')}: Vgs = VBUS x 0.3125")
check(set(members('PSW_G')) == {'Q1', 'R5', 'R6', 'R15', 'C19'}, f"PSW_G: {members('PSW_G')}")
expect('R8', {'1': 'PVDD_EN', '2': 'GND'}, 'PVDD_EN pull-down (off while the ESP32 boots)')
expect('U1', {'1': 'VBUS', '8': 'VBUS', '9': 'PD_CFG1', '10': 'PD_PG'}, 'CH224A VHV=VBUS=pin 8 (DS V2.0 table 4-1), CFG1, PG')
check(val('R1') == '120k', f"CH224A Rset {val('R1')} (120k = 20 V, table 5-1)")
check(set(members('PD_PG')) == {'U1', 'R4', 'Q4', 'U5'}, f"PD_PG: {members('PD_PG')}")

# ---- amp control
expect('R54', {'1': 'AMP_SDZ_IO', '2': 'AMP_SDZ'}, 'SDZ series')
expect('R52', {'1': 'AMP_SDZ_IO', '2': 'GND'}, 'SDZ pull-down: amp off at boot')
expect('U8', {'2': 'AMP_SDZ', '3': 'AMP_FAULTZ'}, 'TPA3118 SDZ/FAULTZ')

# ---- ESP32 GPIO map vs firmware header
gpio = {}
for name, nodes in nets.items():
    for ref, p, fn, ty in nodes:
        if ref == 'U5':
            m = re.match(r'IO(\d+)', fn or '')
            if m:
                gpio[int(m.group(1))] = netlib.short(name)
            elif (fn or '').startswith('SENSOR_VP'):
                gpio[36] = netlib.short(name)
            elif (fn or '').startswith('SENSOR_VN'):
                gpio[39] = netlib.short(name)
hdr = open(HDR).read()
def hdr_int(k):
    m = re.search(r'#define\s+%s\s+(\d+)' % k, hdr)
    return int(m.group(1)) if m else None
for k, net in (('PIN_PD_PG', 'PD_PG'), ('PIN_PVDD_EN', 'PVDD_EN'), ('PIN_AMP_SDZ', 'AMP_SDZ_IO'),
               ('PIN_AMP_FAULTZ', 'AMP_FAULTZ'), ('I2C_SDA', 'I2C_SDA'), ('I2C_SCL', 'I2C_SCL')):
    g = hdr_int(k)
    check(gpio.get(g) == net, f'firmware {k}={g} -> board {gpio.get(g)} (want {net})')
adc = {'ADC1_CHANNEL_6': 34, 'ADC1_CHANNEL_3': 39}
for k, net in (('ADC_VBUS', 'VBUS_SENSE'), ('ADC_PVDD', 'PVDD_SENSE')):
    ch = re.search(r'#define\s+%s\s+(ADC1_CHANNEL_\d)' % k, hdr).group(1)
    check(gpio.get(adc[ch]) == net, f'firmware {k}={ch} (GPIO{adc[ch]}) -> board {gpio.get(adc[ch])}')
for k, top, bot in (('VBUS_DIVIDER', 'R13', 'R14'), ('PVDD_DIVIDER', 'R17', 'R18')):
    d = float(re.search(r'#define\s+%s\s+([\d.]+)' % k, hdr).group(1))
    kv = lambda s: float(s.rstrip('k')) * 1e3
    real = (kv(val(top)) + kv(val(bot))) / kv(val(bot))
    check(abs(d - real) < 0.01, f'{k} {d} vs {top}/{bot} {val(top)}/{val(bot)} = {real:.2f}')
# squeezelite pins (patch Kconfig defaults for HIFI_LITE)
pt = open(PATCH).read()
m = re.search(r'model=ES8388,bck=(\d+),ws=(\d+),do=(\d+),di=(\d+),sda=(\d+),scl=(\d+),i2c=\d+,mck=(\d+)', pt)
if m:
    b, w, do, di, sda, scl, mck = map(int, m.groups())
    for g, net in ((b, 'I2S_BCLK'), (w, 'I2S_LRCK'), (do, 'I2S_DOUT'), (di, 'I2S_DIN'), (sda, 'I2C_SDA'),
                   (scl, 'I2C_SCL'), (mck, 'I2S_MCLK')):
        check(gpio.get(g) == net, f'squeezelite DAC_CONFIG GPIO{g} -> {gpio.get(g)} (want {net})')
for g, net in ((13, 'BTN_VOLUP'), (36, 'BTN_VOLDN'), (19, 'BTN_PLAY')):
    check(gpio.get(g) == net and f'"gpio\\":{g}' in pt, f'button GPIO{g} -> {gpio.get(g)} and in AUDIO_CONTROLS')
# straps: GPIO12 (MTDI) must not be pulled high (3.3 V flash on WROVER-E); GPIO0 pull-up; GPIO2/15/5 free
check(gpio.get(12, '').startswith('unconnected'), f"GPIO12 strap left to its pull-down: {gpio.get(12)}")
check(gpio.get(0) == 'I2S_MCLK' and set(members('I2S_MCLK')) >= {'R21', 'Q3', 'U7'}, 'GPIO0 = MCLK with R21 pull-up and Q3 auto-program')

# ---- auto-program (DevKitC: EN Q base<-DTR emitter<-RTS; IO0 Q base<-RTS emitter<-DTR)
expect('Q2', {'1': 'Q2_B', '2': 'RTS', '3': 'ESP_EN'}, 'auto-program EN')
expect('Q3', {'1': 'Q3_B', '2': 'DTR', '3': 'I2S_MCLK'}, 'auto-program IO0')
expect('R24', {'1': 'DTR', '2': 'Q2_B'}, 'Q2 base from DTR')
expect('R25', {'1': 'RTS', '2': 'Q3_B'}, 'Q3 base from RTS')

# ---- PCB: the mute's pads carry the same nets (routing parity for the new parts)
pcb = open(PCB).read()
for ref in ('Q11', 'Q12', 'Q13', 'Q14', 'Q15', 'C90', 'R80', 'R81', 'R82'):
    m = re.search(r'\(footprint\s+"[^"]+"(?:(?!\(footprint\s).)*?\(property\s+"Reference"\s+"%s"(.*?)(?=\n\t\(footprint\s|\Z)' % ref, pcb, re.S)
    if not m:
        check(False, f'{ref} on the PCB'); continue
    body = m.group(0)
    pads = dict(re.findall(r'\(pad\s+"(\d+)".*?\(net\s+(?:\d+\s+)?"([^"]*)"', body, re.S))
    want = pins(ref)
    bad = {p: (netlib.short(pads.get(p)), w) for p, w in want.items() if netlib.short(pads.get(p)) != w}
    check(not bad, f'PCB {ref} pad nets match the netlist' + (f' {bad}' if bad else ''))

print(f'\n{len(fails)} fail(s)')
sys.exit(1 if fails else 0)
