# Simulations before the 2026-11-05 order

One table per board, one row per check. Every deck under `docs/sims/` reads or
asserts the values it uses against the current netlist
(`boards/*/kicad/*.net`), so a stale value makes the deck stop instead of
passing. Each deck writes its output next to it (`out_*.txt`). Rerun all of them
with:

```bash
eval "$(~/modules/bin/hwde env)"
for d in core/vals core/pvdd_switch core/rails core/thermal lite/vals lite/pvdd_switch \
         lite/rails lite/thermal amp/amp_stability; do $HWDE_PY docs/sims/$d.py; done
for b in core_ethclk core_i2sbck lite_i2sbck uf_sck; do $HWDE_PY docs/sims/si/run_benches.py docs/sims/si/$b; done
for d in si/vals si/core_pvdd/pvdd_hf_loop si/core_pvdd/gnd_return \
         si/lite_pvdd/pvdd_hf_loop si/uf_sck/ffc_orient uf/power; do $HWDE_PY docs/sims/$d.py; done
```

**What these are.** ngspice benches (power switch, SI), closed-form AC (amp
filter) and hand calculations on datasheet curves (rails, thermal). None of it
has been measured: no board has run yet. Thermal rows are **estimates**. A
"fail" in a row whose behaviour is covered by firmware (the governor, OTW) says
so in the row.

Margins are (limit − result), in the limit's units or as a % of the limit.

## core (Plus / Ultra main board)

| what it checks | deck | result | limit | margin | pass/fail | assumptions |
|---|---|---|---|---|---|---|
| Netlist values the benches use | `docs/sims/core/vals.py` | 70 checks, 0 mismatches (Q1 pins, R1/R4-R9/R17/R20/R43/R404, C1/C4/C5-C8/C10/C311, PVDD ceramics, D1/D2/D4/D5, Q2/Q3) | exact match | – | pass | – |
| PVDD soft start: t90 after EN | `docs/sims/core/pvdd_switch.py` A | 154-260 ms over C4 125/152/220 nF, VBUS 28/29.4 V, Vth 1.7/1.9/2.5 V | 500 ms (firmware `soft_start_ms`) | ≥ 240 ms | pass | AO4485 VDMOS model; C4 low corner 125 nF = 220 nF X7R 0603 at bias, low tolerance |
| PVDD soft start: Q1 peak power, energy | same, A | P peak 7.1-13.1 W, E 0.55-0.61 J, I peak 0.31-0.60 A | AO4485 SOA (via Tj row) | – | – | 470 µF ×3 bulk at nominal C |
| PVDD soft start: Q1 Tj (`soa_check`, AO4485) | same, A | 220 nF nominal: 114-123 °C at Ta 60 °C. Worst stacked corner (C4 125 nF, 29.4 V, Vth 1.7): **153 °C at Ta 60 °C**, 143 °C at Ta 50 °C | 150 °C | **−3 K** at Ta 60; +7 K at Ta 50; +27 K nominal | **FAIL at the stacked corner**, pass at nominal | Ta 60 °C is box air +5 K; single pulse. Posted to af884: C4 330 nF (or 2×220 nF) clears it; not changed here |
| PVDD gate held off on a PD 5→28 V step | same, B | Vsg max 0.61-1.05 V, PVDD ≤ 4 mV | Vsg < 1.5 V (Vth min 1.7) | ≥ 0.45 V | pass | 30 V/ms step, PVDD_EN low |
| PD hard reset with PVDD on (turn-off) | same, C | VBUS 0.160 V at 650 ms, PVDD 0.68 V | VBUS < 0.8 V at 650 ms (USB PD tSafe0V) | 0.64 V | pass | source discharge 200 Ω, amps shut down |
| Hot plug into a live 28 V source | same, D | VBUS_PD 28.82 V; VBUS 44.4 V; Vsg 1.18 V | VBUS_PD < 32 V (CH224Q); VBUS vs SY8303 40 V / INA186 42 V | 3.2 V; **−4.4 V** | pass (CH224Q); VBUS over: known item R3-POWER-6, non-compliant source only | 1 µH / 50 mΩ cable, SMBJ30A |
| Rail set points | `docs/sims/core/rails.py` | +5V 5.100 V, +3V3 3.327 V | 5 V / 3.3 V ± 3 % | – | pass | 0.6 V reference, nominal resistors |
| Rail load vs regulator rating | same | +5V 855 mA peak (28 % of SY8303 3 A); +3V3 680 mA peak (34 % of SY8089 2 A); +3V3A 50 mA (20 % of LP5907) | ratings | ≥ 66 % | pass | load budget est., includes ultra-front 250 mA on 5 V / 50 mA on 3V3 |
| Buck inductor peak current | same | L9 Ipk 1.28 A | Isat 2.9 A | 56 % | pass | L10 Isat not on file (SY8089 Ipk 0.86 A) |
| Rail ripple | same | +5V 6.9 mV p-p; +3V3 1.7 mV p-p | 1 % (51 / 33 mV) | ≥ 86 % | pass | output MLCCs at DC-bias low corner |
| Regulator Tj U4/U5/U6 | same | 83 / 74 / 72 °C | 150 / 125 / 125 °C | 67 / 51 / 53 K | pass | **estimate**; box air 55.3 °C |
| Box air (check_thermal enclosure) | `docs/sims/core/thermal.py` | 51.8 °C at 7.4 W; 56.7 °C with chips at 1/8 P + inductors + Q1 | PETG Tg 80 °C | 28 K at 7.4 W | pass | **estimate**; room 30 °C, G 0.340 W/K (walls + vents) |
| TPA3220 Tj, Plus 1/8 P 4 Ω (= governor 19 W) | same | 2.0 W/chip → 103 °C (23.5 °C/W), 108 °C (26 °C/W) | OTW 115 °C min | **7 K** | pass, thin | **estimate**; chip loss SLASEE9C fig 6-9, review-2 thermal.md 1.2 |
| TPA3220 Tj, Plus 1/8 P 8 Ω | same | 1.0 W/chip → 75 °C | 115 °C | 40 K | pass | **estimate** |
| TPA3220 Tj, Ultra under the 17 W governor, 4 Ω | same | 1.8 W/chip → 106 °C | 115 °C | 9 K | pass, thin | **estimate**; 2 ch per chip |
| TPA3220 Tj, Ultra 1/8 P 8 Ω | same | 1.5 W/chip → 96 °C | 115 °C | 19 K | pass | **estimate** |
| TPA3220 Tj, rated power continuous (77 W/ch 4 Ω, 43 W/ch 8 Ω; Ultra 1/8 P 4 Ω both zones) | same | 161-413 °C | OTE 145 °C | – | **fail, design-accepted**: governor + OTW hold the chips to the governed rows | **estimate** |
| Thermal-via screen under U15/U16 | same (check_thermal) | 20 / 21 vias | screen wants ≥ 36 | – | warning | the screen's copper credit is conservative |
| Bulk caps C5/C6/C311 ripple current | same (check_thermal) | 0.85 A rms at full power | 118 % of rating | – | warning, power-limited | only full continuous power; governed power is far lower |
| Q1 conduction | same | 5 A supply limit: 0.57 W → Tj 107 °C; governed: 57 °C | 150 °C | 43 K | pass | **estimate**; ~90 °C/W on this copper, R_DS(on) hot 23 mΩ |
| Shunt R1 10 mΩ 2512 | same | 0.25 W at 5 A | 3 W | 92 % | pass | – |
| Output inductors L1-L4 (Ultra L5-L8) | same | rated: 4.39 A rms, +21 K DCR rise, Ipk 6.8 A; 1/8 P: 1.55 A, +3 K | Isat 10 A | 32 % at rated | pass | core loss not on file (0.2-0.4 W each est.) |
| Output filter + Zobel, open load | `docs/sims/amp/amp_stability.py` | +15.8 dB @ 43 kHz nominal, +17.5 dB @ 48 kHz with caps at 14 V bias (Q ≈ 7.5) | +22 dB (core-zobel.md) | 4.5 dB | pass | L 10 µH/30 mΩ, C 1 µF 0805 → 0.78 µF, Zobel 4.7 Ω + 470 nF → 335 nF |
| Output filter, 4 Ω and 8 Ω resistive | same | 20 Hz-20 kHz within −0.49/+1.39 dB; min \|Z\| 3.75 Ω | ±1.5 dB; ≥ 3.2 Ω | 0.11 dB; 0.55 Ω | pass | – |
| Output filter, reactive speakers (4 Ω woofer with Le; 8 Ω 2-way with crossover) | same | peak +16.9 / +9.6 dB; filter moves min \|Z\| by +1.9 / −2.0 % | +22 dB; ≤ 5 % | 5.1 dB; 3 % | pass | Thiele-Small woofer (Re 3.2, Le 0.4 mH, fs 45 Hz); 2nd-order crossover at 2.5 kHz |
| Zobel resistor at a full-swing 20 kHz sine | same | 0.90-1.71 W | 0.1 W (0603) | – | fail for steady HF sine; covered by the firmware tone guard (`hifi_tone_check`); music is mW | – |
| SI deck values vs current `.net` / `.kicad_pcb` (lengths, widths, layers, series R) | `docs/sims/si/vals.py` | all match (length drift 0.1-0.2 mm) | max(0.5 mm, 2 %) | – | pass | review-5's 33 R on I2S_BCK is **not** on the board |
| PVDD HF loop at U15/U16 (switching-edge spike) | `docs/sims/si/core_pvdd/pvdd_hf_loop.py` | loop 3.5 nH; spike 8.1-8.2 V; peak 37.5-37.6 V | 37 V (TPA3220 abs max) | **−0.5 to −0.6 V** | **FAIL (estimate)** → R5-PA-3: bench gate ≤ 35 V, fallback 20 V contract (Decisions) | 7 A step in 3 ns on 29.4 V (review-3 convention, OCP-level current); 1206 ESL not counted. At the governed 19 W peak (≈ 3 A) the same loop gives ≈ 3.6 V spike. Same result as review-5 |
| 5 A ground return neck to J1 | `docs/sims/si/core_pvdd/gnd_return.py` | narrowest 5.21 mm (U16/C6), 5.44 mm (U15) | ≥ 3.5 mm (IPC-2152, 1 oz, ΔT 10 K) | 49 % | pass | only In1.Cu at that width counted |
| ETH_CLK 50 MHz at LAN8720A CLKIN, ring-back | `docs/sims/si/core_ethclk/` | 1.5 ns edge: low-state ring-back 0.73 V; 2 / 4 ns: 0.48 / 0.29 V; 1.0-1.4 ns: 0.84-1.19 V | ≤ 0.8 V (VIL) | 0.07 V at 1.5 ns | **pass, thin; FAIL below ≈ 1.45 ns** → bench measure at U9 CLKIN (Decisions) | 73 Ω lossless line, 20 Ω driver. Y2 (YSO110TR) gives only "4 ns max" edge → measure at U9 CLKIN at bring-up |
| ETH_CLK at ESP32 GPIO0 (2 ns) | same | high 2.88 V, low 0.43 V | ≥ 2.48 V; ≤ 0.8 V | 0.41 / 0.37 V | pass | – |
| I2S_BCK at ES7243E / PCM5102A / ES9018K2M, ring-back | `docs/sims/si/core_i2sbck/` | ≈ 0.9 V clear of both thresholds, all cases | 0.7 / 0.3 VDD | ≈ 0.9 V | pass | ESP32 driver 30-45 Ω, 1.5-2 ns |
| I2S_BCK overshoot | same | 3.85-3.89 V (1.5 ns, 30 Ω); 3.31 V with a 33 R series R | 3.9 V (VDD + 0.6, warning line) | **0.006 V** | warning, thin → firmware `GPIO_DRIVE_CAP_0` on BCK/LRCK/DOUT (Decisions) | estimate; the 33 R fix is not on the board |

## lite-main

| what it checks | deck | result | limit | margin | pass/fail | assumptions |
|---|---|---|---|---|---|---|
| Netlist values the benches use | `docs/sims/lite/vals.py` | 75 checks, 0 mismatches | exact match | – | pass | C59 ESR 0.3 Ω assumed |
| PVDD turn-on at 20 V: peak current, power | `docs/sims/lite/pvdd_switch.py` | 0.36-0.43 A; 4.9-6.3 W | 2 A; 20 W (100 ms SOA) | 78 %; 69 % | pass | AO4485 VDMOS model, Vth 1.7/1.9/2.5, MLCC typ/low |
| PVDD turn-on at 20 V: energy, t90 | same | 0.138 J; t90 72-126 ms | 0.4 J; 1 s (`pvdd_timeout`) | 66 %; 87 % | pass | – |
| PVDD turn-on at 20 V: Q1 Tj (`soa_check`) | same | 92-99 °C at Ta 60 °C | 150 °C | 51 K | pass | single pulse |
| PVDD turn-on at 15 V | same | 0.21-0.28 A, 2.0-2.9 W, 0.078 J, t90 94-179 ms, Tj ≤ 79 °C at Ta 60, Vsg end 4.67 V | as above; Vsg ≥ 3.5 V | 1.17 V on Vsg | pass | – |
| Gate held off on a PD 5→20 V step | same | Vsg max 0.78 V (MLCC typ), 1.02 V (low); PVDD < 3 mV | 1.7 V | 0.68 V | pass | 30 V/ms step |
| Hot plug at 5 V | same | VBUS 8.7 V, Vsg 0.2 V | 14 V; 1.2 V | 5.3 V | pass | ceramics only on VBUS |
| Hot plug into a live 20 V source | same | VBUS 31.6-32.2 V with D1 SMBJ20A (36-37 V without) | TPS54302 / TPA3118 abs max 30 V; CH224A 32 V | **−2.2 V**; −0.2 V | **over: known item R3-POWER-6**, non-compliant source only | 1-2 µH / 50 mΩ cable |
| PD hard reset (turn-off), 20 V and 15 V | same | VBUS 0.007 V at 650 ms; PVDD 0.2-0.43 V | 0.8 V | 0.79 V | pass | 200 Ω source discharge |
| Rail set points | `docs/sims/lite/rails.py` | +5V 5.18 V (5.03-5.34); +3V3 3.33 V (3.23-3.43) | SY8089 Vin 5.5 V; ESP32 3.0-3.6 V | **3 %**; 7.7 % / 4.8 % | pass, thin | ±2 % reference, 1 % resistors |
| Rail ripple | same | 29-33 mV; 9-10 mV (derated caps) | 50 mV | 34 % | pass | PFM light-load ripple not modelled |
| Rail load vs rating | same | +3V3 543 mA max; +5V 428 mA max | U2 3 A; U3 2 A; U4 250 mA | ≥ 73 % | pass | every peak coincident, ESP32 500 mA |
| Buck inductors L5/L6 peak | same | 1.20 A; 0.89 A | Isat 3.2 / 3.4 A | 62 % / 74 % | pass | L −20/−30 %, fsw −20 % |
| U4 LP5907 dropout, Tj | same | headroom 1.63 V; 68 °C | 0.25 V; 125 °C | 46 % | pass | **estimate** |
| U8 TPA3118 Tj, 1/8 P 4 Ω (1.75 W) | `docs/sims/lite/thermal.py` | 101 °C at 28 °C/W, 52 °C air; 113 °C at 35 °C/W, 62 °C air; 135 / 145 °C at check_thermal's 47.5 °C/W | 135 °C (OTE 150 − 15 K) | 34 K; 22 K; **0 / −10 K** | pass at 28-35 °C/W; **fail at the 47.5 °C/W screen** (1.7-2× pessimistic, review-2) | **estimate**; chip loss review-2 |
| U8 Tj, rated continuous | same | 163-472 °C | 135 °C | – | **fail, design-accepted** (governor THERMAL-3, chip thermal shutdown) | **estimate** |
| Regulators U2/U3/U4 Tj | same | U2 108.5 °C at 62 °C air, all peaks at once; U3 86 °C; U4 78 °C | 125 °C | 16.5 K | pass, thin | **estimate**; efficiency 0.85/0.90 |
| Q1 conduction | same | 4.4 A full power: 0.31 W → 84 °C at 62 °C air | 150 °C | 66 K | pass | **estimate**; ~70 °C/W |
| Inductors L1-L6 | same | L1-L4 full: 74 °C at 62 °C air; L5/L6 ≤ 72 °C | 125 °C | 51 K | pass | **estimate**; core loss is a guess |
| Output filter, open load (no Zobel) | `docs/sims/amp/amp_stability.py` | **+38.6 dB @ 52 kHz** (Q ≈ 85), caps at 10 V bias | – | – | **reported, not passed**: no Zobel, risk accepted in core-zobel.md; check at bring-up with speakers off | 10 µH/30 mΩ, 1 µF 1206 → 0.95 µF |
| Output filter, 4 Ω and 8 Ω resistive | same | within −0.60/+0.84 dB; min \|Z\| 4.06 Ω | ±1.5 dB; ≥ 3.2 Ω | 0.66 dB | pass | – |
| Output filter, reactive speakers | same | 4 Ω woofer +38.9 dB @ 53 kHz (looks open above 10 kHz); 2-way +18.0 dB; \|Z\| moved +1.9 / −1.3 % | ≤ 5 % on \|Z\| | 3.7 % | pass on \|Z\|; peaking as the open-load row | same speaker models as core |
| U8 PVDD HF loop (switching-edge spike), best cap C55 | `docs/sims/si/lite_pvdd/pvdd_hf_loop.py` | loop 7.0 nH; spike 17.5 V; peak 38.5 V | 30 V abs max (TPA3118) | **−8.5 V** | **FAIL (estimate, wide bracket)** → accepted: bench gate ≤ 27 V at U8 PVDD, fallback 15 V PD (Decisions) | 7.5 A (OCP) in 3 ns on 21 V; cap GND vias return 3.7-9.1 mm on B.Cu to the U8 GND vias. At the 9 W / 4 Ω rated peak (≈ 2.1 A) the spike is ≈ 5 V → ≈ 26 V. Pass at OCP needs ≤ 2.8 nH. No earlier Lite loop analysis existed |
| same, C51 / C52 | same | spike 21-24 V; peak 42-45 V | 30 V | −12 to −15 V | FAIL (estimate) | same |
| same, optimistic bound (all side caps in parallel, no mutual L) | same | peak 25.3-26.1 V (method B), 32-33 V (method A) | 30 V | ≈ 4 V / negative | thin / fail | optimistic bound only |
| I2S_BCLK U5 IO27 → ES8388 SCLK, ring-back (45.6 mm, no series R) | `docs/sims/si/lite_i2sbck/` | 10 cases; high ≥ 2.69 V | 2.31 V high / 0.99 V low | ≥ 0.38 V | pass | Z0 100 / 145 Ω (Hammerstad estimate, no field solve); ES8388 thresholds taken as 0.7/0.3 VDD |
| I2S_BCLK overshoot / undershoot | same | Z0 145 Ω: 4.03 / −0.73 V at 1.5 ns, 4.54 / −1.24 V at 1.0 ns; Z0 100 Ω: 3.69 V; 33 R fix at 1.0 ns: 3.78 V | 3.9 V / −0.6 V (warning line) | −0.13 V (145 Ω, 1.5 ns); +0.21 V (100 Ω) | warning; firmware already sets `GPIO_DRIVE_CAP_0` (not modelled) | estimate; a series R is not on the board |

## ultra-front

| what it checks | deck | result | limit | margin | pass/fail | assumptions |
|---|---|---|---|---|---|---|
| LCD SCK through the FFC, as built (49.2 mm, 33 R, 57.8 mm, 50 mm FFC, 20.4 mm) | `docs/sims/si/uf_sck/tb_sck_asbuilt.cir` | high 3.25 V, low 0.05 V; overshoot 3.77 V, undershoot −0.47 V | 2.31 V / 0.99 V; 3.9 V / −0.6 V | 0.94 V; **0.13 V** | pass; overshoot thin (warning line) | review-5 bench, values re-checked by `si/vals.py` |
| FFC orientation and pin map J3 (core) → J1 | `docs/sims/si/uf_sck/ffc_orient.py` | consistent | type-B (reversing) cable | – | pass | J1 has 7 GND pins on the netlist (review-5 said 5) |
| Backlight current (3 × 120 R) | `docs/sims/uf/power.py` | 38.7-63.8 mA | 80 mA (module max) | 20 % | pass | LED Vf 2.7-3.2 V |
| R13 / R17 / R18 dissipation | same | 54 mW each | 100 mW (0603) | 46 % | pass | – |
| RGB LED sink on TCA9535 | same | 4.2 mA/pin, 11.8 mA total | 25 mA/pin, 200 mA total | ≥ 83 % | pass | TCA9535 limits from memory, not re-read |
| IR receiver supply after R10 | same | 3.18 V at 1.2 mA | ≥ 2.7 V | 18 % | pass | – |
| FFC per-contact current | same | 38 mA max (+5V, 2 pins); 7.2 mA (+3V3) | 0.5 A | 92 % | pass | – |
| FFC drop incl. ground rise, 50 / 150 mm cable | same | +5V 14 / 23 mV; +3V3 5.6 / 8.9 mV | 100 mV / 66 mV (own limits, 2 %) | ≥ 77 % | pass | 0.3 mm × 35 µm conductors, 60-100 mΩ contacts |
| Backlight switch Q1 AO3400A | same | 0.2 mW, Tj 60 °C at Ta 60 °C; gate drive 3.0 V worst | 350 mW / 150 °C; Vgs ≥ 2.5 V | ≈ 100 %; 20 % | pass | R_DS(on) and Rth from memory, no datasheet in repo |
| J1 pad count and pitch | same | 22 pads, 0.500 mm | 0.5 mm FFC | – | pass | – |
| Load the front draws from core | (input to `core/rails.py`) | ≈ 76 mA on +5V, ≈ 15 mA on +3V3 | core rails budget uses 250 / 50 mA | – | core rails pass with the larger figure | – |

## Items that fail or are thin, and what covers them

- **Core Q1 soft start, stacked corner** (−3 K at Ta 60 °C, C4 at its low corner). Pass at nominal. Fix offered, not applied: C4 330 nF.
- **PVDD HF loop spike, both boards.** This is an estimate at OCP-level current. Lite fails by 8.5 V, core by 0.5 V. At rated or governed current both pass with margin. Probe PVDD at the amp pins at first power-up (at low volume).
- **Hot plug into a live non-compliant source** (R3-POWER-6). This is a known accepted item. It needs a source that switches on 20/28 V before PD negotiation.
- **Lite has no Zobel.** The open-load peak is +38.6 dB at 52 kHz. The risk was accepted in core-zobel.md. Check at bring-up with the speakers off.
- **Rated continuous power on all amps** goes over OTE in the enclosure. The design relies on the governor and OTW, and the governed rows pass with 7-9 K (core) and 22-34 K (lite, at 28-35 °C/W) margin. These are estimates.
- **Lite U8 at the check_thermal screen's 47.5 °C/W** reaches the 135 °C limit. review-2 considers that screen 1.7-2× pessimistic.
- **ETH_CLK below a 1.45 ns edge, and I2S overshoot on core and lite** are on or near the warning line. Measure the edges at bring-up.
- **Lite +5V set point** has 3 % margin to the SY8089 5.5 V Vin max. The 5.5 V figure is from memory.

## Decisions (af884, 2026-11-04; no copper change for this order)

- **Lite U8 PVDD HF loop: accepted, with a bench gate.**
  - The failing case needs a 7.5 A OCP edge through a single cap, which only happens on an output short.
  - At rated current, and with the caps working in parallel, the peak is 25-26 V.
  - Gate: scope U8 PVDD to its GND pin at 20 V into 4 Ω, near clip. The peak must stay ≤ 27 V.
  - Fallback: firmware requests 15 V PD instead of 20 V.
  - Next spin: put the 1 µF caps at the pins with GND straight to the EP (≤ 2.8 nH).
- **Core U15/U16 HF loop: already R5-PA-3.** Gate: ≤ 35 V on the bench. Fallback: a 20 V contract.
- **Core ETH_CLK: measured at bring-up.** The Y2 edge at U9 CLKIN must be slower than 1.45 ns. This is to go into `docs/bringup/core.md`.
- **Thin BCK overshoot: firmware fix.**
  - Core gets BCK/LRCK/DOUT at `GPIO_DRIVE_CAP_0`, as Lite already has. This replaces the 33 R that is not on the board.
  - Ultra-front SCK, at 0.13 V margin: pass, listed.

## How these were checked

Each deck was rerun on the current netlist before it was written down. Each one asserts the values it uses, so if a value goes stale the deck stops. Every result above is a model; none has been measured.

- **Limits revised after the first run.** `amp_stability.py` first applied one flatness and min-|Z| limit to every load. With the speaker models, that limit failed on the speakers' own impedance rather than on the board. The limit is now split: flatness and min-|Z| for resistive loads, and "filter moves the speaker's |Z| by ≤ 5 %" for the speaker models.
- **One rerun did not complete.** The independent rerun of `lite/pvdd_switch.py` never ran: it waited in the heavy queue until it timed out. Its output is from the original run.
