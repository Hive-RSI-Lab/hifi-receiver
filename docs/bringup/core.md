# Bring-up: core (Plus and Ultra main board)

This is the first-power plan for `boards/core`, both populations:

- **Plus:** `fab/BOM.csv`. It fits the knob SW1, the buttons SW2/SW3, the IR supply resistor R400, and the bananas J20/J21 (zone A L) and J26/J27 (zone B R).
- **Ultra:** `fab/ultra/BOM.csv`. It fits the FFC J3 to the front board in place of SW1-SW3 and R400, and all eight bananas J20-J27.

Both fit both amps, U15 (zone A) and U16 (zone B).

**Nothing in this plan has been run on hardware.** Each expected value comes from the netlist (`boards/core/kicad/core.net`), the design reviews (`docs/review-2/`) or a datasheet. A value marked *(est.)* is an estimate, so the first board sets the real figure. Write what you measure into the record sheet at the end and into `docs/known-unknowns.md`.

The board has **no TP test points**, so every probe point below is a pad: `C5.1` means pad 1 of C5. Pad 1 of a 2-pad part is the end the netlist names. Where it matters, this plan says which pad is the rail and which is GND. Check the orientation on the assembly drawing (`fab/` CPL) before you probe.

## Kit

- A bench supply, 0-30 V at 3 A or more, with a current limit you can set in 10 mA steps.
- A USB-C power breakout (VBUS/GND screw terminals) so the bench supply can feed J1. A plain one is fine: CC is left open, so the CH224Q makes no contract.
- A USB-C PD charger. 100 W (20 V) is the minimum; 140 W or more with EPR (28 V) is the target. Use a 240 W (5 A) EPR cable.
- A laptop with a USB-C port, `bench/bench.py` and an ESP-IDF build (`docs/bench.md`).
- An optional inline USB-C power meter, to read the laptop port's current.
- A multimeter with a mV range, and a 2-channel scope with 10x probes.
- **Never clip a scope ground to a speaker terminal.** The outputs are BTL, and both legs swing around PVDD/2. Use two probes and math A-B, or a differential probe.
- Dummy loads at 4, 6 and 8 Ω, rated 50 W or more each. You need two for Plus and four for Ultra.
- A thermocouple, taped to the board beside U15, and one more for U4 if you have it.
- An RCA line source, such as a phone with a DAC or a signal generator at 1 Vrms or less.
- A capture interface for `bench.py` (`--capture`). Feed it through a 10k/1k divider on each speaker leg; never connect a speaker leg to it directly.
- Fine clip leads (micro-grabbers), and one short clip lead for the PVDD test in step 3.5.

## Firmware hooks used

Use the console on the CH340 at 115200 baud: `hifi <cmd>`, or `api <op> [json]`, which takes the same ops as HTTP `/api/v1/<op>`.

| Hook | What it gives |
|---|---|
| `hifi info` | Board, firmware version, MAC |
| `hifi state` | `power{state, vbus_v, pg, contract{protocol, epr, v, i_max_a, w}, pvdd_on, pvdd_v, vbus_i_a}`, `amp{enabled, fault, faults, latched, temp_c}`, `governor{...}`, `audio{...}`, `net` |
| `hifi selftest` | pd_sink_i2c, pd_contract, expander_i2c, dac_i2c, adc_i2c, front_i2c (Ultra), amp_otw, pvdd_on, amp_no_fault, psram, heap, network |
| `hifi amp on\|off\|reset` | The amp path. It needs a PD contract of 9 V or more (firmware C4). |
| `hifi tone sine F dBFS [both\|left\|right] [s]`, `tone sweep`, `tone noise`, `tone silence`, `tone stop` | Test signals |
| `hifi volume N`, `maxvol N`, `mute on\|off`, `input <name>`, `sub <dB>` | Level and routing |
| `hifi ir learn <action>`, `ir forget` | IR remote |
| `api wifi {"ssid","password"}` | Join Wi-Fi (bench: `--wifi SSID:PW`) |
| `api capture {"source":"loopback"\|"line","frames":N}` | The ES7243E records the zone A speaker taps or the RCA input |
| `bench.py find / flash / selftest / tone / calibrate / sweep / soak / bringup` | `docs/bench.md`. `bringup` runs on core too (G1, fixed). |

The boot log on the console also prints the bus scan and the PD result:

- `I/O expander at 0x..`
- `line-in ADC at 0x..`
- `sub DAC on I2S1 ...`
- `source lists N fixed PDOs`
- `asked the charger for N V`

Build with `HIFI_BOARD=core firmware/tools/build.sh` for Plus, or `HIFI_BOARD=ultra` for Ultra. Flash with `bench/bench.py flash <build dir>`.

## Nets and where to probe them

| Net | Pads | Expected |
|---|---|---|
| VBUS_IN | R1.1, J1 A4/A9/B4/B9 | Supply volts |
| VBUS | R1.2, C10.1, C11.1, U4 pin 5 | VBUS_IN minus I × 10 mΩ |
| VBUS_PD (CH224Q supply) | C1.1 | ≈ VBUS |
| ISENSE_RAW / ISENSE | R3.1 / R3.2, C3.1 | 0.25 V/A nominal, about 0.264 V/A real (see 5.6) |
| PVDD | C5.1, C6.1, C7.1, Q1 pins 5-8 | 0 V until enabled, then ≈ VBUS |
| PVDD_G (Q1 gate) | Q1 pin 4 | VBUS when off; VBUS − ~14 V when on (D4 clamps at 15 V) |
| PVDD_PD | R5.2, R404.2, Q2 pin 3 | ≈ VBUS when off, ≈ 0 V when on |
| PVDD_ENG / PD_PG | Q2 pin 1 / Q2 pin 2, R20 | See 5.1 |
| OV_B (overvoltage trip base) | Q3 pin 2, R9.1 | ≈ 0 V below about 33 V VBUS |
| BUCK5_LX / BUCK5_BS | L9.1 / C12 | 1.05 MHz switch node |
| +5V | L9.2, C13.1, C18.1 | 5.10 V (0.6 × (1 + 15k/2k)) |
| BUCK3_LX | L10.1 | Switch node |
| +3V3 | L10.2, C16.1 | 3.33 V (0.6 × (1 + 100k/22k)) |
| +3V3A (LP5907 U6, pin 5) | C19.1 | 3.30 V ±1 % (datasheet) |
| +5VA (from +5V via FB1) | C21.1 | ≈ +5V |
| +3V3E (from +3V3 via FB2) | C22.1 | ≈ +3V3 |
| VREF | C64.1, R90.2 | +5VA / 2 ≈ 2.55 V (R90/R91 470R/470R; the README's 2.4 V is stale) |
| A_AVDD / A_GVDD (U15 pins 21/22, B_ on U16) | U15 pins 21/22 | ≈ 5.0 V each (from +5V through 4.7 Ω, review-2 audio.md) |
| ESP_EN | C32.1, U21 pin 2, U7 pin 3 | 3.3 V once out of reset |
| ETH_CLK = ESP32 IO0 | R31, U7 pin 25 | High at reset (R31 10k pull-up), then 50 MHz once PHY_RST is high |
| PHY_RST (Y2 OE) / PHY_NRST | R45 (2.2k to GND), Y2 pin 1 / C47.1, U9 pin 15 | Low at boot; PHY_NRST lags through R67 10k × C47 1 µF (τ 10 ms) |
| Speaker legs | J20-J27 (A_SPK_L±, A_SPK_R±, B_SPK_L±, B_SPK_R±) | Differential only |

---

## 1. Visual, then rail resistance with no power

Do this on each board, with nothing plugged in.

**1.1 Visual.** Use a loupe or microscope, top and bottom. Fourteen parts are on the bottom (ORDERING.md): C207-C210 and C307-C310 under the amps, C218/C220, D6/D9 at the USB-C, and D7/D8 at the RCA jacks.

- Check that U15 and U16 (TPA3220, DDV44) have no bridges between their 0.5 mm pins, and that pin 1 is where the drawing puts it.
- Check pin 1 on U7 (WROVER), U17 (TCA9539), U1 (CH224Q), U3 (INA186), U4/U5 (SOT-23-6/8 bucks) and U9 (LAN8720A).
- Check the polarity of C5/C6 (470 µF/50 V electrolytics) and D1 (SMBJ30A: cathode toward VBUS).
- Check that the banana collars are seated: J20/J21/J26/J27 on Plus, all eight on Ultra.
- Check that J1's through-hole shell pins are soldered.

If anything is wrong, fix it before power, or reject the board.

**1.2 Resistance to GND.** Use the meter's Ω range, black lead on GND (J1 shell, or the GND pad of C10). Read after the value settles: the big caps make it climb for several seconds. Then swap the leads and read again: a rail that reads low both ways is a short, while one that is low only one way is a body diode or an ESD clamp.

| Rail | Pad | Expected *(est.)* | Fail |
|---|---|---|---|
| VBUS_IN / VBUS | R1.1, R1.2 | > 50 kΩ, climbing (R10+R11 200k, R404+R5, U3, U4 off) | < 1 kΩ |
| R1 itself | R1.1 to R1.2 | ≈ 0 Ω (10 mΩ, below the meter) | Open |
| PVDD | C5.1 | > 10 kΩ after it settles; 1000 µF takes about 10 s to charge | < 1 kΩ |
| +5V | C13.1 | > 1 kΩ (R13+R14 17k, U5/U6 inputs, amp VDD) | < 100 Ω |
| +3V3 | C16.1 | > 500 Ω (ESP32 module, CH340B, PHY, codecs) | < 50 Ω |
| +3V3A / +5VA / +3V3E / VREF | C19.1, C21.1, C22.1, C64.1 | +5VA ≈ +5V; +3V3E ≈ +3V3; VREF ≈ 470 Ω to GND (R91); +3V3A > 500 Ω | Under a tenth of these |
| PVDD to VBUS | C5.1 to R1.2 | Open (Q1 off) | < 1 kΩ: Q1 shorted or a bridge on Q1 pins 1-3/5-8 |

Record the figures from the first boards; later boards should match them within about 20 %.

**On a fail:** lift one end of the suspect rail's ferrite or inductor (FB1, FB2, L9, L10) to split the rail, then measure again. The usual cause is a solder bridge at the amp's PVDD/OUT pins or at the WROVER.

## 2. First power: a bench supply at 5 V

**Connect:** bench supply → USB-C breakout → J1, with CC left open. Leave the speaker bananas, RCA, Ethernet and FFC unplugged.

**Limit:** set 5.0 V, 100 mA, output off. Turn it on and watch the current.

- **Expected *(est.)*:** a short inrush to the limit, then 30-90 mA. The WROVER sits in its boot ROM, or in the factory image if one is loaded. The PHY and its 50 MHz oscillator stay off, because PHY_RST is held low by R45. The amps stay in reset (R40/R41 100k pull-downs), and PVDD stays off.
- **If it sits at 100 mA with the voltage pulled down:** turn off and repeat step 1.2. A short drags it to 0 V; if the voltage only sags, raise the limit to 250 mA once. The ESP32's boot peaks can reach about 150 mA *(est.)*.
- **If it draws 0 mA:** check J1's VBUS pins and R1, and the breakout's wiring.

Once the current is steady, set the limit to 300 mA for the steps below.

## 3. Each rail in order

Do these steps on the bench supply, with CC open.

**3.1 VBUS.**

| Measure | Expected |
|---|---|
| R1.1 and R1.2 | Supply ± 50 mV |
| C1.1 (VBUS_PD) | ≈ VBUS |

On a fail: if R1.2 is low, check R1's solder. If C1 is low, U1 is pulling its supply down.

**3.2 +5V (U4, SY8303).** Measure at C13.1.

- **Expected at a 5 V input:** 4.5-4.95 V *(est.)*. U4 is in dropout; 5.1 V needs 6 V or more in.
- Raise the bench supply in steps to 12, 20 and 28 V, with the limit at 300 mA. At each step, +5V should be 5.10 V ±3 %: 0.6 V reference ±1 %, plus the R13/R14 tolerance. Input current falls as the voltage rises: about 40, 25 and 18 mA at 12, 20 and 28 V *(est.)*.
- **Stop at 29 V.** D1 (SMBJ30A) starts to conduct at about 33 V, and the overvoltage trip is near it.
- **On a fail:**
  - If +5V reads 0 V, check BUCK5_EN at R10/R11. It should be VBUS/2 (100k/100k) and is always above U4's enable threshold.
  - If +5V reads about VBUS, U4 has failed shorted, or the LX/FB pins are bridged. **Turn off at once:** the 5 V parts will be damaged.
  - If +5V is high by a fixed ratio, check R13 (15k) and R14 (2k).
- **Scope:** probe BUCK5_LX at L9.1 on a 20 MHz bandwidth limit, with the ground spring on C13's GND.
  - Expect a 1.05 MHz rectangle from 0 V to VBUS.
  - Record the overshoot and the ringing at 28 V, both now and again under amp load in step 6. This is U4's ringing open item: C12 sits below L9, so its BS and LX links are long tracks.
  - Ringing above VBUS + 10 V or more, or a bootstrap (BS-LX) dip under 3 V, goes to the hifi-receiver thread.

**3.3 +3V3 (U5, SY8089).** Measure at C16.1. Expect 3.33 V ±3 %. U5 runs from +5V and works in each case above, even at a 5 V input.

- On a fail: check BUCK3_FB against R15 (100k) and R16 (22k), and check L10.
- Scope L10.1: expect about a 1.5 MHz switch node *(est., SY8089A1 datasheet)*.

**3.4 The quiet rails.**

| Measure | Expected |
|---|---|
| C19.1, +3V3A | 3.30 V ±1 % (LP5907-3.3) |
| C21.1, +5VA | +5V − 20 mV *(est.)* |
| C22.1, +3V3E | +3V3 − 10 mV *(est.)* |
| C64.1, VREF | 2.55 V ±2 % (+5VA × 470/940) |
| U15 and U16 pins 21/22, AVDD/GVDD | ≈ +5V − 0.1 V |
| ESP_EN (C32.1) | 3.3 V; the TLV803S releases it about 200 ms after +3V3 *(datasheet)* |

On a fail: if +3V3A is low, check U6 (pin 1/3 in, pin 5 out) and C19. If VREF is off, check R90 and R91.

**3.5 PVDD off, then the switch with no firmware.**

1. With the bench supply at 28 V (CC open, so no contract and PD_PG stays high), PVDD should be off:

   | Measure | Expected |
   |---|---|
   | C5.1, PVDD | < 0.5 V (leakage only) |
   | Q1 pin 4, PVDD_G | ≈ VBUS (Vgs ≈ 0) |
   | R5.2, PVDD_PD | ≈ VBUS |
   | R9.1, OV_B | < 0.1 V |

   If PVDD follows VBUS: Q1 is shorted, Q2 is on, or PVDD_PD is pulled low by a bridge at Q2 or R404. Check Q2 pin 2 (PD_PG): it should be 3.3 V (R20), because the CH224Q has no contract. Turn off.

2. To test Q1 without firmware, turn off and set the limit to 1 A. Clip PVDD_PD (R5.2, or Q2 pin 3) to GND. The amps stay in reset through R40/R41. Scope PVDD at C5.1 and turn on at 28 V.
   - **Expected:**
     - PVDD rises to VBUS in about 60 ms *(est.)*. The README's 15 ms is stale.
     - The inrush is about 1000 µF × 28 V / 60 ms ≈ 0.47 A *(est.)*, so the supply shouldn't hit the 1 A limit.
     - Q1 pin 4 sits ≈ VBUS − 14 V (gate drive set by R4/R5, clamped by D4).
     - VBUS − PVDD < 50 mV at this current.
     - The added current with both amps in reset is < 30 mA *(est.)*.
   - **On a fail:**
     - If PVDD doesn't rise, check that Q1 pin 4 actually moves. If it doesn't, check R5 (470k) and the clip. If it moves but PVDD stays at 0, Q1 is open or its pins 5-8 aren't soldered.
     - If the supply goes to its current limit, look for a short on PVDD at the amps (step 1.2).
   - Remove the clip. PVDD should decay over a few seconds (the 1.1 s time constant). Don't move on until C5.1 reads under 5 V.

3. **Optional: the overvoltage trip divider.** With the clip removed and the limit at 100 mA, raise VBUS slowly from 28 V and watch OV_B (R9.1). It should stay under 0.1 V until D2 (BZT52C33) conducts. It should then rise to about 0.6 V (Q3 on) at a VBUS of 32-36 V *(est.)*. **Stop at 34.5 V**, and turn down at once if the current climbs: D1 is conducting.

   This checks only the divider. The trip itself, with PVDD on, needs PVDD enabled by firmware above 30 V, which a charger can't give. Leave that for a later spin with a firmware hook (gap G4).

## 4. The ESP32: flash, first boot, console

**Connect:** the laptop's USB-C port to J1. It powers the board at 5 V and carries the CH340B's USB (J1 A6/A7/B6/B7, with U2 as the ESD clamp). Use the inline power meter if you have one.

**4.1 Enumeration.**

- `bench/bench.py find` should list a CH340 port (VID 1a86, PID 7523); `lsusb` should too. Expect about 50-120 mA at 5 V from the port *(est.)*.
- **If no port appears:**
  - Check U8's VCC (pins 4/16 at 3.3 V).
  - Check D+/D- continuity from J1 (A6/B6, A7/B7) to U8 pins 5/6. U2 (USBLC6-2SC6 ESD) hangs on those lines from +5V; if U2 is shorted, the lines read low to GND.
  - Try the other cable orientation: the A and B pairs are both wired.

**4.2 Flash.**

1. Build: `HIFI_BOARD=core firmware/tools/build.sh`, or `ultra`.
2. Flash: `bench/bench.py flash firmware/build`. Add `--dry-run` to see the esptool line.
3. Auto-program uses Q4 (RTS → ESP_EN) and Q5 (DTR → IO0, which is ETH_CLK).

**On a fail** ("Failed to connect", or "wrong boot mode"):

- Scope IO0 at R31 and ESP_EN at C32.1 together while esptool resets the chip. IO0 must be low when EN rises.
- Check that Y2 isn't driving IO0: PHY_RST (Y2 pin 1) should be low, held by R45 2.2k.
- Fall back to holding IO0 to GND by hand at R31's IO0 pad while you short ESP_EN to GND (C32.1). Release EN, then IO0.

**4.3 Scope IO0 at EN rise (open item digital:2).** Trigger on ESP_EN rising (C32.1). On channel 2, probe IO0 at U7 pin 25 or R31.

- **Normal boot:** IO0 should sit high (R31) and quiet, with no 50 MHz on it, for at least 1 ms after EN rises. The ROM samples it there.
- **During auto-program:** IO0 should be low across the EN edge.

Save both traces into `docs/known-unknowns.md`.

If you see 50 MHz on IO0 at reset, Y2's OE is not held low. Check R45 and PHY_RST at U17 pin 9: the expander resets to inputs, so R45 must win.

**4.4 First boot.** Open the console at 115200, for example with `python -m serial.tools.miniterm PORT 115200`, and press reset (or toggle EN).

- **Expected:**
  - The IDF banner.
  - `I/O expander at 0x74` (TCA9539, 0x74-0x77 by strap).
  - `line-in ADC at 0x10` or `0x14`.
  - `sub DAC on I2S1 ...`
  - On a laptop port: no contract, or a 5 V contract; laptops seldom list more.
  - No reboot loop.
- **On a fail:**
  - `no I/O expander answers` means U17 or the I2C bus. Check SDA (IO13) and SCL (IO14), each pulled up by R34/R35 to 3.3 V, and U17 pin 3, its RESET, which is tied to ESP_EN.
  - A brownout reset loop: measure +3V3 at C16.1 on the scope across the Wi-Fi start. A dip under 3.0 V means U5 or its input.
  - PSRAM errors point at the WROVER itself.

**4.5 Console checks.**

`hifi info` should give the board (`core` or `ultra`), the firmware version and the MAC.

`hifi selftest` on the laptop port should give:

| Item | Expected on a laptop port | If it fails |
|---|---|---|
| pd_sink_i2c | pass | U1 not answering at 0x22: check C1.1 (VBUS_PD) and U1 pins 2/3 |
| pd_contract | **fail, expected** (no contract of 9 V or more) | |
| expander_i2c | pass | See 4.4 |
| dac_i2c | pass (ES9018K2M at 0x48) | See 5.3 |
| adc_i2c | pass | See 5.4 |
| front_i2c | Ultra only, 0x27. It fails with no front board on J3. | |
| amp_otw | pass | OTW stuck low: U15/U16 pin 3, or the wired-OR 10k |
| pvdd_on, amp_no_fault | **fail, expected** on 5 V (PVDD is refused; C4) | If PVDD comes on at 5 V, that's a design or firmware fault: stop and report it |
| psram | pass (≥ 2 MB) | |
| heap | pass | |
| network | fail until Wi-Fi is set (step 5.6) | |

`hifi state` should show:

- `power.pg` false or a 5 V contract;
- `pvdd_on` false;
- `pvdd_v` and `amp.temp_c` null, because core has no PVDD sense and no amp temperature (gap G2);
- `vbus_i_a` 0, because the reading is floored below about 0.4 A.

`hifi amp on` must be **refused** on the laptop port.

## 5. The blocks

**5.1 PD contract and PVDD.**

- **Connect:** the PD charger through the 240 W cable into J1. The console is gone (J1 carries only power now), so use Wi-Fi or HTTP: set Wi-Fi in step 5.6 first, or take the boot log on a second board. If the laptop can stay connected through a USB-C PD hub, that works too.
- **Watch:** the boot log, `source lists N fixed PDOs` and `asked the charger for 28 V` (EPR) or `for 20 V`. Then check `hifi state` → `power.contract`.

| Measure | Expected |
|---|---|
| VBUS at C10.1 | The contract voltage ±5 %: 26.6-29.4 V for 28 V; 19-21 V for 20 V |
| `contract.v` | 28 (EPR) or 20 |
| `contract.epr` | true at 28 V |
| `pg` | true about 3 s after the request |
| PD_PG at Q2 pin 2 | < 0.4 V (contract good) |
| +5V | 5.10 V |

Then run `hifi amp on`.

| Measure | Expected |
|---|---|
| PVDD_EN (U17 pin 7) | 3.3 V |
| PVDD_ENG (Q2 pin 1) | 3.3 V |
| PVDD_PD | Falls to about 0 V |
| PVDD at C5.1 (scope, 20 ms/div) | Ramps to VBUS in about 60 ms *(est.)* |

Then run `hifi amp off`. PVDD should fall, faster than the 1.1 s bleed if Q3's fast turn-off (D5/PVDD_OFFB) works. Record the fall time.

**Fallback (open item audio:38):** on a charger without EPR, or a 100 W charger, the contract should come out at 20 V. Record what each charger gives: `contract{v, i_max_a, w}`, and the PDO count from the boot log.

**On a fail:**

- If no contract is made, check CC1/CC2 at J1 A5/B5 with the scope during plug-in: you should see the BMC bursts. Also check D6/D9 orientation on the bottom side, and the CH224Q's CFG1 (R2 210k).
- If the contract is good but PVDD stays off, find which of PD_PG, PVDD_EN and PVDD_ENG is wrong. PVDD_ENG low with PVDD_EN high means Q3's OV unit is on: check OV_B.
- If PVDD rises slowly or Q1 gets hot, Q1's Vgs is short of drive. Check Q1 pin 4 against VBUS.

**5.2 Current sense, INA186 gain (U3).** The firmware assumes 0.25 V/A (25 V/V × 10 mΩ). The real gain should be about 5.6 % high, about 0.264 V/A *(est., README design note)*: R3 and C3 load the output, and the shunt pads add resistance. It is a compile-time `#define`, `ISENSE_V_PER_A` in `esp/hifi_board_core.h`.

**Measure** under a steady load of 1-3 A, such as the amps playing into dummy loads in 5.5, or the bench supply with the clip in step 3.5 and a load on PVDD:

| Measure | What it is |
|---|---|
| V(R1.1 − R1.2) | mV range: the shunt drop |
| V(C3.1) | ISENSE, to GND |
| V(R3.1) | ISENSE_RAW |
| VBUS current | From the bench supply or a USB-C meter |

**Expected:**

- V(C3.1) / V(R1) ≈ 26.4 *(est.)*, within 25-27.5.
- V(C3.1) ≈ 0.264 V/A × I.
- `hifi state` `vbus_i_a` within 10 % of the meter after the correction.

Note the gain you measure, and the firmware constant to set, in the record sheet. That constant is a firmware change (gap G3).

**On a fail:** if ISENSE reads 0, check U3's supply (pin 3 at 3.3 V) and ISNS_P/ISNS_N at C9. If ISENSE reads full scale, U3's inputs are swapped or bridged.

**5.3 DAC (U10 ES9018K2M, U11 OPA1678, U12 PCM5102A sub).**

- **Static checks:**
  - +3V3A at U10 pins 15-17.
  - DAC_DVDD at C56: the ES9018K2M's internal regulator, about 1.2 V *(est., datasheet)*.
  - DAC_EN (U17 pin 8) at 3.3 V once the firmware enables audio.
  - Y1 at 49.152 MHz on DAC_MCLK, U10 pin 7. Use a 10x probe with the ground spring, and expect a measured frequency within ±50 ppm.
- **Test:**
  1. `hifi input` to a digital source, then `hifi tone sine 1000 -20 both 10`.
  2. Measure MAIN_L/MAIN_R (U11 pins 1/7) with the meter's AC range. They also reach U14 pins 2/4.
  3. Expect about 0.2 Vrms *(est.)*: −20 dBFS of a 2 Vrms full scale. DC should sit at the +5VA/2 bias, ≈ 2.55 V, matching VREF.
- **Sub:** `hifi sub 0`, then a tone sine of 50 Hz. Expect 0.1 Vrms *(est.)* at J4 pins 1/3 (SUB1_OUT, SUB2_OUT): −20 dBFS of 1 Vrms.
- **Open item digital:15, whether DAC I2C needs MCLK:** dac_i2c in the self-test passes or fails with Y1 running. To test without MCLK, Y1 has to be held off: its pins 1/4 go to +3V3, so there is no enable pin. This can't be done without a rework. Record "I2C works with MCLK present" and leave the row open.
- **Open item digital:31, the PCM5102A at 64 fs:** if the sub tone is clean (no clicks, the right pitch), mark it confirmed.
- **On a fail:**
  - If dac_i2c fails, check U10 pins 3/4 and 28 (DAC_EN). The ES9018K2M holds I2C in reset while DAC_EN is low.
  - If there is no audio, check BCK (IO2), LRCK (IO4) and DOUT_MAIN on U10 pins 25-27 with the scope.

**5.4 Line-in ADC (U13 ES7243E).**

- **Static:** +3V3 at U13 pins 1/5 and +3V3A at pin 12.
- **Test:** feed a 1 kHz tone at a known level, 0.5 Vrms, into J5 L/R. Then run `api capture {"source":"line","frames":4096}`. Expect `stats.rms_fs` of about 0.5 / full-scale-Vrms.
- **Open item audio:35:** record the full-scale volts you measure; the datasheet figure is unconfirmed. Then raise the level until `clipped` > 0, and record that level too.
- **Loopback:** `api capture {"source":"loopback"}` during a tone records the zone A speaker taps through TAP_SEL (expander P17) and U20. It needs PVDD and the amp on: do it in 5.5. Zone B has no taps (gap G6).
- **DJ bypass:** set `hifi input` to the DJ path. It drives DJ_DIRECT (P02 → Q6 → U14). The RCA signal should reach AMP_IN_L/R (U14 pins 10/6) straight, with no ESP32 in the path. Check it with a meter at low level.
- **On a fail:** if adc_i2c fails, check that U13 answers at 0x10 or 0x14. If capture returns zeros, check I2S_DIN at U7 pin 4.

**5.5 The amps, at low volume into dummy loads.**

**Connect:** the charger (28 V contract, or 20 V), with a dummy load on each fitted output pair:

- **Plus:** 8 Ω across J20/J21 (zone A L) and across J26/J27 (zone B R).
- **Ultra:** all four pairs.

Set `hifi maxvol` low first, with a −40 dBFS tone and volume about −30 dB.

1. Run `hifi amp on`, then `hifi state`.
   - Expect `amp.enabled` true and `fault` false.
   - AMP_A_RST and AMP_B_RST (R40.1, R41.1) at 3.3 V. They are released together in one I2C write, for the controller/peripheral sync.
2. **Idle, no signal:**
   - Each leg sits at about PVDD/2 DC, 14 V at 28 V. Each pair reads < 50 mV DC differential (meter across the load).
   - PVDD current rises by about 30-60 mA per chip *(est.)* while idle.
   - Scope one leg against GND (one probe, which is safe on a single leg): expect switching at the FREQ_ADJ rate set by R214 (100k to GND; read the frequency off the TPA3220 datasheet's FREQ_ADJ table and record it). B runs as a peripheral on A's clock, so the switching should match on U16.
3. **Tone:** run `hifi tone sine 1000 -30 left 10`, then `right`.
   - Measure AC volts across each load, and check that only the selected channel plays. On Plus, left is zone A and right is zone B.
   - The gain is 24 dB (controller and peripheral). The single-ended input gain (open item audio:21) is to be measured: record Vrms out over Vrms at MAIN_L.
   - `predict.py` has core's `fs_vpk` unset; this measurement sets it (gap G5).
4. **Polarity:** play a tone of 50 Hz at −30 dBFS, and use two probes as differential (A − B) on each pair against MAIN_L. Mark L+ as in phase or inverted; the AUDIO-8 review says inverting gain is expected. Record it.
5. **Loopback capture:** run `api capture {"source":"loopback","frames":4096}` during the tone. `stats.rms_fs` × `v_per_fs` should match the meter within 10 % *(est.)*. Then run `bench.py calibrate --vrms <meter>` to save `volts_per_fs`.

**On a fail:**

- If FAULT goes low at once:
  - Check for shorted outputs or a load short.
  - Check that the bootstrap caps C222-C225 and C320-C323 are fitted.
  - Check AVDD/GVDD on pins 21/22.
- If OTW is stuck, check U15/U16 pin 3 and the wired-OR.
- If one zone is silent, check the RESET pin (U15/U16 pin 10) and OSCM/OSCP between the chips (pins 12/13).
- If there is a large DC offset on a pair, check the input caps C218/C220 (bottom side) and the GAIN strap R212/R213.

**5.6 Wi-Fi and Ethernet.**

- **Wi-Fi:** on the console, run `api wifi {"ssid":"...","password":"..."}`. `hifi state` → `net` should show it joined, `bench.py find` should find the board by mDNS, and selftest `network` should pass. If it fails, check the antenna keep-out: no metal near the WROVER's end.
- **Ethernet:**
  - With a cable to a switch, the firmware sets PHY_RST (P05).
  - Scope: Y2 pin 3 (ETH_OSC) should give 50 MHz ±50 ppm. PHY_NRST at C47.1 should rise about 10 ms after PHY_RST (firmware C9, R67 × C47).
  - J2's link LED should light, `net` should show an Ethernet IP, and a ping should get through.
- **Open item:** the HR911105A centre taps are taken as pins 4/5. J2 pins 4/5 on +3V3E should read 3.3 V. If the link comes up at 100 Mbit/s and holds a 1-minute `iperf` (or a large HTTP capture), mark it confirmed.
- **On a fail:**
  - Check that the PHY has +3V3E at pins 1/19 and PHY_VDDCR at pin 6 (1.2 V, internal).
  - Check that MDIO and MDC toggle at boot.
  - Check that ETH_CLK reaches U7 pin 25 through R60 33R.

**5.7 IR, knob and buttons.**

- **Plus:**
  - Turn SW1 and press SW1, SW2 and SW3. The firmware's controls should act: `hifi state` volume changes, and input and mute change.
  - Scope ENC_A/ENC_B (U17 pins 15/16) for clean quadrature, and EXP_INT (IO39, U17 pin 1) going low on each change.
  - **IR:** U18 has +3V3 through R400 at IR_VCC (C400.1). IR_IN (U7 pin 7) idles high (R58) and pulses low on a remote press. Then run `hifi ir learn vol_up` and press the remote.
- **Ultra:** these come from the front board over J3 (ENC_A/B, BTN, IR_IN, LCD). R400 is not fitted, so U18 is unpowered. Bring up the front board in its own plan; on core, check only J3's +5V (pins 3/18) and +3V3 (pins 2/19).
- **LED_STATUS** (U17 pin 11 → R401 → D3) should light as the firmware sets it.
- `hifi state`'s `controls.expander` shows the expander's input bits (ENC_A/B, ENC_SW, BTN_1/2, FAULT, OTW), `controls.last` the last button, knob step or remote action, and `ir.last_code` the last raw remote code. Hold a button and read the bits, then release and read again. `hifi led test` holds LED_STATUS on for 30 s.

## 6. Open items, measured

**6.1 Thermal (firmware C3, README step 6).**

1. Tape a thermocouple to the board's top copper beside U15, and a second one beside U4 if you have it. Run with the board in the open, then again in the vented box.
2. **8 Ω soak:** play 1/8 power into 8 Ω for 10 min. Using predict.py's 43 W at 1 %, that's about 5.4 W, or 6.6 Vrms across the load *(est.)*. Run `bench.py soak --ohm 8 --temp-manual`, or tone sine 1000 at the level that gives 6.6 Vrms. Expect no OTW, and the board temperature under 85 °C *(est. Tj ≈ 84 °C in a 45 °C box from 26 °C/W)*.
3. **4 Ω soak:** repeat at 1/8 power into 4 Ω, with the governor on. Expect no FAULT; the governor's `limit_events` may climb.
4. `amp.temp_c` is null on core, so log the thermocouple by hand (`--temp-manual`).
5. Log U4's case temperature across the run too: this is open item thermal:18, the U4 load. Read the +5V load as ISENSE minus PVDD, or estimate it from `governor.in_w`.

**6.2 The INA186 gain.** Done in 5.2. Record it, and file the firmware change to `ISENSE_V_PER_A` (gap G3).

**6.3 U4 ringing.**

- Repeat step 3.2's scope of BUCK5_LX at 28 V with the amps playing (the 5.5 tone). Use the ground spring; don't use the clip lead.
- Record the peak and the ring frequency.
- If it peaks above about 40 V *(est.: U4's abs-max range)*, report it with the trace.

**6.4 IO0 at EN rise.** Done in 4.3.

**6.5 PD fallback.** Done in 5.1, on each charger you have.

**6.6 ES7243E full scale, TPA3220 single-ended gain, the HR911105A centre taps, the PCM5102A at 64 fs.** Covered in 5.4, 5.5, 5.6 and 5.3. Write each result into its row in `docs/known-unknowns.md`.

**6.7 The power sweep.** Once 6.1 passes, run `bench.py sweep --loads 4,6,8 --governor both`. This fits `predict.py` core's 77/55/43 W at 1 % THD+N on 28 V. It needs a capture through the 10k/1k divider and `volts_per_fs` calibrated (5.5 step 5).

**6.8 PVDD spike at the amp pins (R5-PA-3, `docs/sims.md`).** The sim puts the switching-edge spike at U15/U16 PVDD at 37.5 V against the TPA3220's 37 V abs max. That figure is an estimate for a 7 A edge in 3 ns, the OCP-level current. At the governed peak (about 3 A) it gives about 3.6 V of spike.

- **Probe:** PVDD at U15 pins 29-31 (then 36-38, and U16 the same), with the ground spring on the nearest GND pad. Use full scope bandwidth, not the 20 MHz limit.
- **Load:** the 28 V contract, 4 Ω dummy loads, a tone at the governor's limit (step 5.5, then raise the volume to the governor cap).
- **Pass:** peak ≤ 35 V.
- **On a fail:** set the firmware to ask for a 20 V contract, not 28 V, and report the trace in the hifi-receiver thread.

**6.9 ETH_CLK edge at U9 CLKIN (`docs/sims.md`, SI).** The sim gives low-state ring-back over VIL (0.8 V) for Y2 edges faster than about 1.45 ns. Y2's datasheet only gives "4 ns max".

- **Probe:** U9 pin 5 (ETH_CLK, CLKIN) with the ground spring, at full bandwidth (≥ 500 MHz probe and scope if you have one).
- **Measure:** the 10-90 % rise and fall times, and the lowest ring-back level while the clock is low.
- **Pass:** edges slower than 1.45 ns, and ring-back under 0.8 V.
- **On a fail:** report it with the trace. If the link drops or shows CRC errors, a series resistor at Y2's output (or a slower part) is the next-spin fix.

## Firmware and bench gaps

These are the steps above that need support which doesn't exist yet. G4 and G7 are closed (`hifi i2c`, `controls` in `hifi state`); G1 is closed in `bench.py`.

- **G1 (fixed): `bench.py bringup` couldn't pass on core.** Stage 1 on 5 V now
  marks `pd_contract`, `pvdd_on`, `amp_no_fault` and (before `--wifi`)
  `network` as expected; the bench reads `state.power.contract`; the amp
  check passes when PVDD stays off after the request; and the controls
  item in the checklist is the model's own (Plus: knob and two buttons,
  Ultra: the front board's knob and three buttons). Host-tested only
  against a firmware-shaped fake, not yet on a board.
- **G2: no PVDD or VBUS voltage sense, and no amp temperature.** On core, `state.power.pvdd_v` and `amp.temp_c` are null. The PVDD ramp (3.5, 5.1) and the soak (6.1) need a scope and a thermocouple; `bench.py soak` needs `--temp-manual`.
- **G3: ISENSE can't be calibrated at run time.**
  - `ISENSE_V_PER_A` is a compile-time `#define`.
  - `state` has no raw ISENSE volts.
  - `vbus_i_a` reads 0 below about 0.4 A (`ISENSE_RAW_FLOOR`), so idle current can't be read.
- **G4: closed:** `hifi i2c <addr> <reg> [val]` reads and writes any register (`docs/firmware.md`). Writes are refused while audio plays or the amps are on, so forcing PVDD for the trip test still needs its own hook. Before, the bench couldn't:
  - read the CH224Q's registers or PDO list on demand (they appear only in the boot log, which is lost once the charger takes J1);
  - toggle PHY_RST, LED, TAP_SEL or DAC_EN by hand;
  - read the ES9018K2M or ES7243E registers;
  - force PVDD for the overvoltage-trip test.
- **G5: core's `fs_vpk` is unset in `predict.py`** until the firmware fixes its gain, so the sweep can't judge level against prediction.
- **G6: no zone B taps.** Loopback capture covers zone A only, and the Ultra 4-channel capture (plan item 23) isn't built.
- **G7: closed:** `hifi state` has `controls.expander` (ENC_A/B, ENC_SW, BTN_1/2, FAULT, OTW), `controls.last` and `ir.last_code`.

**Doc notes found on the way:**

- `boards/core/README.md` gives VREF as 2.4 V (it is 2.55 V) and the PVDD ramp as 15 ms (about 60 ms).
- `blocks.md` says TCA9555 at 0x20, screw terminals and a 50 MHz Y1. The board has a TCA9539 at 0x74-0x77, bananas, and Y1 at 49.152 MHz (Y2 is the 50 MHz).
- `test-api.md` says CH340C; the board has a CH340B.
- known-unknowns power:19 still names the TCA9555. The TCA9539 has no internal pull-ups, which R40-R45 cover.

## Record sheet

Fill in one per board: serial or MAC, population (Plus or Ultra), date.

| Step | Item | Measured | Pass? |
|---|---|---|---|
| 1.2 | VBUS / PVDD / +5V / +3V3 to GND (Ω) | | |
| 2 | 5 V idle current (mA) | | |
| 3.2 | +5V at 5 / 12 / 20 / 28 V in | | |
| 3.2 | BUCK5_LX overshoot at 28 V (V) | | |
| 3.3-3.4 | +3V3, +3V3A, +5VA, +3V3E, VREF | | |
| 3.5 | PVDD ramp time (ms), inrush (A), Q1 Vgs | | |
| 4.3 | IO0 at EN rise (trace saved) | | |
| 4.5 | selftest items | | |
| 5.1 | Contract V / i_max per charger; PVDD off time | | |
| 5.2 | INA186 V/A | | |
| 5.3 | MAIN_L/R at −20 dBFS; sub; Y1 ppm | | |
| 5.4 | ES7243E full scale (Vrms) | | |
| 5.5 | Amp gain (dB), polarity, idle current, `v_per_fs` | | |
| 5.6 | Wi-Fi / Ethernet link, Y2 ppm | | |
| 5.7 | Knob, buttons, IR, LED | | |
| 6.1 | 8 Ω and 4 Ω soak: max temperature at U15 and U4; OTW/FAULT | | |
| 6.3 | U4 ringing under load | | |
