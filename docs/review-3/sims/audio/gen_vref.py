"""Generate core VREF benches: rail dip -> MAIN_L -> TPA3220 (24 dB) speaker voltage,
and L->R crosstalk through the shared VREF. Values from boards/core/kicad/gen/blocks_audio.py
(R72-R75/R80-R83, C142-C151, R90/R91/C64), FB1+C21 (blocks.py), TPA3220 Rin 24k, 10 uF in caps
(blocks_amp.py, SLASEN3B table 3). Ideal op-amp (Aol 1e6), ES9018K2M legs = 1.65 V + 806R
(ES9018K2M datasheet v3.7 p.31 note 1).
Re-run: python3 gen_vref.py && sim_run.py --dir .
"""
import json
VARIANTS = {
    # name: (C64, prefilter R, prefilter C)  prefilter=None -> as built
    "asbuilt": ("10u", None, None),
    "fix_c100u": ("100u", None, None),
    "fix_rc": ("10u", "47", "220u"),   # +5VA -47R- VREF_IN (220uF) -> R90/R91 divider, C64 10 uF
    "fix_inm_vref": ("10u", None, None),  # amp IN_M caps to VREF instead of GND (DAC path)
    "fix_inm_vref_c100u": ("100u", None, None),
}
DIP = "PWL(0 5.1 50m 5.1 50.01m 5.09 51m 5.09 51.01m 5.1 1 5.1)"   # 10 mV, 1 ms rail dip

def body(c64, rp, cp, inm_ref="0"):
    pre = (f"Rpre a5 vin {rp}\nCpre vin 0 {cp}\nR90 vin vref 470\n") if rp else "R90 a5 vref 470\n"
    return f"""Vrail p5 0 {DIP}
* FB1 120R@100MHz ferrite ~ 0.05 ohm DC, C21 10 uF
RFB p5 a5 0.05
C21 a5 0 10u
{pre}R91 vref 0 470
C64 vref 0 {c64}
* left channel: DAC legs (P = DACL, N = DACLB), 806R internal
VDLP dlp 0 DC 1.65 AC 0
VDLN dln 0 DC 1.65 AC 0
RlP dlp dlp2 806
RlN dln dln2 806
R72 dln2 opl_p 2k
R73 opl_p vref 1.3k
C142 opl_p vref 1n
R74 dlp2 opl_n 2k
R75 opl_n main_l 1.3k
C143 opl_n main_l 1n
Eop_l main_l 0 opl_p opl_n 1e6
* right channel, driven for crosstalk
VDRP drp 0 DC 1.65 SIN(1.65 0 1k) AC 0.5
VDRN drn 0 DC 1.65 AC 0.5 180
RrP drp drp2 806
RrN drn drn2 806
R80 drn2 opr_p 2k
R81 opr_p vref 1.3k
C150 opr_p vref 1n
R82 drp2 opr_n 2k
R83 opr_n main_r 1.3k
C151 opr_n main_r 1n
Eop_r main_r 0 opr_p opr_n 1e6
* TPA3220 zone A, 24 dB, Rin 24k per pin, single-ended: IN_M 10 uF to GND
Cin1 main_l inp 10u
Rinp inp 0 24k
Cin2 inm {inm_ref} 10u
Rinm inm 0 24k
Espk spk 0 inp inm 15.85
"""

for name, (c64, rp, cp) in VARIANTS.items():
    inm = "vref" if "inm_vref" in name else "0"
    bd = lambda a, b, c: body(a, b, c, inm)
    # transient: R channel at DC (set SIN amp 0 for the dip bench)
    tr = bd(c64, rp, cp).replace("SIN(1.65 0 1k)", "")
    tr = f"* tb_vref_dip_{name}: 10 mV 1 ms dip on +5V -> left speaker\n" + tr + \
        ".tran 20u 0.4 0 20u\n.measure tran spk_pk MAX v(spk) FROM=0.045 TO=0.4\n" \
        ".measure tran spk_min MIN v(spk) FROM=0.045 TO=0.4\n" \
        ".measure tran spk_pp PARAM='(spk_pk-spk_min)*1000'\n.end\n"
    open(f"tb_vref_dip_{name}.cir", "w").write(tr)
    json.dump([{"measure": "spk_pp", "max": 2.0, "severity": "error",
                "msg": "speaker tick (mVpp) from a 10 mV, 1 ms +5V dip; 2 mV ~ 20x the TPA3220's 72-100 uV A-wtd floor"}],
              open(f"tb_vref_dip_{name}.bounds.json", "w"), indent=1)
    ac = f"* tb_vref_xtalk_{name}: R driven 1 Vpk diff-leg AC, L output\n" + bd(c64, rp, cp).replace("PWL", "DC 5.1 ;").split(";")[0] + "\n" + \
        bd(c64, rp, cp).split("\n", 1)[1] + \
        ".ac dec 50 10 30k\n.measure ac xt100 FIND vdb(main_l) AT=100\n.measure ac xt1k FIND vdb(main_l) AT=1k\n" \
        ".measure ac ref1k FIND vdb(main_r) AT=1k\n.measure ac sep1k PARAM='ref1k-xt1k'\n" \
        ".measure ac ref100 FIND vdb(main_r) AT=100\n.measure ac sep100 PARAM='ref100-xt100'\n" \
        ".measure ac spk1k FIND vdb(spk) AT=1k\n.measure ac spk100 FIND vdb(spk) AT=100\n" \
        ".measure ac sepspk1k PARAM='ref1k+24-spk1k'\n.measure ac sepspk100 PARAM='ref100+24-spk100'\n.end\n"
    open(f"tb_vref_xtalk_{name}.cir", "w").write(ac)
    json.dump([{"measure": "sepspk1k", "min": 60.0, "severity": "warning", "msg": "L/R separation at the speaker, 1 kHz (dB)"},
               {"measure": "sepspk100", "min": 40.0, "severity": "warning", "msg": "L/R separation at the speaker, 100 Hz (dB)"},
               {"measure": "sep1k", "min": 0.0, "severity": "warning", "msg": "L/R separation at 1 kHz (dB)"},
               {"measure": "sep100", "min": 0.0, "severity": "warning", "msg": "L/R separation at 100 Hz (dB)"}],
              open(f"tb_vref_xtalk_{name}.bounds.json", "w"), indent=1)
print("ok")

# DJ-direct: the TPA3220 hears LSW_L (U14 NC): C100 4.7 uF from LDIV_L (R100/R101 4.7k/4.7k from
# the RCA, FLX4 source 100R), R102 100k to VREF. Same rail dip.
for name, c64, inm in (("asbuilt", "10u", "0"), ("fix_inm_vref_c100u", "100u", "vref"), ("fix_c100u", "100u", "0")):
    t = f"""* tb_vref_dip_dj_{name}: DJ-direct, 10 mV 1 ms dip on +5V -> speaker
Vrail p5 0 {DIP}
RFB p5 a5 0.05
C21 a5 0 10u
R90 a5 vref 470
R91 vref 0 470
C64 vref 0 {c64}
* the DAC legs still load VREF (2 x 4.1k to 1.65 V)
VD d 0 1.65
Rleg1 vref d 4.106k
Rleg2 vref d 4.106k
Vflx flx 0 0
Rsrc flx lin 100
R100 lin ldiv 4.7k
R101 ldiv 0 4.7k
C100 ldiv lsw 4.7u
R102 lsw vref 100k
Cin1 lsw inp 10u
Rinp inp 0 24k
Cin2 inm {inm} 10u
Rinm inm 0 24k
Espk spk 0 inp inm 15.85
.tran 20u 0.4 0 20u
.measure tran spk_pk MAX v(spk) FROM=0.045 TO=0.4
.measure tran spk_min MIN v(spk) FROM=0.045 TO=0.4
.measure tran spk_pp PARAM='(spk_pk-spk_min)*1000'
.end
"""
    open(f"tb_vref_dip_dj_{name}.cir", "w").write(t)
    json.dump([{"measure": "spk_pp", "max": 2.0, "severity": "error",
                "msg": "DJ-direct speaker tick (mVpp) from a 10 mV, 1 ms +5V dip"}],
              open(f"tb_vref_dip_dj_{name}.bounds.json", "w"), indent=1)
