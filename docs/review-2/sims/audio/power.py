"""Output power at the actual PVDD (TI SLOS708G eq. 2 / same model for TPA3221).
P_unclipped = (RL/(RL+2RS) * Vp)^2 / (2 RL); P(10 %) = 1.25 x P(1 %)."""
cases = {
    # name: PVDD, RS per leg (FET HS or LS + inductor DCR + 10 mohm copper), gain V/V
    "lite TPA3118 20V": (20.0, 0.120 + 0.030 + 0.010, 10 ** (26 / 20)),
    "core TPA3221 28V": (28.0, 0.070 + 0.030 + 0.010, 10 ** (24 / 20)),
    "core TPA3221 20V (PD fallback)": (20.0, 0.070 + 0.030 + 0.010, 10 ** (24 / 20)),
}
# TPA3221 calibration: datasheet 88 W @ 30 V, 4 ohm, 1 % -> Vpk = sqrt(2*4*88) = 26.5 V = 0.885 x PVDD
for name, (pv, rs, g) in cases.items():
    for rl in (4, 6, 8):
        vp = rl / (rl + 2 * rs) * pv * 0.95          # 0.95 = max modulation depth (assumed)
        p1 = vp ** 2 / (2 * rl)
        print(f"{name:32s} RL={rl}  Vpk={vp:5.2f} V  P1%={p1:5.1f} W  P10%={1.25*p1:5.1f} W  Ipk={vp/rl:4.2f} A  Vin_clip={vp/g/2**0.5:5.3f} Vrms")
print("check TPA3221 @30V 4ohm model:", round((4/(4+2*0.11)*30*0.95)**2/8,1), "W vs 88 W datasheet")
