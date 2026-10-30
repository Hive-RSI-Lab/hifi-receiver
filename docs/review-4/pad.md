# Review round 3 (Lite): power, class-D amp, digital/ESP32 (PAD)

Board `boards/lite-main` (root.py, lite-main.net, .kicad_pcb, fab/BOM.csv, CPL.csv) and the Lite firmware
(`esp/board_lite.c`, `esp/hifi_main.c`, `esp/hifi_board_lite.h`, `core/hifi_power.c`, `core/hifi_gov.c`, `core/hifi_pd.c`,
`patches/0001-hifi-receiver-hooks.patch`). Reviewer: PAD, 2026-10-30. Read-only on boards/ and firmware/.

**Result: 0 blocker, 0 major.** 4 minor (3 new, 2 repeats of round-2 minors still open = 5 minor lines), 4 notes.
The round-2 sub-out mute is built as drawn (netlist, PCB pad nets, BOM, CPL) and works in the typical case
(-40 dB in-band on the unplug thump). Its +5V sense has a -13 mV/°C trip tempco (Vbe x 6.45), which is the root of
R4-PAD-1/-2. No firmware path on Lite drives a pin the board doesn't have.

## Findings

| id | sev | repeat | finding | fix |
|---|---|---|---|---|
| R4-PAD-1 | minor | new | Mute trip moves with Q12's Vbe x (R80+R81)/R81 = 6.45: SUB_MUTE reaches 1.0 V at +5V = 4.86 V (-20 °C, hFE 60, R80/R81 2 % high), 4.56 V (0 °C), 3.94 V (25 °C), 3.22-3.33 V (70 °C); fully on (3.5 V) at 2.94-2.97 V hot. Hot, the mute leans on the ES8388 holding LOUT2 at VMID down to AVDD ~2 V (datasheet AVDD min 1.7 V): then the worst part (JSCJ 2N7002 Vth 2.5 V, Rds 7 Ω max x1.4 hot) leaves -62 mV in a 20-80 Hz band vs -0.60 V unmuted (-20 dB; typical part -6 mV, -40 dB). With round 2's own premise (LOUT2 moves at +5V 3.4 V) and a slow +5V fall (bulk back-feeding VBUS), at 70 °C the mute acts too late: -0.57 V, no better than none. | R36/R38 100R -> 470R (0603, LCSC C23179, JLC Basic - verify; also R3-SYSTEM-7's ESD fix): worst corner -33 mV (-25 dB). Bring-up: pull the USB-C after a 30-min loud run (box ~60 °C), scope SUB_L and +5V; record +5V where LOUT2 leaves 1.65 V. Contingency if it moves above +5V ~3 V: fit a 4.0 V open-drain voltage detector in place of Q12/R80/R81 (temperature-flat), next spin. |
| R4-PAD-4 | minor | new | USB-C capacitance on VBUS. At attach: C1 1 µF + C3 10 µF + C4 4.7 µF + C5 0.1 µF = 15.8 µF nominal (~13 µF at 5 V bias) vs Type-C cSnkBulk max 10 µF. After PG with Q1 on: + C58 470 µF + C59 220 µF = 706 µF on VBUS vs USB PD cSnkBulkPd max 100 µF (also on any negative transition / hard reset via Q1's body diode). Field: sonocotta/esp32-audio-dock #202 (Apple 15/30 W chargers refuse a CH224K board with 2820 µF straight on VBUS). Lite is immune to #202's mechanism (bulk behind Q1, charged 28 ms after PG); the 13 µF is a small overshoot. | None required. Bring-up: 20 attaches each on an Apple 30 W and 96/140 W and one Anker multi-port, plus plugging a phone into the second port of the multi-port (renegotiation) while playing. Contingency: DNP C4 (VBUS keeps C3 10 µF + C5, TI table 7-2 is 10 µF + 0.1 µF). |
| R4-PAD-5 | minor | new | The squeezelite DAC_CONTROLSET for HIFI_LITE writes reg 48/49 (LOUT2/ROUT2) = 30 (0 dB) and reg 46/47 = 28; board_lite.c's SUB_CODE_MAX 26 is applied only 2 s later by codec_apply(). In that window a 0 dBFS sub swings 1.41 Vpk, past the mute FETs' -Vth (1.0 V min) - clipping the sub's negative half, no damage. Amp side is safe (amp held until codec_up + 2.5 s). | patches/0001: DAC_CONTROLSET HIFI_LITE reg 48/49 val 26, reg 46/47 val 27, so the codec never sits above the caps. |
| R4-PAD-7 | minor | R3-DIGITAL-5 / R3-AUDIO-5 | Still open: hifi_board_lite.h LINE_DIVIDER 2.0 (R30/R31 15k/10k), HIFI_HAS_TAPS 0 (taps fitted), PIN_AMP_SDZ comment "through 100k" (R54 is 10k). Captures/self-test read wrong levels. | As round 2. |
| R4-PAD-8 | minor | R3-DIGITAL-6 / R3-LAYOUT-14 | Still open: no ESD array on USB D+/D- (CH340B pins 5/6 straight from J1). | As round 2: USBLC6-2SC6 (C2687116) at J1. |
| R4-PAD-2 | note | new | 5 V-only port (laptop, +5V = VBUS - 0.3 V = 4.2-4.7 V): sub out fully muted at 0 °C (SUB_MUTE 3.4 V at +5V 4.2 V), off at 25 °C (0.11 V). Amp is off there anyway (PG/v_start 14 V). | Document for bring-up; R4-PAD-1's detector contingency removes it. |
| R4-PAD-3 | note | new | Power-up is not covered by the mute: it releases at +5V ~3.9 V, ms after plug-in; squeezelite starts the ES8388 (VMIDSEL 50k, outputs on) seconds later. LOUT2's VMID ramp (tau 0.25 s) reaches the sub as a 0.31 V sub-sonic bump, 16 mV in a 20-80 Hz band (-31 dB vs the unmuted unplug thump). | Accept. |
| R4-PAD-6 | note | new | DJ-direct on Lite is analog (LIN1 -> mixer -> LOUT1) and never passes hifi_gov_process(): the power and thermal governor are blind there. Bounded today by the analog caps (round 2: 5.4 Vrms max = 7.3 W/ch into 4 Ω, 14.6 W both < 18 W thermal cap, ~19 W input < any 20 V PDO). R3-AUDIO-4's proposed DJ gain raise (10.9 Vrms = 29.7 W/ch into 4 Ω) would break both. | Don't apply R3-AUDIO-4 beyond 6.0 Vrms (9 W/ch into 4 Ω) unless DJ-direct gets a governor (e.g. ADC on, level from LIN1). |
| R4-PAD-9 | note | new | Mute FETs off while playing at the cap: -0.71 Vpk vs JSCJ 2N7002 Vth min 1.0 V at 25 °C, ~0.82 V at 70 °C (-4 mV/°C): 0.11 V margin; sub-threshold leakage through two series FETs into the 100R build-out is est. <= 20 µA, <= 2 mV (0.3 %) at the worst corner (level-1 model can't sim it). | None. |

## Round-2 fixes checked (Lite, this domain)

| id | holds | how |
|---|---|---|
| R3-AUDIO-2 (sub-out power-fail mute) | yes, typical; see R4-PAD-1 | `sims/pad/check_pad_nets.py`: Q11-Q15, R80-R82, C90 pins/values in lite-main.net and PCB pad nets match root.py; only C90/Q11/R82 on MUTE_RES; BOM C8545 (JSCJ 2N7002), C20526; CPL has all 9. Round-2 benches `docs/review-3/sims/fix/lite_mute` re-run: pass (sub_min -25 mV, play cap 0.71 Vpk, mute 0.075 V). My corners `sims/pad/mute/`: normal +5V 5.0-5.18 V mute <= 0.14 V at all corners (Vth min 1.0 V); C90 holds SUB_MUTE 4.3-4.6 V at t+0.9 s; IGSS 3 x 80 nA keeps it > 2.5 V for > 3 min. |
| R3-SYSTEM-1 / R3-POWER-3 (governor: unclipped power, 50 ms, peak limit) | yes | `docs/review-3/sims/system/run_gov.sh` against hifi_gov.c at HEAD: Lite 60/65 W peak 0.96x/0.91x, 0 ms over; 15 V/3 A steady 40.5 W. |
| R3-SYSTEM-2 / R3-LAYOUT-13 (Lite thermal 9 W/ch) | yes | hifi_gov_cfg_lite thermal_out_w 18 W = both channels (hifi_gov.h l.42, cap = thermal_out_w x amps), tau 30 s; r_load 4 Ω conservative for the 6 Ω Mozart. Not covering DJ-direct: R4-PAD-6. |
| R3-SYSTEM-5 (amp held until the codec settles / across an input change) | yes | hifi_main.c: o.amp_on forced false until codec_up and amp_hold_until = codec_up + 2500 ms; input_changed(): hold 200 ms, route at +30 ms; fw_duties.py L4/C5 PASS. |
| R3-SYSTEM-9 (input saved) | yes | input_changed() writes NVS hifi_input; restored in init (hifi_main.c l.963); fw_duties.py PASS. |
| SUB_CODE_MAX (board_lite.c) | yes after 2 s | sub_code() clamps to 26 in codec_apply(); the controlset's 30 before it: R4-PAD-5. |
| OTW step-down (C2) | n/a on Lite | board_lite.c sets otw = false (TPA3118 has no OTW pin); thermal handled by the governor. |
| R3-DIGITAL-5 / R3-AUDIO-5 constants | no | R4-PAD-7. |
| R3-DIGITAL-6 / R3-LAYOUT-14 USB ESD | no | R4-PAD-8 (no USBLC6 in root.py). |

## Blocks passed (reference, margin)

- **CH224A (U1)**: pins vs WCH CH224 DS V2.0 table 4-1 (VHV 1 and VBUS 8 shorted with 1 µF, CFG1 9, PG 10, CFG2/SCL 2, CFG3/SDA 3); Rset 120k = 20 V (table 5-1); "single-resistor mode enables I2C", single-byte reads only for CH224A (5.2.3) - hifi_i2c_read() does single-byte reads; status bits (0x09: BC/QC2/QC3/PD/EPR) decoded right in hifi_pd.c. VHV/PG/CC abs max 32 V vs 21 V (11 V margin; SMBJ22A Vc 35.5 V covered by R3-POWER-6). I2C VIH max 3.3 V vs 3.33 V bus (R3-DIGITAL-12). I2C failure falls back to a 7.5 W budget (hifi_contract_from).
- **PVDD switch**: Q1 S=VBUS/D=PVDD, Vgs = 0.3125 x VBUS (-6.25 V at 20 V, AO4485 ±20 V abs: 3.2x); Q4 emitter on PD_PG makes PVDD_EN AND PG; R8 100k holds PVDD_EN low through boot; firmware won't start below 14 V (gate drive 4.4 V). Q1 turn-off after PVDD_EN or PG drops is slow (R5 x C19 = 0.47 s) but SDZ goes low first (hifi_bsp_power_out) so no current flows through Q1 mid-ramp.
- **TPS54302 / SY8089 / LP5907**: values vs TI SLVSDG6C table 7-2 (10 µH, 100k/13k = 5.18 V, FF 47 pF after POWER-10); +5V 5.18 V vs mute's highest normal-op threshold 4.86 V (-20 °C): 0.32 V. LP5907 from +5V: 1.9 V headroom.
- **TPA3118D2 (U8)**: pins vs SLOS708G DAD pin table (MODSEL=GND BD, PLIMIT=GVDD no limit, MUTE=GND, AM2..0=GND, SYNC NC master); gain 20k/100k = 26 dB master (table 1); OC trip 7.5 A vs 19 V/4 Ω = 4.75 A peak (1.6x); min load 3.2 Ω typ with 10 µH/680 nF (ours 10 µH/1 µF, 1206 50 V X7R ~0.8 µF at 10 V bias); SDZ 10k/47 nF, VIH 2 V vs >= 2.8 V; FAULTZ 10k to 3.3 V.
- **ESP32-WROVER-E**: firmware pins vs netlist (check_pad_nets.py, 61 checks): PD_PG 4, PVDD_EN 18, SDZ 21, FAULTZ 23, VBUS_SENSE GPIO34/ADC1_6, PVDD_SENSE GPIO39/ADC1_3, I2C 33/32, squeezelite I2S 27/25/26/35, MCLK GPIO0, buttons 13/36/19; dividers 11.0 = 100k/10k. Straps: GPIO12 free (pull-down, 3.3 V flash), GPIO0 10k up + Q3, GPIO2/5/15 free. EN 10k/1 µF (Espressif HW guide). Auto-program matches DevKitC (EN transistor base<-DTR emitter<-RTS; IO0 base<-RTS emitter<-DTR). CH340B TXD(2)->GPIO3, RXD(3)<-GPIO1. GPIO36 glitch (errata 3.11) debounced 40 ms (round 2).
- **IR / buttons**: IRM-3638T with 100R/4.7 µF (Everlight app circuit), 10k pull-ups.

## Field issues searched

| issue | source | our board | how |
|---|---|---|---|
| CH224K board with 2820 µF straight on VBUS refused by Apple PD chargers; fix = soft-start switch gated by PG | github.com/sonocotta/esp32-audio-dock/issues/202 | immune (mostly) | Bulk behind Q1, closed after PG + 200 ms, 28 ms ramp; 13-16 µF at attach (R4-PAD-4). |
| CH224K trigger boards cycling on/off with every charger (brown-out during negotiation suspected) | goughlui.com 2026-07-09 HW-443 review | immune | CH224A VHV direct to VBUS (no VDD shunt resistor as on CH224K); VR 2.2-2.65 V. |
| PD hard reset with sink bulk back-feeding VBUS | USB PD tSafe0V (round 2 sim) | immune with >= 200 Ω source discharge | round 2 power field issue; unchanged board. |
| TPA3116/3118 power-off pop; TI: put SDZ low before the supply goes; DIY "supply-sense mute" circuits | TI SLOS708G 7.4; diyaudio.com threads 379831, 321342; forum.pedalpcb.com TPA3118 thread | handled | Firmware drops SDZ on VBUS < 13 V or PG loss within 10 ms (power_task); the sub out (separately powered sub) has the R3-AUDIO-2 supply-sense mute - same idea as the pedalpcb two-transistor circuit, whose author notes the same trip-vs-supply tuning trade (R4-PAD-1). |
| TPA3118 pop when muting in SPW mode | e2e.ti.com 618444 | n/a | Lite runs BD mode (MODSEL = GND). |
| TPA3118 output DC detect on DC-coupled inputs | SLOS708G 7.3 | immune | C60-C63 1 µF AC coupling. |
| ESP32 GPIO36/39 80 ns glitch on ADC power-up | ESP32 errata 3.11 | immune | 40 ms debounce (round 2 DIGITAL-6). |

## Tests built (docs/review-4/sims/pad/)

- `check_pad_nets.py` (python3 -I): 61 netlist/PCB/firmware checks: mute topology and values, MUTE_RES/MUTE_S membership, PCB pad nets of the 9 mute parts, PVDD switch + PG interlock, CH224A pins/Rset, SDZ/FAULTZ, ESP32 GPIO map vs hifi_board_lite.h and the squeezelite Kconfig defaults, divider constants, straps, auto-program. **0 fails.**
- `mute/run.sh` (hwde sim_run, 15 benches): `tb_trip_{cold,coldhi,zero,room,hot,hotlo}` (DC +5V sweep at -20/0/25/70 °C, hFE 60/416, R tol): error bounds pass (mute off >= 5.0 V everywhere, begins <= 4.86 V, full on >= 2.94 V); 3 warnings = R4-PAD-2. `tb_loss_*` (unplug; worst part; 20 ms and 200 ms falls; LOUT2 collapse at 2.0 V or 3.4 V; 470R fix; unmuted reference; 20-80 Hz "sub" band filter): error bounds pass; 2 warnings = the round-2-premise cases of R4-PAD-1. `tb_powerup`: band 16 mV pass. Exit 1 only on those 5 documented warnings.

## Known unknowns

- Where the ES8388's LOUT2 actually leaves VMID as AVDD (LP5907 out = +5V - ~20 mV) falls: unverified; R4-PAD-1's hot margin depends on it (bench test above).
- CH224A behaviour when the source drops the 20 V PDO on renegotiation (fallback voltage, PG state, its OVP while our 706 µF holds VBUS high on the negative transition): not in DS V2.0; unverified; firmware's 13 V/14 V floors protect the amp either way.
