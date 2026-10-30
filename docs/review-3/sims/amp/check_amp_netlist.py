"""R3-AMP: pin-by-pin and value check of Lite U8 (TPA3118D2) and core U15/U16
(TPA3220) against TI SLOS708G / SLASEN3B / SLAU754A / SLOU342A. Read-only.
Exits 1 on any FAIL. Run: python3 check_amp_netlist.py"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import pcbq

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
fails = []

def load(path):
    t = pcbq.load(path)
    nets, vals = {}, {}
    for n in pcbq.find(pcbq.first(t, 'nets'), 'net'):
        name = pcbq.first(n, 'name')[1]
        for nd in pcbq.find(n, 'node'):
            nets[(pcbq.first(nd, 'ref')[1], pcbq.first(nd, 'pin')[1])] = name
    for c in pcbq.find(pcbq.first(t, 'components'), 'comp'):
        vals[pcbq.first(c, 'ref')[1]] = pcbq.first(c, 'value')[1]
    return nets, vals

def chk(cond, msg):
    print(('PASS ' if cond else 'FAIL ') + msg)
    if not cond: fails.append(msg)

def other(nets, net, ref):
    return sorted({r for (r, p), n in nets.items() if n == net and r != ref})

def two_terminal_between(nets, vals, a, b):
    """parts (ref, value) with one pin on net a and the other on net b"""
    out = []
    for ref in vals:
        ns = {nets.get((ref, '1')), nets.get((ref, '2'))}
        if ns == {a, b}: out.append((ref, vals[ref]))
    return out

# ---------------- Lite TPA3118D2 (SLOS708G pin table p3-4)
nets, vals = load(f'{ROOT}/boards/lite-main/kicad/lite-main.net')
U = 'U8'
p = lambda n: nets.get((U, str(n)))
chk(p(1) == 'GND', 'Lite MODSEL (1) = GND -> BD modulation')
chk(p(12) == 'GND', 'Lite MUTE (12) = GND (unmuted; SDZ is the only control)')
chk(all(p(n) == 'GND' for n in (13, 14, 15)), 'Lite AM2..AM0 = 000 -> 400 kHz (SLOS708G 6.6)')
chk(p(16) is None or p(16).startswith('unconnected'), 'Lite SYNC (16) open: output in master mode (GAIN/SLV master divider), valid')
chk(p(6) == p(7), 'Lite PLIMIT tied to GVDD (no limit)')
g = two_terminal_between(nets, vals, p(8), 'GND'); h = two_terminal_between(nets, vals, p(8), p(7))
chk(g and g[0][1] == '20k' and h and h[0][1] == '100k', f'Lite GAIN/SLV 20k to GND, 100k to GVDD = master 26 dB (table 1): {g} {h}')
for bs, out in ((20, 21), (24, 23), (26, 27), (30, 29)):
    c = two_terminal_between(nets, vals, p(bs), p(out))
    chk(len(c) == 1 and c[0][1] in ('220nF', '220n'), f'Lite BST pin {bs} -> OUT pin {out} 220 nF: {c}')
chk(all(p(n) == 'PVDD' for n in (17, 18, 19, 31, 32)), 'Lite AVCC+PVCC on PVDD')
for o in (21, 23, 27, 29):
    ls = [r for r in other(nets, p(o), U) if r.startswith('L')]
    chk(len(ls) == 1, f'Lite OUT pin {o} -> one inductor {ls}')
    snub = [r for r in other(nets, p(o), U) if r.startswith('R')]
    print(f'INFO Lite OUT pin {o}: pre-filter snubber parts {snub} (TPA3118D2EVM: 10R + 330pF per OUT)')
chk(p(2) == '/AMP_SDZ' and p(3) == '/AMP_FAULTZ', 'Lite SDZ/FAULTZ wired to firmware nets')

# ---------------- core TPA3220 (SLASEN3B pin table p3-4, fig 50, SLAU754A)
nets, vals = load(f'{ROOT}/boards/core/kicad/core.net')
for U, z, master in (('U15', 'A', True), ('U16', 'B', False)):
    p = lambda n: nets.get((U, str(n)))
    chk(p(1) == '+5V', f'{U} VDD = +5V (LDO bypass, 4.5-5.5 V)')
    chk(p(11) == 'GND', f'{U} HEAD = GND -> AD mode')
    for pin in (5, 6, 7, 18, 19, 20, 25, 26, 33, 34, 41, 42, 45):
        if p(pin) != 'GND': chk(False, f'{U} GND pin {pin}')
    chk(all(p(n) == 'PVDD' for n in (29, 30, 31, 36, 37, 38)), f'{U} PVDD pins')
    chk(p(39) == p(40) and p(27) == p(28), f'{U} doubled OUT pins joined')
    for bs, out in ((44, 39), (43, 35), (24, 32), (23, 27)):
        c = two_terminal_between(nets, vals, p(bs), p(out))
        chk(len(c) == 1 and c[0][1] == '33nF', f'{U} BST {bs} -> OUT {out} 33 nF: {c}')
    cm = [r for r in other(nets, p(17), U)]
    chk(len(cm) == 1 and vals[cm[0]] == '33nF', f'{U} CMUTE 33 nF only (no external driver): {cm}')
    gnd = two_terminal_between(nets, vals, p(2), 'GND'); av = two_terminal_between(nets, vals, p(2), p(21))
    want = ('20k', '100k') if master else ('75k', '47k')
    chk(gnd and av and gnd[0][1] == want[0] and av[0][1] == want[1],
        f'{U} GAIN/SLV {gnd}/{av} = {"master" if master else "slave"} 24 dB (SLASEN3B table 2)')
    if master:
        fr = two_terminal_between(nets, vals, p(14), 'GND')
        rp = 1 / sum(1 / float(v.rstrip('k')) for _, v in fr) if fr else 0
        chk(49.5 <= rp <= 50.5, f'{U} FREQ_ADJ {fr} = {rp:.1f}k -> 600 kHz (49.5-50.5k)')
        chk(two_terminal_between(nets, vals, p(12), p(21)) and two_terminal_between(nets, vals, p(13), 'GND'),
            f'{U} OSCM 47k to AVDD, OSCP 47k to GND (SLASEN3B 9.3.2)')
    else:
        chk(two_terminal_between(nets, vals, p(14), p(21)), f'{U} FREQ_ADJ to AVDD -> slave, oscillator off')
    chk(nets[('U15', '12')] == nets[('U16', '12')] and nets[('U15', '13')] == nets[('U16', '13')],
        'OSCM-OSCM, OSCP-OSCP (slave 1)')
    for o in (39, 35, 32, 27):
        ls = [r for r in other(nets, p(o), U) if r.startswith('L')]
        rs = [r for r in other(nets, p(o), U) if r.startswith('R')]
        chk(len(ls) == 1, f'{U} OUT {o} -> {ls}')
        spk = nets[(ls[0], '2')]
        sn = [r for r in other(nets, spk, None) if r.startswith('R')]
        print(f'INFO {U} OUT {o}: snubber R on switch node {rs}, on speaker node {spk}: {sn} '
              '(SLASEN3B fig 50: 1 uF + 1 nF + 3R3/10 nF after the inductor; EVM: 1 uF film only)')
    for pin in (3, 4):
        chk(p(pin) in ('/AMP_OTW', '/AMP_FAULT'), f'{U} pin {pin} -> {p(pin)}')
chk(nets[('U15', '10')] != nets[('U16', '10')], 'separate RESETs: firmware must release both in one expander write')

print(f'\n{len(fails)} fail(s)')
sys.exit(1 if fails else 0)
