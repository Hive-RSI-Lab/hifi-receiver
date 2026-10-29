# Second read of #48's fixes (Lite and core), and hwde aa1ec48's new checks

Task 261029-111235 (issue #55), 2026-10-29. #48's own fix-check agent never
finished (`docs/review-2/fix-check.md` there stops at "in progress"), so this
is the independent read of the ECO it shipped: PR #48 head `e5896e6`, read
against its base `18ceb29`. Nothing here was taken from #48's dispositions
without checking it on the board files.

**How it was read.** Each board's `.kicad_pcb` was parsed pad by pad (every
footprint's value, LCSC code and pad nets) at the base and at #48's head and
the two were diffed, so every changed part and every net a changed pad sits
on is listed below and was looked at. The PVDD switch on Lite was simulated
with review-2's own AO4485 model (`sims/second-read/`, `run.py` as in
review-2's power sims). KiCad's DRC ran on both PCBs with zones refilled.
hwde aa1ec48's new checks ran on #48's files: `stock_check` (live JLC/LCSC),
the waiver sign-off rule and `sim_board` (below).

## Findings

| # | Board | Severity | Finding | Done |
|---|---|---|---|---|
| S-1 | lite-main | **major** | #48's PVDD switch values (R6 47k -> 220k, new C19 4.7 uF) cut Q1's gate drive to VBUS x 100/320 = 0.31. At a 9 V contract Vsg is 2.2-2.3 V: a max-Vth (2.5 V) AO4485 never turns on (sim: PVDD stays at 0, `tb_lite_built_9v_vth2.5`), a typical one runs at Vsg 2.15 V. At 12 V it is 2.9 V (sub-4.5 V, no datasheet RDS(on)); at 15 V 3.7 V and at 20 V 5.0 V (still rising: tau (R5‖R6)·C19 = 0.2 s). Firmware allowed the amp from 9 V (`hifi_power_cfg_lite` v_min, #48's firmware-requirements L4). | Firmware v_min 14 V on this branch, so a 9/12 V-only charger leaves the amp off; manual and firmware.md say so. #48's L4 line should read 15 V. |
| S-2 | lite-main | **major** | The same RC delays PVDD: it starts 80-110 ms after PVDD_EN and reaches 90 % 110-150 ms after it at 20 V (170-210 ms at 15 V, Vth typ/max). Firmware released SDZ 100 ms after PVDD_EN (`soft_start_ms`), so the amp came out of shutdown with PVDD at 0-10 V and still ramping: start-up pop and a FAULTZ read inside a 50 ms blank. | Firmware soft_start 400 ms on this branch (about 2x the slowest ramp); tests updated. |
| S-3 | lite-main | minor (tool) | hwde's new `stock_check` blocks J1 (C165948, HRO TYPE-C-31-M-12) as `unknown` though parts.json's MPN equals JLC's model exactly: `stocklib.matches()` skips the MPN branch when MPN == value, and the family-word rule needs a 3-character word with a digit. bom_cpl's live fab run would refuse Lite on this one row. | Brief 261029-142656 on hive-hwde (fix in `matches()`). |
| S-4 | core | note | TCA9539 has no I/O pull-ups, so LCD_BL (P06, only on J3) floats from reset until firmware drives it: the Ultra display's backlight is undefined during boot. Harmless to the board; a 100k to GND would fix it at the next spin. | Listed for the next core ECO. |
| S-5 | both | follow-up | The enclosures in main were fitted to the base boards. After #48: core U18 is a front-facing THT IRM-3638T at the front edge (core_box.py still cuts a lid window over a top-looking IRM-H638T), core J1 is a THT USB4085 (core.board.json still has the SMD USB4105), Lite J1 is the HRO part, and THERMAL-1's heatsink is pressed from the lid, which no enclosure models. | For the enclosure re-fit once #48 lands. |
| S-6 | core | note | Y1 49.152 MHz (C7431354) has 451 sellable at JLC today: enough for 5 boards, thin for 100. | Watch at order time. |
| S-7 | both | major (bring-up) | Self-monitoring gaps from hwde's check_observability (task 261029-020701): neither board has a single test point; core has no PVDD sense and no GPIO for PD_PG; no rail other than VBUS/PVDD (Lite) or VBUS current (core) has an ADC path. | Per rail below. Needs an ECO on #48's branch, which this branch can't reach. |

No blocker found.

## S-7: what each board can see of itself (read on #48's PCBs)

**There are no free ADC1 pins on either board**, and ADC2 is unusable with
Wi-Fi up. Core's WROVER uses GPIO36 (I2S_DIN), 39 (EXP_INT), 34 (ISENSE) and
35 (IR_IN). Lite's uses 36 (BTN_VOLDN), 39 (PVDD_SENSE), 34 (VBUS_SENSE), 35
(I2S_DIN) and 32/33 (I2C). Every core GPIO is taken, and all 16 TCA9539 pins
are too (P17 is now TAP_SEL_IO). So a new sense path costs a moved function
as well as the parts.

| Board | Signal | Today | Proposal | Cost |
|---|---|---|---|---|
| both | every rail (VBUS, PVDD, +5V, +3V3, +3V3A, core's +5VA and +3V3E) | no test point anywhere | a 1.0 mm round SMD test-point pad on each rail and GND, bottom side, on the bench fixture's grid (`docs/bench.md`) | none in the BOM: copper only |
| lite-main | +5V | no ADC path | move BTN_VOLDN from GPIO36 to GPIO15. A strap pin, but its pull-up and a button to GND only change the boot log when held at reset, and the existing 10k R60 is the pull-up. Then put a 100k/47k divider (+5V × 0.32 = 1.66 V) and 100 nF on GPIO36 | 3 passives, about $0.003 + 6 joints |
| lite-main | +3V3, +3V3A | no ADC path | justified. A dead +3V3 is a dead ESP32: its brownout detector logs the reset reason, and the test point covers the bench. +3V3A feeds only the ES8388's analog side, so the speaker-tap loopback (taps on LIN2/RIN2) already fails when it is missing | none |
| core | PD_PG | gates Q2 only | justified for this spin. A lost contract drops PVDD in hardware, and firmware sees it as the INA186 current (ISENSE) falling to the buck's draw and the amps' FAULT on the expander. The CH224Q's own I2C status (address 0x22, contract and voltage registers) can also report it, but which register bit means 'contract lost' isn't verified yet: a known unknown for the firmware task. Next spin: PG to a TCA9535 input on Ultra's front board, or a second expander | none now |
| core | PVDD | no sense | for this spin, the only free ADC1 pin would be GPIO39, by dropping EXP_INT (firmware polls the TCA9539 over I2C, about 0.1 ms per read at 400 kHz; the encoder needs ~1 kHz polling). Then 100k/10k (PVDD/11, 2.55 V at 28 V, as on Lite) and 100 nF on GPIO39. Otherwise justified by ISENSE plus the amps' FAULT/OTW | 3 passives + one firmware change |
| core | +5V, +5VA, +3V3, +3V3A, +3V3E | no ADC path | justified: +3V3 as on Lite; +3V3A and +5VA by the speaker-tap/RCA loopback through U20 and the ES7243E; +3V3E by the LAN8720A answering on MDIO (PHY ID registers) and reporting link; +5V feeds +3V3, the TPA3221s' VDD and the op-amp, so it fails loud | test points only |

## What checks out (each fix, on the board files)

**Lite (149 parts; 11 added, 9 changed):**

- **SDZ (AUDIO-4):** R54 100k -> 10k (C25804) from AMP_SDZ_IO to U8 pin 2, C26 47 nF to GND: tau 0.47 ms; 3.3 V - 50 uA x 10k = 2.8 V vs VIH 2.0 V.
- **PVDD hold-off (POWER-3/4):** R6 220k (C22961) PSW_G -> PSW_PULL (Q4 collector, emitter on PD_PG), C19 4.7 uF/16 V (CL10A475KO8NNNC) VBUS -> PSW_G. 5 -> 20 V step at 30 V/ms with a 1.7 V-Vth FET: Vsg peaks 0.48 V, PVDD 2 mV, 0.6 A from the source (`tb_lite_built_pdstep_sr30_vth1.7`). Turn-on at 20 V: 2 A after the ramp gives Vds 29-32 mV. C19 sees at most ~6.3 V (10 V at the TVS clamp) on a 16 V part. The fix holds; S-1 and S-2 are its cost.
- **Line-in (AUDIO-7):** R30/R33 15k over R31/R34 10k: x0.4 unloaded, 25k load on the source.
- **5 V loop (POWER-10):** C9 47 pF (C1671) across the FB upper leg, C25 22 uF on +5V.
- **J1 HRO TYPE-C-31-M-12 (C165948):** VBUS on A4B9/B4A9, GND on A1B12/B1A12 and the four shell pads, CC1/CC2 on A5/B5, D+/D- on A6/B6, A7/B7: same nets as the USB4105 it replaces.
- **U6 CH340B (C81010):** pins 1-6, 13, 14, 16 as on the CH340C; pin 7 (RST#) and 15 (TNOW) open, V3 on +3V3 with VCC 3.3 V.
- **Taps:** R66/R68 100k from SPK_x_P (after L1/L3) over R67/R69 10k, 470 pF: /11, 37 kHz; 0.9 V DC +-0.9 V at 20 V, AC-coupled (1 uF) to ES8388 LIN2/RIN2 (pins 22/21, unused before).
- DRC (KiCad 10, zones refilled): 0 violations, 0 unconnected.

**core (301 parts; 19 added, 25 changed):**

- **U17 TCA9539PWR (C131972, JLC model TCA9539PWR):** pin 3 RESET on ESP_EN (was A2 on GND), A1/A0 on GND = 0x74 (firmware C1 says 0x74); port map unchanged except P17 now TAP_SEL_IO. Every input (P10-P16: FAULT, OTW, encoder, buttons) has its own 10k pull-up (R50-R56), so losing the TCA9555's internal pull-ups loses nothing there. Outputs at reset: AMP_A/B_RST (R40/R41 100k), DJ_DIRECT (R42), PVDD_EN (R43, R7), DAC_EN (R44) and PHY_RST (R45 2.2k) are held low; TAP_SEL_IO low through R134. Nothing else on the bus sits at 0x74: CH224Q, ES9018K2M, ES7243E, and over J3 the Ultra front board's TCA9535 (0x20-0x27).
- **PVDD gating (POWER-5/6):** Q2 2N7002 source on PD_PG (R20 10k to +3V3, CH224Q pin 10, which was on GND on the board but open in the schematic: PARTS-2 is fixed). PG high (no contract) puts Q2 at Vgs 0 whatever the expander does. PG sinks 0.33 mA + 30 uA.
- **Q1 SOA (POWER-2/7):** R4/R5 470k (C25790), C8 2.2 uF/50 V 0805 (C377773): Vsg = VBUS/2, clamped by D4 15 V; 4.5 V at a 9 V contract, so core doesn't share S-1.
- **5 A path (POWER-1, THERMAL-7):** within 8 mm, VBUS_IN vias at R1 2 -> 8 (plus J1's four THT VBUS pins), VBUS vias at Q1 6 -> 12, PVDD vias at Q1 4 -> 12.
- **J1 USB4085-GF-A (C7095263):** 16 THT pins 0.70 mm on 0.40 mm drills at 0.85 mm pitch, rows 1.35 mm apart, B row mirrored (B12 behind A1), as EasyEDA's model for C7095263 has them; VBUS_IN A4/A9/B4/B9, GND A1/A12/B1/B12 and 4 shell slots, CC1 A5, CC2 B5, D+ A6/B6, D- A7/B7. The pad gap is 0.15 mm, VBUS (28 V) beside CC1: the order remark (inspect for bridges) matters.
- **CH224Q CC pins:** pins 6/7 swapped nets between base and head (now CC1 -> pin 6). CC1/CC2 are symmetric for a sink, so either way works.
- **Taps and switch (AUDIO-2/3):** R130/R136 150k over 10k from A_SPK_x+ (after the LC filter), 470 pF, 1 uF into TAP_x biased at 1.65 V (100k/100k): 0.78-2.5 V at 28 V. U20 TS5A23157 (V+ +5VA): COM = ADC_SRC_x, NO = LSW_x, NC = TAP_x; IN from Q7 S8050 (B 47k/100k from P17, C 10k to +5VA): P17 floating or low = IN high = RCA input. All inside 0..5 V.
- **Values:** R73/R75/R81/R83 1.3k (AUDIO-1), C101/C102/C111/C112 4.7 uF 0603 (AUDIO-5), C47 1 uF (DIGITAL-5), Y1 49.152 MHz (C7431354, 3225, pin 1 OE on +3V3), R57 LCD_CS pull-down (DIGITAL-16), U18 IRM-3638T (C42421366: 1 OUT, 2 GND, 3 VCC), U8 CH340B.
- DRC (KiCad 10, zones refilled): 0 violations, 0 unconnected.

## hwde aa1ec48's new checks on #48's boards

- **stock_check** (c23af92, live, 5 boards, 2026-10-29 14:11Z): core Plus BOM 78/78 rows ok, Ultra 78/78 ok (J1, U8, U17, U18, Y1 all match JLC's model and are for sale); Lite 52/53 ok, J1 `unknown` (S-3, a tool bug).
- **Waiver sign-offs** (5e96c27): core's check_decoupling waiver and Lite's check_thermal waiver pass `approval_problem` (both name who and when). Both are signed by the Hive's workers under the person's delegation, not by the person; Lite's carries an open bring-up measurement.
- **sim_board** (f7def67): #48's real `.net` files were past the forge
  read cap, so it ran on netlists made from #48's PCB pads (nets, pin
  functions and types). On those it finds nothing new against the base. Three
  warnings carry over: C10's DC bias, U3 INA186 inputs at 30 V (rated 42 V),
  and an LED it can't light through an open IC. It also models fewer parts,
  and no MOSFETs at all, because the made-up netlist has no symbol data. That
  is a limit of this run, not a finding on the boards. More important: even
  on a real netlist it can't see S-1. `mosfet_vgs` only fires when Vgs is
  above the part's rating, never when it is too low. Brief
  261029-143919 on hive-hwde asks for an under-drive check.

## Not done

- #48's branch isn't reachable from a worker sandbox, so its files were read
  through the forge's contents API, which allows 300 reads an hour. 26 files
  over about 200 kB didn't come through: both `.net` files,
  `lite-main.kicad_sch`, core's `ethernet.kicad_sch` and `aiee.kicad_sym`,
  `state.json`, `verify.json`, `thermal.md`, `tools/known_unknowns.py`, the
  Lite power `_FIX` testbenches and a few Gerbers. So ERC and
  schematic-to-PCB parity weren't re-run here, the thermal review wasn't
  re-read, and the heatsink (open in #48's own follow-up) is out of scope.
- The boards weren't changed. S-1 and S-2 are fixed in firmware, which is
  also safe on main's current Lite (R6 47k: 10 V of gate drive at 15 V, PVDD
  up in about 5 ms).
