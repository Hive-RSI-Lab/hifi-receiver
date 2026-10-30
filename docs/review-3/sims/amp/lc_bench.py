"""R3-AMP output-stage bench (numbers, not SPICE): per board, per load and
per corner, the LC filter response, the inductor's peak current against its
saturation, ripple at idle, DCR heating and open-load peaking. Exits 1 on a
FAIL. Inputs are the BOM parts' own datasheet figures (cited inline).

  eval "$(/hive/tools/hwde/bin/hwde env)"; $HWDE_PY lc_bench.py   (needs numpy)
"""
import math, sys
import numpy as np

fails = []
def chk(ok, msg):
    print(('PASS ' if ok else 'FAIL ') + msg)
    if not ok: fails.append(msg)

def L_at(I, L0, isat):
    # molded alloy-powder chokes roll off softly; both datasheets define Isat
    # as the DC current for ~30 % drop. Quadratic roll-off through that point
    # (conservative below Isat), linear-extrapolated beyond it.
    x = I / isat
    return L0 * (1 - 0.30 * x * x) if x <= 1 else L0 * max(0.05, 0.70 - 0.60 * (x - 1))

BOARDS = {
  # Lite: TPA3118D2 (SLOS708G): PVDD 20 V (CH224A 20 V PDO, +5 % = 21 V),
  # rDS(on) 120 mOhm "pin to pin" (6.5) taken per H-bridge path; OC 7.5 A (6.6);
  # BD modulation at 400 kHz (AM2-0 = 000). L = XR0850-100M (C48391614):
  # 10 uH +/-20 %, DCR 30 typ / 40 max mOhm, Isat 8 A (-30 %), Irms 6.5 A (dT 40 C).
  # C = CL31B105KBHNNNE 1 uF 50 V X7R 1206 at PVDD/2 = 10 V: ~85 %.
  'lite': dict(pvdd=21.0, rds=0.12, ioc=7.5, fsw=400e3, mode='BD', L0=10e-6, ltol=0.2,
               dcr=0.040, isat=8.0, irms=6.5, C=1e-6, cder=0.85, lmin_ioc=1e-6),
  # core: TPA3220 (SLASEN3B): PVDD 28 V EPR (+5 % = 29.4 V), RDS(on) 70+70 mOhm
  # per half-bridge path at 25 C (x1.5 hot), IOC 10 A typ (no max), AD at 600 kHz.
  # L = PSPMAA0805-100M-ANP (C2962885): 10 uH +/-20 %, DCR 30/35 mOhm, Isat 10 A
  # (-30 %), Irms 6 A (dT 40 C). TI: >= 5 uH at IOC (SLASEN3B 7.3, SLAU754A 3.7).
  # C = CL21B105KBFNNNE 1 uF 50 V X7R 0805 at 14 V: 0.6 (round-1 figure; hwde
  # mlcc_dcbias generic curve says 0.86).
  'core': dict(pvdd=29.4, rds=0.105 * 2, ioc=10.0, fsw=600e3, mode='AD', L0=10e-6, ltol=0.2,
               dcr=0.035, isat=10.0, irms=6.0, C=1e-6, cder=0.60, lmin_ioc=5e-6),
}
LOADS = [2.7, 3.2, 4.0, 6.0, 8.0, 1e6]

for b, p in BOARDS.items():
    print(f'\n===== {b}')
    Lmin = p['L0'] * (1 - p['ltol'])
    # 1) short / overload: inductance left at the amp's current limit
    Lioc = L_at(p['ioc'], Lmin, p['isat'])
    ok = Lioc >= p['lmin_ioc']
    chk(ok, f'{b}: L at IOC {p["ioc"]} A, -20 % part = {Lioc*1e6:.2f} uH (TI minimum {p["lmin_ioc"]*1e6:.0f} uH); '
            f'margin {100*(Lioc/p["lmin_ioc"]-1):.0f} %')
    # 2) idle ripple per leg
    D = 0.5
    dI = p['pvdd'] * D * (1 - D) / (Lmin * p['fsw'])
    Ceff = p['C'] * p['cder']
    dV = dI / (8 * p['fsw'] * Ceff)
    kind = 'differential (AD: legs complementary)' if p['mode'] == 'AD' else 'common-mode (BD: legs in phase)'
    print(f'INFO {b}: idle ripple {dI:.2f} App per inductor, {dV:.2f} Vpp per leg, {kind}')
    for RL in LOADS:
        # 3) full-power sine peak current through the filter
        rpath = p['rds'] + 2 * p['dcr'] if RL < 1e5 else 0
        ipk = p['pvdd'] / (RL + rpath) if RL < 1e5 else 0
        ipk_tot = min(ipk, p['ioc']) + dI / 2
        L_pk = L_at(ipk_tot, Lmin, p['isat'])
        irms = ipk / math.sqrt(2)
        pdcr = irms ** 2 * p['dcr']
        # 4) small-signal response with derated parts, legs to GND, RL across
        f = np.logspace(2, 6.3, 4000); w = 2 * np.pi * f
        L = L_at(0, Lmin, p['isat'])
        Zl = 1j * w * L + p['dcr'] + p['rds'] / 2
        Zc = 1 / (1j * w * Ceff) + 0.005
        # differential half-circuit: source V/2 -> Zl -> node; node to GND: Zc || RL/2
        Zsh = 1 / (1 / Zc + 1 / (RL / 2))
        H = Zsh / (Zl + Zsh)
        hdb = 20 * np.log10(abs(H))
        f0 = 1 / (2 * math.pi * math.sqrt(L * Ceff))
        h20 = float(np.interp(20e3, f, hdb)); hpk = float(hdb.max()); hfs = float(np.interp(p['fsw'], f, hdb))
        name = f'{RL:g} ohm' if RL < 1e5 else 'open'
        print(f'      {name:>7}: Ipk {ipk:.2f} A (+ripple {ipk_tot:.2f} A), L there {L_pk*1e6:.1f} uH, '
              f'Irms {irms:.2f} A, DCR loss {pdcr:.2f} W/inductor | f0 {f0/1e3:.0f} kHz, '
              f'20 kHz {h20:+.1f} dB, peak {hpk:+.1f} dB, at fsw {hfs:.1f} dB')
        if RL < 3.2 and b == 'lite':
            print('      (below the TPA3118D2 3.2 ohm minimum load: not checked)')
        elif RL < 3.2:
            # TPA3220 allows 2.7 ohm; CB3C caps the current at IOC, so the
            # TI criterion (>= 5 uH at the current) is the test, not Isat
            chk(L_pk >= p['lmin_ioc'], f'{b} {name}: L at {ipk_tot:.2f} A = {L_pk*1e6:.2f} uH >= {p["lmin_ioc"]*1e6:.0f} uH')
        elif RL < 1e5:
            chk(ipk_tot < p['isat'], f'{b} {name}: peak {ipk_tot:.2f} A under Isat {p["isat"]} A')
            chk(irms <= p['irms'], f'{b} {name}: sine Irms {irms:.2f} A within rated {p["irms"]} A')
            if RL >= 4: chk(abs(h20) <= 3.0, f'{b} {name}: 20 kHz within +/-3 dB ({h20:+.1f})')
        else:
            # open load: circulating current at f0 per volt of drive at f0
            Zres = abs(Zl[np.argmin(abs(f - f0))] + Zc[np.argmin(abs(f - f0))])
            print(f'      open-load Q peak {hpk:+.1f} dB; 1 Vrms of drive at f0 circulates {1/Zres:.1f} Arms '
                  f'(only DCR+ESR+RDS limit it)')
print(f'\n{len(fails)} fail(s)')
sys.exit(1 if fails else 0)
