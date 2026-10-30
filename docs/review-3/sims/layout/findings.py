"""Findings data for docs/review-3/layout.json (review-3 LAYOUT). Edit here, run build_report.py."""
D = "2026-10-30"
T = "docs/review-3/sims/layout/"

FINDINGS = [
 {"id": "R3-LAYOUT-1", "board": "core", "severity": "major", "repeat_of": "new",
  "refs": ["U15", "U16", "C218", "C220", "C316", "C318", "C219", "C221"],
  "nets": ["/amp_a/A_IN1P", "/amp_a/A_IN2P", "/amp_b/B_IN1P", "/amp_b/B_IN2M", "PVDD"],
  "finding": "The TPA3220 input nodes after the DC-blocking caps are long, single-ended and run over the switching PVDD plane. "
             "U15's input caps C218/C220 sit at x~80 mm, next to U16, not next to U15 (x 46). From the cap to the pin, A_IN1P is 45.2 mm and A_IN2P 55.8 mm. "
             "Their partners A_IN1M/A_IN2M are 6.7/9.5 mm long and go to C219/C221 at U15, so the pair is not routed together and does not cancel pickup. "
             "Coupled length to PVDD copper on the adjacent layer, 0.24 mm away: A_IN1P 31 mm, A_IN2P 16 mm, B_IN1P 17 mm, B_IN2M 7 mm. A_IN1P also passes 3 mm from U16's B_OUT1P pads and 1.6 mm from B_BST1M. "
             "This is the TPA3220's high-impedance input node, after the cap.",
  "evidence": "coupling.py on a refilled copy (out/coupling_core_amp_in.log); gnd_ref.py (out/gndref_core_amp.log: A_IN1P 15 % over GND, 31 mm over PVDD, 12 plane changes). "
              "Estimate: a 0.2 mm trace over 31 mm at 0.244 mm prepreg (er 4.3) is about 1 pF parallel-plate, ~2 pF with fringing. "
              "PVDD carries the edge spikes of R3-AMP-2 (several V for ns), and the OUT/BST nodes swing 28 V at 600 kHz with ~ns edges. "
              "With a 2 pF coupling into the ~10-20 kohm input, a 28 V edge injects charge that the 18-24 dB gain amplifies. "
              "Its 600 kHz part aliases with the AM/PWM loop. Because U15 and U16 are synced (OSCM/OSCP), it shows as raised noise and idle tones rather than a beat. Not simulated.",
  "reference_design": "SLASEN3B fig 53 / 11.1: input caps and input traces sit at the input pins (pins 8/9 and 15/16), routed as pairs on the far side from the power stage. On the TPA3220EVM the input network sits beside the chip, inside ~10 mm.",
  "fix": "Move C218/C220 to within 5 mm of U15 pins 8 and 15, beside C219/C221, so the long run is the low-impedance OPA1678/TS5A23157 side (AMP_IN_L/R) rather than the high-impedance pin side. "
         "Route A_INxP with A_INxM as a pair on F.Cu over In1 GND, and keep them off the PVDD In2 area and ≥3 mm from any OUT/BST copper. Do the same for C316/C318 (U16), keeping B_IN1P and B_IN2M off PVDD. "
         "Bench check: idle noise at the speaker terminals A-weighted ≤ 100 uV with the inputs shorted at AMP_IN_L/R, both chips running.",
  "margin": "No number for the audible effect; the geometry is 5-10x TI's input route length, over the noisiest copper on the board."},
 {"id": "R3-LAYOUT-2", "board": "core", "severity": "minor", "repeat_of": "new",
  "refs": ["U7", "U10", "U12", "U13", "U9"], "nets": ["/I2S_BCK", "/I2S_LRCK", "/I2S_DOUT_MAIN", "/I2S_DOUT_SUB", "/RMII_TXD0", "/RMII_TXD1", "/linein/LIN_L", "/linein/LIN_R", "/dac/DACL_N", "/MAIN_R", "+3V3"],
  "finding": "On core, the I2S and RMII buses and several analog lines run on B.Cu. Their nearest plane is In2.Cu, split into +3V3/PVDD/VBUS/VBUS_IN, not GND. "
             "Over +3V3: I2S_BCK 79 of 105 mm, LRCK 83/114, DOUT_MAIN 104/111, RMII_TXD0 52/96. Each crosses a plane edge 3-15 times. "
             "The analog lines (LIN_L 25 mm, LIN_R 18, DACL_N 17, MAIN_R 13) sit over +3V3, which carries the ESP32's Wi-Fi bursts. "
             "Return current crosses from +3V3 to GND only through the nearest decoupling cap. That is an EMI and digital-to-analog coupling path, not a failure.",
  "evidence": "gnd_ref.py (out/gndref_core.log): % length over GND, plane under each trace, in-segment plane changes.",
  "reference_design": "Espressif and ESS layout notes: high-speed and audio lines over continuous GND. A JLC 4-layer is usually GND / GND-or-power / signal.",
  "fix": "Cheapest: make In2.Cu GND under the I2S/analog corridor (x 100-140, y 130-180). +3V3 there can be a wide F.Cu pour or trace, since it carries ~0.3 A. Alternatively, add 100 nF +3V3-GND stitching caps where each I2S line changes reference. Keep the analog lines on F.Cu over In1 GND.",
  "margin": "EMI only; no audio failure demonstrated."},
 {"id": "R3-LAYOUT-3", "board": "core", "severity": "note", "repeat_of": "THERMAL-1",
  "refs": ["U15", "U16"], "nets": ["GND"],
  "finding": "TPA3220 (pad-down DDW) thermal path passes at realistic music power and at the governor limit. "
             "EP land 7.30 x 4.43 mm (= TI's), 15 x 0.3 mm vias into the In1 GND plane (93 % coverage). "
             "Chip loss is ~1.0 W at the governed limit: Plus 14.8 W/ch into 6 ohm with one channel per chip, or Ultra 4.25 W/ch x2 into 4 ohm. That gives Tj 92-93 C (98-100 C at the 36 C/W upper bound) in a 62 C box, against OTW 115 C min. "
             "At 1/8 clip into 6 ohm, Tj is 76 C. constraints.json's 2.0 W per chip is a 2x-conservative budget. "
             "hwde check_thermal wants ≥36 vias (it finds 20/21 counting nearby vias). That is a screen, not a datasheet rule.",
  "evidence": "thermal_calc.py (out/thermal_calc.log): Rds 70 mOhm x1.4 hot, P_idle 0.55 W AD @30 V scaled (SLASEN3B 7.5/7.6), theta 30 C/W nominal = 23.5 (7.4, JEDEC 2s2p) x1.1 for 0.5 oz inner x1.15 for the second chip on the board.",
  "reference_design": "SLASEN3B 7.4: RthJA 23.5 C/W on JEDEC 2s2p. DDW0044D land pattern: EP 7.3 x 4.43, 0.2 mm vias at 1.3 mm pitch.",
  "fix": "None required. Cheap upgrade on the next layout touch: 0.3 mm vias at 1.0-1.3 mm pitch to fill the EP (~24), as in TI's land pattern.",
  "margin": "22 K to OTW min at the governed limit (nominal theta), 15 K at the upper bound."},
 {"id": "R3-LAYOUT-4", "board": "core", "severity": "note", "repeat_of": "PARTS (R3-AMP-9)",
  "refs": ["U15", "U16"], "nets": [],
  "finding": "JLC/LCSC stock of TPA3220 (C2879306) is 0 today; 10 are needed for 5 boards. Every other line on all four BOMs is in stock at ≥5 boards' need. Thinnest: ES9018K2M C2688377 705, 49.152 MHz Y1 C7431354 497, L1-L4 Lite C48391614 1676 (21 needed), Ultra U19 PLR237/T8BK 236.",
  "evidence": "jlc_stock.py against JLC's component API, read 2026-10-30 (out/stock_*.log).",
  "reference_design": "-", "fix": "Keep the pre-buy (fab/prebuy.csv) and re-run jlc_stock.py on the order day.",
  "margin": "TPA3220: -10 parts. Others ≥ 2x need."},
 {"id": "R3-LAYOUT-5", "board": "both", "severity": "note", "repeat_of": "new",
  "refs": [], "nets": [],
  "finding": "CPL rotation and pin 1 pass on all four placements (Lite 145, Plus 293, Ultra 292+U19, ultra-front 36 parts). Two checks were run. "
             "First, every LCSC EasyEDA model pad, turned by the CPL rotation and put at the CPL position, lands on the board pad with the same number (max error 0.135 mm, Lite U6). "
             "Second, no vendor ground pin lands on a non-ground net (the 'CHECK' lines are intentional straps to GND: MODSEL, AMx, ADDR, HEAD, V-). "
             "Every footprint is an EasyEDA import, so CPL rotation = board rotation by construction. The remaining risk is only an LCSC model whose pin 1 disagrees with the physical part; spot checks of the model silk dots pass. "
             "Ultra U19 (PLR237/T8BK, C17223620) has no EasyEDA model (API 404), so JLC cannot machine-place it; it is THT and hand-soldered anyway.",
  "evidence": "cpl_pin1.py (out/pin1_*.json); hwde cpl_verify.py (out/cplv_*.json: Lite 145/145, Plus 293/293 ok; Ultra and front were rate-limited and are covered by cpl_pin1).",
  "reference_design": "JLC places by its LCSC footprint model turned by the CPL rotation.",
  "fix": "None. Keep JLC's DFM preview check on the order day (U19 placement by hand).", "margin": "0 failures / 766 rows."},
 {"id": "R3-LAYOUT-6", "board": "lite-main", "severity": "minor", "repeat_of": "new",
  "refs": ["U5", "U7", "U6", "J1"], "nets": ["/I2S_MCLK", "/I2S_BCLK", "/I2S_LRCK", "/I2S_DOUT", "/USB_DP", "/USB_DM"],
  "finding": "Lite (2-layer): the I2S bus to the ES8388 runs largely on B.Cu, with gaps in the F.Cu GND above it. Longest unreferenced run: BCLK 12.3 mm, DOUT 11.8, LRCK 10.8, MCLK 10.1 mm; 41-70 % of each line has GND on the other side. "
             "USB D+/D- run 74 mm from J1 to the CH340B (59-65 % referenced, longest gap 6.5-7.3 mm). "
             "The amp's inputs are 100 % referenced. The RCA line-in is 80-86 %, its gaps ≤ 1.8 mm at the jack.",
  "evidence": "gnd_ref.py (out/gndref_lite.log); F.Cu GND covers 68 % and B.Cu 82 % of the board.",
  "reference_design": "Espressif HW design guide: digital audio lines over unbroken GND. 2-layer boards usually route them on top over a bottom GND plane.",
  "fix": "Optional: add F.Cu GND stitching over the B.Cu I2S run near (52-56, 106-124) mm. No functional change needed.",
  "margin": "MCLK 12.288 MHz, harmonics to ~300 MHz: EMI only."},
 {"id": "R3-LAYOUT-7", "board": "lite-main", "severity": "minor", "repeat_of": "new",
  "refs": ["U2", "C3", "C5"], "nets": ["VBUS", "GND", "/BUCK5_SW"],
  "finding": "TPS54302 (5 V buck) hot loop. The nearest input cap, C5 100 nF 0603, is 1.9 mm from VIN pin 3. Its GND pad returns to GND pin 1 through two vias and B.Cu, 4.4 mm pad to pad. "
             "The bulk input cap C3 10 uF 1206 is 3.6 mm from VIN and 6.1 mm from GND. The loop is ~5 mm² plus two vias, ~3-4 nH. "
             "TI's layout puts the 10 uF and 100 nF on the top layer with VIN and GND pins in a direct same-layer loop. "
             "SY8089 (U3): C11 100 nF 1.7/2.6 mm, C10 10 uF 3.5/4.4 mm, which is fine.",
  "evidence": "pad geometry from the PCB (plot.py crop, pcbgeo); TPS54302 datasheet SLVSDB4 10.1 layout guidelines.",
  "reference_design": "TPS54302EVM: Cin right at pins 3/1 on top, GND pour joining them on top.",
  "fix": "Optional, on the next layout touch: put a GND F.Cu bridge from C5's GND pad straight to U2 pin 1. Otherwise accept; the 100 nF covers the HF edge.",
  "margin": "Works; SW ringing maybe ~10-20 % higher than the EVM's. No rating at risk: VIN 20 V, part rated 28 V."},
 {"id": "R3-LAYOUT-8", "board": "lite-main", "severity": "minor", "repeat_of": "THERMAL-4/DFM-9",
  "refs": ["U8"], "nets": ["GND"],
  "finding": "TPA3118 EP land is 3.81 x 3.71 mm, against TI's 3.74 x 3.04 (DAP0032C example board layout). It has 16 x 0.3 mm vias at 0.85 mm pitch, open on top and tented on the bottom. "
             "The four 1.4 mm paste windows (7.8 mm², 69 % of the die-pad area) sit over all 16 open vias. Solder wicks into the vias and the tented bottom traps the gas, which is the usual cause of EP voiding. "
             "The 0.343 mm pin pads are narrower than TI's 0.45 mm.",
  "evidence": "U8 footprint pads from the PCB; TI SLOS708G DAP0032C land/stencil pages (EP 3.04 x 3.74; stencil 3.04 x 3.74 at 0.125 mm).",
  "reference_design": "TI: 0.2 mm vias at 1.3 mm pitch in the EP, 100 % paste.",
  "fix": "Order option: JLC 'via in pad, epoxy filled and capped' for this board, or move the paste windows off the via grid. Otherwise accept; round 1 justified it.",
  "margin": "Thermal margin (below) holds with 30 % voiding."},
 {"id": "R3-LAYOUT-9", "board": "both", "severity": "minor", "repeat_of": "DIGITAL-18/DIGITAL-19",
  "refs": ["U5 (Lite)", "U7 (core)"], "nets": [],
  "finding": "ESP32-WROVER antenna. The keep-out under the antenna is copper-free on both boards (round-1 fix holds on core: 4-layer keepout x 148.9-154.6, y 189.6-213.5). "
             "The cleared strip past the module sides is only 2.25/2.25 mm (Lite) and 0.85/2.55 mm (core), against Espressif's 15 mm or a board cut-out. "
             "Expect a few dB of range loss, worse inside a metal-lidded box.",
  "evidence": "hwde check_antenna on refilled copies (out/check_antenna_*.json).",
  "reference_design": "ESP32 HW design guidelines §1.4.8 / WROVER-E datasheet keep-out: antenna over the edge, or ≥15 mm clear on each side.",
  "fix": "No change for the first spin; if Wi-Fi is weak in the box, the WROVER-IE (U.FL) variant on the same footprint is the contingency.",
  "margin": "2.25 mm / 0.85 mm against 15 mm."},
 {"id": "R3-LAYOUT-10", "board": "both", "severity": "note", "repeat_of": "new",
  "refs": [], "nets": [],
  "finding": "KiCad DRC with zones refilled and schematic parity: 0 errors, 0 warnings, 0 unconnected on all three boards. "
             "The project files set 'missing_courtyard', 'footprint_type_mismatch' and 'lib_footprint_mismatch' to ignore, so DRC is blind to parts without a courtyard.",
  "evidence": "kicad-cli 10.0.6 pcb drc --refill-zones --schematic-parity --severity-all (out/drc_*.json).",
  "reference_design": "-", "fix": "None.", "margin": "-"},
 {"id": "R3-LAYOUT-11", "board": "lite-main", "severity": "note", "repeat_of": "new",
  "refs": [], "nets": ["PVDD", "/OUT_*", "VBUS"],
  "finding": "hwde check_current passes Lite only because constraints.json budgets PVDD at 0.8 A, VBUS at 0.4 A and the speaker outputs at 1.0 A. "
             "Real values at a 20 V/3 A contract are ~2.7 A on VBUS, ~2.4 A on PVDD, and 1.6-2.2 A rms per output into 6 ohm near clip. That gate is not a check of the real currents.",
  "evidence": "out/check_current_lite-main.json; current from the PD contract and P=V²/R.",
  "reference_design": "-", "fix": "Raise the constraints.json currents to the real values and re-run check_current (the worker owns boards/).", "margin": "pending re-run"},
 {"id": "R3-LAYOUT-12", "board": "core", "severity": "blocker", "repeat_of": "DFM-1/DFM-2",
  "refs": ["J4", "J5"], "nets": ["/dac/SUB1_OUT", "/dac/SUB2_OUT", "/linein/LIN_L", "/linein/LIN_R"],
  "finding": "Round 1's RCA fix is not on the core board. J4 (SUB OUT) and J5 (LINE IN, the FLX4 input) still carry the old footprint instance: 3 round 1.6 mm holes at (-4,-1.95), (0,+1.95), (4,-1.15). "
             "There are no 2.0 x 3.0 mm locating-leg slots and no 1.6 x 2.4 plated slots. The library file boards/core/lib/aiee.pretty/CONN-TH_XDJK-0271-1650.kicad_mod WAS fixed (legs at ±7.65, +1.60; slots at -1.55), but the board was never updated from it. "
             "The Gerber drill file core.drl has no 2.0 mm tool and no slot for these legs. This is unchanged in every core commit since the round-1 fix (9da177c, 20c8bc3, 92af8ee, ac484b1). "
             "Lite's J2/J3 do carry the fixed footprint.",
  "evidence": "PCB footprint pads (pcbgeo; out/esd_core.log lists the pads too); git show <commit>:boards/core/kicad/core.kicad_pcb for the four commits; boards/core/fab/gerbers/core.drl tool list T1-T16 (no 2.0). "
              "Round 1 DFM-1/DFM-2 jack drawing: legs 2.0 x 3.0 at ±7.65; signal pins on one row.",
  "reference_design": "Lite's J2/J3 instances = the fixed library footprint.",
  "fix": "In core.kicad_pcb, update J4 and J5 from the library (Update Footprint from Library, or the generator's footprint step) so they get the NPTH 2.0 x 3.0 legs at (±7.65, +1.60) and the 1.6 x 2.4 plated slots for pins 1/3 at y -1.55. "
         "Re-check clearances to C71/C72/R92/R93 and R100/R110, re-route, then regenerate the Gerbers and drill file. "
         "Add a test: compare each board footprint instance against its library .kicad_mod pad list. KiCad's lib_footprint_mismatch DRC is set to ignore in core.kicad_pro; set it to error.",
  "margin": "The jack cannot be inserted: no hole for its two 2.0 x 3.0 legs."},
 {"id": "R3-LAYOUT-13", "board": "lite-main", "severity": "minor", "repeat_of": "THERMAL-3",
  "refs": ["U8"], "nets": ["GND"],
  "finding": "TPA3118 junction temperature at the Lite governor's ceiling is near the 150 C trip. 13 W/ch continuous into 4 ohm gives a chip loss of ~2.9 W, mostly conduction: 1.8 A rms x 2 x 168 mOhm hot per channel plus 0.64 W idle. "
             "In the 62 C governed box (round-1 THERMAL-6) that is Tj 126 C at TI's 22 C/W EVM figure, 149 C at 30 C/W, and >150 C at hwde's 48 C/W screen. Into 6 ohm, Tj is 108-126 C. "
             "Realistic loud music (1/8 clip, 6 ohm, 52 C box) gives 1.1 W and Tj 76-84 C. Heavily compressed music at clip (~10 W/ch, 4 ohm) gives ~2.3 W and Tj 113-131 C. "
             "Sustained near-limit signal with 4 ohm speakers in a warm room can reach the latched OTE. Firmware recovers it (FAULTZ), which is heard as a dropout.",
  "evidence": "thermal_calc.py (out/thermal_calc.log); SLOS708G 6.4 (RthJA 22 C/W on the 2-layer EVM), 6.5 (Rds 120 mOhm, Icc 32 mA); hwde check_thermal (out/check_thermal_lite-main.json, 48 C/W).",
  "reference_design": "TPA3118D2EVM: ~32 EP vias into a 2-layer GND pour; ours has 16.",
  "fix": "Firmware, cheapest: Lite 4 ohm governor 13 -> 9 W/ch (2.1 W chip, Tj ≤ 125 C at 30 C/W). Layout, on the next touch: double the EP vias to ~30 (0.3 mm, filled or tented, see R3-LAYOUT-8). Bench: 30 min of pink noise at the governed level into 4 ohm in the closed box; read the case temperature (Tj ≈ Tcase + ~2 K, psiJT 0.3) and require ≤ 125 C.",
  "margin": "1-24 K to OTE at the 4 ohm governed limit (theta 22-30 C/W); 24-42 K at 6 ohm; 66 K at 1/8 power."},
 {"id": "R3-LAYOUT-14", "board": "both", "severity": "minor", "repeat_of": "new",
  "refs": ["J1", "U1", "U6 (Lite)", "U2 (core)"], "nets": ["/USB_DP", "/USB_DM", "/CC1", "/CC2"],
  "finding": "ESD at USB-C. Lite's D+/D- go straight from J1 to the CH340B 54 mm away with no TVS; core has a USBLC6-2SC6 there. "
             "On both boards CC1/CC2 go straight to the CH224A/Q, 3-10 mm from the jack, with no TVS. "
             "VBUS has SMBJ22A (Lite, 5.6 mm) and SMBJ30A (core, 5.1 mm). Speaker, RCA (15k/4.7k series R) and Ethernet (magnetics) are protected by their series parts.",
  "evidence": "esd_scan.py (out/esd_*.log).",
  "reference_design": "WCH CH224 reference: CC direct (no TVS), which is common practice; USB 2.0 data lines usually get a USBLC6/ESD5V3 at the connector.",
  "fix": "Lite: add USBLC6-2SC6 (C2687116, the part core uses) on D+/D-/VBUS within 5 mm of J1, as on core. CC: accept (vendor practice), or add a DNP 0402 footprint for a low-capacitance ≥24 V TVS on each CC, e.g. ESD9B24 class.",
  "margin": "CH340B HBM rating not published by WCH; IEC 61000-4-2 events at plug-in are not covered."},
 {"id": "R3-LAYOUT-15", "board": "core", "severity": "minor", "repeat_of": "new",
  "refs": ["U4", "C10", "C11"], "nets": ["VBUS", "GND", "/rails/BUCK5_LX"],
  "finding": "SY8303 (28 V to 5 V buck) input loop. IN pin 5 drops through a via into the In2 VBUS plane and comes back up at C10 (10 uF 1206), 4.0 mm away. C10's GND returns on F.Cu to GND pins 3/4 (3.8 mm). C11 100 nF is 4.0/7.2 mm. "
             "The hot loop is ~3 x 4 mm plus two via transitions, ~3-5 nH. A 2 A switching edge in ~2 ns adds ~3-5 V to the 28 V IN at the switch transition, giving ~33 V against the 40 V absolute maximum. "
             "The LX node to L9 is short (fine).",
  "evidence": "pad geometry (pcbgeo), crop render plot.py at (67,172); SY8303 layout guidance: input cap 'as close as possible' between IN and GND.",
  "reference_design": "SY8303 datasheet layout: Cin directly across IN and GND on the same layer.",
  "fix": "On the next layout touch: move C11 (100 nF, 0603 or 0402, ≥50 V) directly across pins 5 and 4, on F.Cu with no vias. Optional: rotate C10 to sit on the same F.Cu island. Bench: scope LX and IN at 28 V and 2 A load; IN spike ≤ 35 V.",
  "margin": "estimated 33 V vs 40 V abs max (7 V)."},
 {"id": "R3-LAYOUT-16", "board": "core", "severity": "minor", "repeat_of": "DFM-9",
  "refs": ["U15", "U16", "U9", "U10", "U13"], "nets": ["GND"],
  "finding": "TPA3220 EP paste: four 3.0 x 1.7 mm windows (20.4 mm², 63 % of the 32.3 mm² land, against TI's 100 % at 0.125 mm). The windows cover the open 0.3 mm vias at (±1.5/±3.0, ±1.3), so solder wicks and the pad voids. Same pattern as Lite U8.",
  "evidence": "U15 footprint pads (pcbgeo); SLASEN3B DDW0044D solder paste example.",
  "reference_design": "TI: 100 % EP paste, 0.2 mm vias.",
  "fix": "Order option: via-in-pad filled and capped (JLC 'POFV'), or leave as is. Thermal has 15-22 K margin (R3-LAYOUT-3) even with 30 % voids.",
  "margin": "-"},

]

ROUND1 = [
 {"id": "DFM-1", "holds": False, "how": "core J4/J5 board instances have no locating-leg holes; the library file was fixed, the board was not (fp_vs_lib.py; core.drl has no 2.0 mm tool). -> R3-LAYOUT-12 blocker."},
 {"id": "DFM-2", "holds": False, "how": "Lite J2/J3: holds (1.6x2.4 slots, 2.0x3.0 legs). core J4/J5: old round holes on the -1.95/-1.15 rows. -> R3-LAYOUT-12."},
 {"id": "DFM-6", "holds": True, "how": "core J1 USB4085 pads 0.70 mm on 0.40 mm drills (0.15 mm annulus), measured on the board."},
 {"id": "DFM-12", "holds": True, "how": "core U19 footprint has NPTH 1.70 mm posts at (±2.80, +3.92). No EasyEDA model for C17223620 (API 404): hand placement."},
 {"id": "DFM-4", "holds": True, "how": "core U18 is an IRM-3638T THT; its pin row is at y 217.7 with the board edge at 220.7 (3.0 mm)."},
 {"id": "DFM-9", "holds": False, "how": "Still EP paste over open vias on Lite U8 and core U15/U16 (R3-LAYOUT-8/16). Round 1 justified it; minor."},
 {"id": "DFM-10", "holds": False, "how": "Still no fiducials on any board (0 FID footprints). JLC accepts single boards without them (it uses pads); a note only."},
 {"id": "THERMAL-1", "holds": True, "how": "Superseded by the TPA3220 pad-down part: EP land 7.30 x 4.43 (= TI), 15 vias to In1 GND; Tj 92-93 C at the governed limit (R3-LAYOUT-3)."},
 {"id": "THERMAL-3", "holds": False, "how": "The Lite 13 W/ch governor is near the trip at 4 ohm in the warm box (Tj 126-149 C vs 150 C), R3-LAYOUT-13 minor. 6 ohm and music power pass."},
 {"id": "THERMAL-4", "holds": True, "how": "Unchanged 16 vias; accepted, see R3-LAYOUT-8/13."},
 {"id": "THERMAL-7", "holds": True, "how": "core Q1 drain: F.Cu PVDD patch 24 mm² plus 12 PVDD vias within 8 mm into the In2 PVDD pour (2833 mm²)."},
 {"id": "THERMAL-9", "holds": True, "how": "constraints.json U15/U16 theta_ja 23.5 C/W citing SLASEN3B 7.4, 2.0 W budget (≥2x my 1.0 W estimate)."},
 {"id": "DIGITAL-18", "holds": True, "how": "Lite antenna keep-out copper-free (check_antenna); side clearance still 2.25 mm (justified, R3-LAYOUT-9)."},
 {"id": "DIGITAL-19", "holds": True, "how": "core ANT_KEEPOUT on all four layers x 148.9-154.6, y 189.6-213.5; check_antenna finds no copper under the antenna; side clearance 0.85/2.55 mm (R3-LAYOUT-9)."},
]
FIELD = [
 {"issue": "JLC CPL rotation errors (KiCad vs EasyEDA package orientation; SOT-23 and SOIC 90/180 deg flips are the classic first-order failure).", "source": "JLC CPL rotation guide; hwde cpl_verify docstring (the owner's SOT-23-5/6 180-degree incident)", "our_board": "immune", "how": "All footprints are EasyEDA imports and CPL rotation = board rotation; two independent fits pass on all 766 rows (R3-LAYOUT-5)."},
 {"issue": "QFN/PowerPAD voiding and solder wicking through open thermal vias under 100 % or windowed paste.", "source": "TI SLMA002/SLMA004 (PowerPAD); TI DAP0032C/DDW0044D stencil notes", "our_board": "has it", "how": "Paste windows over open 0.3 mm vias on U8 (Lite) and U15/U16 (core); thermal margin absorbs 30 % voids (R3-LAYOUT-8/16)."},
 {"issue": "Class-D EMI and noise from input traces near the switching outputs/PVDD (TI: keep inputs short, away from the power stage).", "source": "SLASEN3B 11.1 / fig 53; SLOS708G 10.1", "our_board": "has it", "how": "core amp inputs 45-56 mm, over PVDD (R3-LAYOUT-1). Lite inputs 4-12 mm, 100 % over GND (immune)."},
 {"issue": "TPA3118 thermal shutdown on 2-layer boards at sustained high power (common on cheap TPA3118/3116 boards without heatsink).", "source": "SLOS708G 6.4 (22 C/W EVM) and its 150 C OTE", "our_board": "has it (marginal)", "how": "Only at the governed 4 ohm limit in a 62 C box (R3-LAYOUT-13); music power passes with 66 K margin."},
 {"issue": "ESP32 module antenna detuned by ground copper beside it / metal enclosures.", "source": "Espressif ESP32 HW design guidelines §1.4.8", "our_board": "has it (mild)", "how": "Under-antenna keep-outs are clean; side clearance 2.25 / 0.85 mm (R3-LAYOUT-9)."},
 {"issue": "Footprint fixed in the library but not pushed to the board (stale instance).", "source": "this review (KiCad lib_footprint_mismatch DRC set to ignore)", "our_board": "has it", "how": "core J4/J5 (R3-LAYOUT-12); fp_vs_lib.py now catches it."},
]
TESTS = [
 {"path": T + "cpl_pin1.py", "checks": "Every CPL row: LCSC EasyEDA model pads turned by the CPL rotation land on the same-numbered board pads; ground-pin/net consistency; model pin-1 dot. Exit 1 on misfit or missing model.", "result": "pass (Lite, Plus, front); Ultra: 1 no-model (U19 THT)"},
 {"path": T + "jlc_stock.py", "checks": "JLC stock and basic/extended for every BOM line against 5 boards' need.", "result": "fail: TPA3220 C2879306 stock 0 (Plus, Ultra); Lite and front pass"},
 {"path": T + "gnd_ref.py", "checks": "Per net: % of trace length with GND on the adjacent layer, longest gap, plane nets underneath, plane changes; board GND coverage per layer.", "result": "fail (core I2S/analog over +3V3/PVDD; Lite I2S gaps 10-12 mm)"},
 {"path": T + "coupling.py", "checks": "Victim trace length within d of aggressor copper (same or adjacent layer).", "result": "fail (core amp inputs over PVDD 7-31 mm)"},
 {"path": T + "plot.py", "checks": "Crop render of copper per layer (review aid, no pass/fail).", "result": "n/a"},
 {"path": T + "fp_vs_lib.py", "checks": "Every board footprint instance's pad set against its library .kicad_mod.", "result": "fail: core J4/J5 (stale RCA footprint); Lite, ultra-front pass"},
 {"path": T + "esd_scan.py", "checks": "Every connector signal pin: TVS present? first IC reached directly?", "result": "fail (flags USB CC/D± on Lite, CC on core, FFC lines - internal)"},
 {"path": T + "thermal_calc.py", "checks": "TPA3118/TPA3220 chip loss and Tj at music power and governed limits vs OTE/OTW.", "result": "fail: Lite governed 4/6 ohm at the 48 C/W upper bound; core passes"},
 {"path": T + "refill.sh", "checks": "Makes refilled copies of the boards in /tmp for the geometry tests (never touches boards/).", "result": "n/a"},
]
KU = [
 {"fact": "Vendor forum / GitHub field-issue search (E2E, Louder-ESP32, squeezelite-esp32 boards) was not completed in this round; field issues above are from vendor documents.", "board": "both", "refs": [], "status": "unverified", "source": "-", "date": D, "could_stop_board": False},
 {"fact": "TPA3220 input impedance and the actual noise pickup on the 45-56 mm input runs (R3-LAYOUT-1) are estimated, not simulated or measured.", "board": "core", "refs": ["U15", "U16"], "status": "unverified", "source": "estimate", "date": D, "could_stop_board": False},
 {"fact": "TPA3220DDWR C2879306 JLC stock 0 on 2026-10-30.", "board": "core", "refs": ["U15", "U16"], "status": "verified", "source": "JLC component API (jlc_stock.py)", "date": D, "could_stop_board": True},
 {"fact": "core J4/J5 RCA footprints lack the jack's leg holes.", "board": "core", "refs": ["J4", "J5"], "status": "verified", "source": "core.kicad_pcb, core.drl, fp_vs_lib.py", "date": D, "could_stop_board": True},
]
