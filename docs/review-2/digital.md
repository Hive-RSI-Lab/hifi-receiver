# Digital / firmware / RF review: lite-main and core

Reviewer domain: `digital`. Date 2026-10-28. Fresh-eyes review: I read the
netlists (`lite-main.net`, `core.net`), the PCBs (through KiCad 10.0.6 `pcbnew`) and vendor
datasheets. I did not read the design's README/architecture/reviews. The firmware pin map comes
from the firmware branch `origin/hive/261028-165619-70c34f28b3` (`firmware/components/hifi/esp/hifi_board.h`,
`firmware/patches/0001-hifi-receiver-hooks.patch`, `firmware/upstream.lock`: squeezelite-esp32
`master-v4.3` @ `ee1720c`, ESP-IDF v4.4.8). That firmware only covers Lite; core has no firmware yet.

This file is written one block at a time. Each section is final when written.

## Findings, by severity

| ID | Board | Sev | One line |
|---|---|---|---|
| DIGITAL-1 | core | **blocker** | GPIO0 strap sees the 50 MHz RMII oscillator at reset (TCA9555 pull-up divider at 1.65 V, plus P05 persisting across ESP32 resets). Fix: TCA9539 with RESET = ESP_EN, R45 10k |
| DIGITAL-2 | core | **major** | TCA9555 internal 100k pull-ups fight the 100k pull-downs, so the outputs are undefined from power-up until firmware writes them (and stay at their last value through an ESP32 reset) |
| DIGITAL-3 | core | **major** | Main DAC ES9018K2M (GPIO5) and sub DAC PCM5102A (GPIO12) take separate I2S data lines with shared BCK/LRCK |
| DIGITAL-14 | lite-main | **major** | TPA3118 SDZ driven through R54 100k: spec IIH 50 uA allows only 12 uA to reach VIH 2 V. R54 to 10k + 47 nF |
| DIGITAL-19 | core | **major** | No antenna keepout on core: GND/+3V3 copper on all 4 layers and 2 tracks under about 5.4 mm of the antenna |
| DIGITAL-4 | core | minor | ES9018K2M MCLK 50 MHz +/-30 ppm is exactly at the internal-DVDD limit (an external DVDD is required when MCLK > 50 MHz) |
| DIGITAL-5 | core | minor | LAN8720A nRST (tau 1 ms) is released before the 50 MHz oscillator is guaranteed running (3 ms max): C47 to 1 uF or an MDIO soft reset |
| DIGITAL-6 | lite-main | minor | Vol-down button on GPIO36: errata GPIO-3.11 80 ns glitches when ADC1 powers up (firmware reads ADC1) |
| DIGITAL-16 | core | minor | LCD_CS is on the expander; squeezelite needs a native CS. R57 becomes a pull-down (CS always asserted), cs=-1 |
| DIGITAL-18 | lite-main | minor | Antenna end is 0.55 mm inside the board edge and ANT_KEEPOUT (F+B) leaves the area under it copper-free (pass), but the cleared strip extends only 2.2-2.5 mm past the module sides, with GND pour beyond (Espressif asks for >=15 ... |
| DIGITAL-20 | core | minor | RMII traces run 82-126 mm (the PHY at x about 64 is far from the ESP32 at x about 137), unterminated |
| DIGITAL-7 | core | note | The auto-program transistor collector is on the 50 MHz ETH_CLK net and adds its C_ob (S8050 about 5 pF) to the clock load |
| DIGITAL-8 | core | note | VDDCR has 1uF + 100nF (datasheet: 1uF + 470pF) |
| DIGITAL-9 | core | note | GPIO15 (MTDO strap) is routed to the FFC as LCD_DC |
| DIGITAL-10 | core | note | TCA9555 pull-ups light the LED faintly (about 15 uA) and enable the backlight before firmware runs |
| DIGITAL-11 | both | note | Flash budget for recovery + two OTA slots + NVS/settings on the 8 MB N8R8 fits |
| DIGITAL-12 | core | note | No ESP32 GPIO is free on core. **Recommended pin for the new TS5A23157 select: ES9018K2M U10 pin 23 (GPIO2, reg 8 = 4'd7/4'd15)** through an S8050 level shifter to +5VA |
| DIGITAL-13 | both | note | CH340C -> CH340B (C81010) is a drop-in on both boards |
| DIGITAL-15 | lite-main | note | The IR receiver supply filter (100R/4.7uF, fc 339 Hz) and 3.3 V output pull-up are fine. |
| DIGITAL-17 | core | note | BCK ratio: PCM5102A PLL needs 32/48/64 fs; ES9018K2M needs DATA_CLK = 2*i2s_length*FSR matching reg 1 (32-bit default) |
| DIGITAL-21 | core | note | TOSLINK into ES9018K2M GPIO1: S/PDIF needs MCLK > 386*FSR, so with 50 MHz the limit is 129.5 kHz (96k ok, 176.4/192k won't lock) |

Counts: blocker 1, major 4, minor 6, note 10. Fix details are in each block below; `digital.json` carries the same findings, the known unknowns and the tools wanted.

---

## Block 1: core, the RMII clock on GPIO0 and the TCA9555 default states (DIGITAL-1, DIGITAL-2)

**Netlist (core.net):**
- `/ETH_CLK` = U7 pin 25 (IO0) + U9 pin 5 (LAN8720A XTAL1/CLKIN) + R60 33R (from Y2 OUT) + R31 10 k pull-up to +3V3 + Q5 collector (auto-program).
- Y2 pin 1 (Tri-state/OE) = `/PHY_RST` = TCA9555 U17 P05 (pin 9) + R45 100 k to GND + R67 10 k to LAN8720A nRST (C47 100 nF).
- TCA9555 pin 3 (A2), pin 2 (A1) and pin 21 (A0) are all GND, so the address is 0x20.
- The other P0 outputs: P00 AMP_A_RST (R40 100 k to GND), P01 AMP_B_RST (R41 100 k), P02 DJ_DIRECT (R42 100 k, plus R121 10 k into the Q6 base), P03 PVDD_EN (R43 100 k, plus R6 1 k into the Q2 2N7002 gate with R7 100 k to GND), P04 DAC_EN (R44 100 k; ES9018K2M RESETB and PCM5102A XSMT), P06 LCD_BL (no resistor), P07 LED_STATUS (470R to LED).

**Datasheet facts:**
- TI TCA9555, SCPS200E (Apr 2019), §5: "identical to the TCA9535, except for the inclusion of the internal I/O pull-up resistor". Fig. 23 shows the pull-up as 100 kΩ (nominal, no tolerance given). Pin table: every P-port pin is an input at power-on. There is no RESET pin; the registers reset only on POR (VPORF 0.75 to 1 V).
- Y2/Y1 = LCSC C717684 = YXC OT322550MJBA4SL (YSO110TR series). Its datasheet p1: tri-state Enable is "high voltage or floating", ≥ 70 % VDD (2.31 V). Disable is ≤ 30 % VDD (0.99 V). Start-up is 3 ms max. Floating = enabled implies an internal OE pull-up of unspecified value.
- ESP32-WROVER-E datasheet v2.4 §4: GPIO0 = 1 gives SPI boot, GPIO0 = 0 with GPIO2 = 0 gives download. "At system reset, the latches sample the bit values of their respective strapping pins". Strap hold time tH ≥ 1 ms after CHIP_PU goes high (Table 5).

**Calculation:**
- At power-up, P05 is an input with a 100 k pull-up against R45's 100 k pull-down: V = 3.3 × 100/(100+100) = **1.65 V**. If the oscillator's internal OE pull-up is, say, 1 MΩ, then 3.3 × 100/(100 + 90.9) = 1.73 V. Either way it is inside the undefined band 0.99 to 2.31 V, and "floating = enable" makes enabled the likely outcome.
- EN rises through 10 k / 1 uF (tau = 10 ms) and crosses VIH_nRST = 0.75 × 3.3 = 2.475 V at t = 13.9 ms. The oscillator starts within 3 ms. So if OE is read as high, GPIO0 is toggling at 50 MHz when the straps latch, and the latched value is random (GPIO2 sits low through its internal pull-down, since the BCK net only has inputs on it). The result is about a 50 % chance of booting into UART download mode: a silent device that only a power cycle recovers.
- Even with OE solidly low at POR, once firmware sets P05 high to run Ethernet, P05 **stays high through every ESP32 reset**: an EN reset (auto-program circuit, reset button), RTC WDT, brown-out of the ESP32 only, a panic reboot, or `esp_restart()`. Every one of those resets latches the straps with the clock running on GPIO0. Firmware can turn the clock off before a deliberate `esp_restart()` (shutdown handler), but not on a watchdog or panic reset. For a box that is "plugged in and left", that means a random hang on every crash recovery.

**DIGITAL-2 margins at power-up (TCA9555 100 k pull-up against the external pull-downs):**
- PVDD_EN: 100 k pull-up against R43 100 k ∥ (R6 1 k + R7 100 k) = 50.2 k, so 3.3 × 50.2/150.2 = **1.10 V** on the 2N7002 (Q2) gate. JSCJ 2N7002 VGS(th) is 1.0 to 2.5 V at ID = 250 uA. A minimum-Vth part therefore carries more than 250 uA at 1.10 V (10 % above Vth min, and Vth falls about 4 mV/°C when hot). Q2 only needs about 2.5 V / R4 100 k = 25 uA through R5 to pull the AO4485 gate past its threshold. So PVDD can switch on at power-up before firmware has decided anything. **Margin: negative for low-Vth parts.**
- AMP_A_RST/AMP_B_RST at 1.65 V: TPA3221 RESET thresholds still need to be cited from SLASEH5 (see the known unknowns). 1.65 V is mid-rail, so the amps may come out of reset uncontrolled.
- DAC_EN at 1.65 V: ES9018K2M VIH = DVCC/2 + 0.4 = 2.05 V and VIL = 0.4 V (ES9018K2M datasheet v3.7, DC characteristics), so it is undefined. PCM5102A XSMT (VIH 0.7 × DVDD = 2.31 V, VIL 0.3 × DVDD = 0.99 V) is also undefined.
- DJ_DIRECT: Thevenin 1.65 V / 50 k feeding R121 10 k into the Q6 base gives Ib = (1.65 - 0.62)/60 k = 17 uA. With S8050 hFE 200 to 350, Ic can reach 3.4 to 6 mA, which is more than the 0.5 mA that R122 (5 V / 10 k) supplies. Q6 saturates, so DJ_SEL is low at power-up and the analog switch starts in the opposite state to the "all pulled low" intent.

**Fix (DIGITAL-1 and DIGITAL-2 together, no layout change beyond one net):**
1. Replace U17 TCA9555PWR with **TI TCA9539PWR** (same TSSOP-24, same pinout except pin 3 = RESET instead of A2; no internal pull-ups; SCPS202C §8.3.2 and pin table; address 1110 1A1A0 = **0x74** with A0 = A1 = GND). Re-net pin 3 from GND to `/mcu/ESP_EN`. Every ESP32 reset (EN low) then also resets the expander to all-inputs, and the external pull-downs hold OE, PVDD_EN, the amp RESETs and DAC_EN low until firmware writes them. tW(RESET) min is 6 ns and VIL is 0.3 × VCC; EN is held low by C32 and Q4 far longer than that. The register map is identical to the TCA9555/PCA9555/PCA9535, so the firmware only changes the address (0x20 to 0x74). LCSC number for TCA9539PWR: I need to look it up (listed as an unknown).
2. Lower **R45 from 100 k to 10 k**. The YSO110TR has an internal OE pull-up of unknown value, and 10 k keeps OE below 0.99 V as long as that pull-up is ≥ 24 k: 3.3 × 10/(10 + 24) = 0.97 V. A TCA9539 output driving 10 k sinks/sources 0.33 mA, well inside its 10 mA rating.
3. Contingency if the swap is refused: keep the TCA9555 but change all the 100 k pull-downs (R40 to R45) to 10 k. That gives 3.3 × 10/110 = 0.30 V, below every VIL above (2N7002: 0.30 V vs Vth min 1.0 V, a 0.7 V margin). Cost: 0.3 mA per pin when the pin is driven high. This fixes DIGITAL-2 and the POR half of DIGITAL-1, but **not** the reset-persistence half. For that, the firmware must also drive P05 low from an `esp_register_shutdown_handler` and accept the hang risk on WDT/panic resets.

---

## Block 2: coordinator additions

### 2a. Flash budget on ESP32-WROVER-E-N8R8 (8 MB): recovery + two OTA slots + NVS/storage (DIGITAL-11)

**Inputs.** The flash is 8 MiB = 0x800000 = 8 388 608 B (WROVER-E datasheet v2.4 Table 1, N8R8 = 8 MB Quad SPI flash). The image sizes come from the firmware branch's own README (`firmware/README.md` l.65, built 2026-10-28 with IDF v4.4.8): `squeezelite.bin` **2.69 MB** and `recovery.bin` **1.38 MB**. I did not rebuild these, so the sizes are unverified. I take MB as MiB for the worst case: 2.82 MB and 1.45 MB in bytes. The current table (`firmware/board/lite-main/partitions.csv`) has a factory recovery of 2 MiB and a single `ota_0` of 4 MiB, so it does not yet have two OTA slots.

Fixed overhead below the first app: bootloader 0x1000 to 0x8000, partition table at 0x8000, nvs 0x9000 (16 KiB), otadata 0xD000 (8 KiB), phy_init 0xF000 (4 KiB). The first app starts at 0x10000 (64 KiB used). App partitions must be 64 KiB aligned.

**Proposed table (fits, every slot with a margin):**

| Name | Type/Sub | Offset | Size | Holds | Fill (worst case, MiB) | Margin |
|---|---|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x4000 (16 KiB) | Wi-Fi/PHY cal | n/a | n/a |
| otadata | data/ota | 0xD000 | 0x2000 | n/a | n/a | n/a |
| phy_init | data/phy | 0xF000 | 0x1000 | n/a | n/a | n/a |
| recovery | app/factory | 0x10000 | 0x1A0000 (1.625 MiB) | recovery.bin 1.38 MB | 1.38/1.625 = 85 % | 0.245 MiB (15 %) |
| ota_0 | app/ota_0 | 0x1B0000 | 0x300000 (3.0 MiB) | squeezelite.bin 2.69 MB | 2.69/3.0 = 90 % | 0.31 MiB (10 %) |
| ota_1 | app/ota_1 | 0x4B0000 | 0x300000 (3.0 MiB) | squeezelite.bin 2.69 MB | 90 % | 0.31 MiB (10 %) |
| settings | data/nvs | 0x7B0000 | 0x40000 (256 KiB) | squeezelite config + hifi | n/a | n/a |
| coredump | data/coredump | 0x7F0000 | 0x10000 (64 KiB) | panic dumps (useful for DIGITAL-1-style field hangs) | n/a | n/a |

The total ends at 0x800000 exactly. The overall arithmetic: 64 KiB overhead + 1.625 + 3.0 + 3.0 MiB + 256 KiB + 64 KiB = 8.0 MiB.

**Margin and verdict.** Everything fits. The tightest number is the OTA slot at **10 % (0.31 MiB)**, because the core build adds the Ethernet (LAN8720) driver, TCA9539/I2S1 code and an ES9018K2M control set; I estimate +60 to 150 KiB, not measured. If 10 % is too thin, two alternatives:
- Shrink `settings` to 0x10000 (squeezelite's settings NVS is 64 KiB upstream). That moves 192 KiB into the OTA slots: 2 × 3.09 MiB, 87 %, 13 % margin.
- Drop Bluetooth from **recovery** only (recovery doesn't need A2DP). Upstream recovery.bin is then about 1.0 to 1.1 MB, so recovery can be 0x120000 and each OTA slot 0x340000 (3.25 MiB, 83 %, 17 % margin).

**Firmware caveat (not hardware):** upstream squeezelite-esp32 assumes `factory` (recovery) + one `ota_0`. Recovery writes `ota_0`, and the app reboots into recovery to update. Two OTA slots need the update path changed to `esp_ota_get_next_update_partition()` and rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). Otherwise `ota_1` is never written. PSRAM (8 MB) does not affect flash.

### 2b. A free control line on core for a new TS5A23157 select (DIGITAL-12)

**ESP32 pin census (core.net, U7).** The WROVER-E brings out 26 GPIO-capable pads. Pads 17 to 22 and 27/28 are NC (flash GPIO6 to 11 and PSRAM GPIO16/17, correctly unused). The 24 usable GPIOs are all assigned:

| GPIO | Net | Notes |
|---|---|---|
| 0 | ETH_CLK | strap, EMAC REF_CLK in (fixed) |
| 1 / 3 | U0TXD / U0RXD | console/CH340C |
| 2 | I2S_BCK | strap |
| 4 | I2S_LRCK | |
| 5 | I2S_DOUT_MAIN | strap |
| 12 | I2S_DOUT_SUB | strap (MTDI, flash voltage) |
| 13 / 14 | I2C_SDA / I2C_SCL | |
| 15 | LCD_DC | strap (MTDO) |
| 18 / 23 | MDIO / MDC | |
| 19, 21, 22 | RMII TXD0, TX_EN, TXD1 | EMAC fixed |
| 25, 26, 27 | RMII RXD0, RXD1, CRS_DV | EMAC fixed |
| 32 / 33 | LCD_SCLK / LCD_MOSI | |
| 34 | ISENSE (ADC1) | input-only |
| 35 | IR_IN | input-only |
| 36 | I2S_DIN | input-only |
| 39 | EXP_INT | input-only |

**There is no free ESP32 GPIO.** The only way to get one is to give something up: for example, LCD_MOSI/SCLK could move onto the expander (too slow for SPI), or the sub DAC could share GPIO5's data (see DIGITAL-3), which frees GPIO12, but GPIO12 is the MTDI strap and must stay low at boot. None of these is "safe at boot, not a strap, not PSRAM, not EMAC".

**Recommendation: use ES9018K2M (U10) pin 23 GPIO2**, which is unconnected today, as the select source.
- ES9018K2M datasheet v3.7, Register #8 (0x08), bits [7:4] `gpio2_cfg`: **4'd7 = output low, 4'd15 = output high**. The default after RESETB is 4'd1 (DPLL lock status output). While RESETB (DAC_EN) is low the pin is tri-stated (Standby Mode section). Address 0x48, register 8. squeezelite-esp32's `dac_controlset` JSON init can write it (`{"reg":8,"val":0x70 | gpio1_cfg}` for low, `0xF0 | gpio1_cfg` for high), and the hifi component can toggle it at run time over I2C.
- Level: VOH ≥ DVCC - 0.2 = 3.1 V at 100 uA (DC table). The existing TS5A23157 (U14) runs from +5VA, where VIH = 0.7 × V+ = 3.5 V (TI SCDS165F, 5 V table), so a 3.1 V high is **not** a valid input, and that would hold for ESP32 GPIOs too (VOH min 2.64 V). The new select therefore needs the same NPN level shifter the design already uses for DJ_SEL (Q6): **S8050 (C2146) with a 47 k base resistor from U10 pin 23, a 100 k base pull-down, and a 10 k collector pull-up to +5VA driving the new TS5A23157 IN1/IN2**. Base current: (3.1 - 0.7)/47 k = 51 uA, inside the 100 uA VOH test condition. The collector needs 0.5 mA, which takes hFE ≥ 10 against the S8050's 200 to 350, so it saturates with a 20× margin. (If the new switch runs from 3.3 V instead, VIH = 0.7 × 3.3 = 2.31 V, and U10 GPIO2 can drive IN directly through a 100 k pull-down: margin 3.1 - 2.31 = 0.79 V.)
- Default state: while U10 is in reset (DAC_EN low; with the DIGITAL-1 fix, that covers POR and every ESP32 reset), GPIO2 is tri-stated, the 100 k pull-down turns the NPN off, and IN is pulled high (5 V). Choose which TS5A23157 throw is the safe/muted path for IN = high. Between RESETB release and the firmware's reg-8 write, the pin shows DPLL-lock status, so firmware must write reg 8 first in the init list, before any I2S starts.
- Caveats: the select only works while the ES9018K2M is out of reset and its 50 MHz MCLK (Y1, always enabled) runs, because ESS register access needs MCLK. Both builds (Plus and Ultra BOMs) fit U10, so the line exists on both. If a future build drops the ES9018K2M, the line goes with it.
- Fallback with no new active part: if the new switch's sense can follow DJ_DIRECT or DAC_EN, put its IN on the existing Q6 collector (DJ_SEL).

---

## Block 3: USB-serial, auto-program and EN, both boards (CH340C to CH340B swap: DIGITAL-13)

**Netlist.** The two boards are identical in topology.
- Lite: U6 CH340C. Pin 16 VCC and pin 4 V3 both on +3V3. Pin 5 D+ goes to J1 A6 and B6, pin 6 D- to J1 A7 and B7: both orientations of the USB-C are joined, with no series resistors. Pin 2 TXD goes to ESP RXD0 and pin 3 RXD to ESP TXD0. Pin 13 DTR# goes to /DTR, pin 14 RTS# to /RTS. Pins 7, 8, 9 to 12 and 15 are unconnected.
- Auto-program, Lite: Q2 MMBT3904 has E = RTS, B = R24 10 k from DTR, C = ESP_EN. Q3 has E = DTR, B = R25 10 k from RTS, C = IO0. EN: R20 10 k to +3V3, C22 1 uF to GND. IO0: R21 10 k pull-up.
- Core: U8 CH340C, wired the same way (pins 7, 8 and 15 unconnected; U2 USBLC6-2SC6 on D+/D-, pins 1/6 and 3/4, VBUS pin 5 on +5V). Q4 S8050 (EN) has E = RTS, B = R32 10 k from DTR. Q5 S8050 (IO0) has E = DTR, B = R33 10 k from RTS. R30 10 k / C32 1 uF on EN, R31 10 k on IO0 (ETH_CLK).

**Checks:**
- CH340 datasheet (WCH, V3D) §4 and §5.1: at 3.3 V, V3 must tie to VCC. **Pass on both boards.** "UD+/UD- connect directly … without a series resistor". **Pass.** The C and B both have an internal clock, so no crystal is needed.
- The topology is the Espressif DevKitC / esptool "classic reset" two-transistor cross-coupling. Truth table: when DTR# and RTS# are both high or both low, both transistors are off, so the board does not reset when the port opens. Linux and Windows assert DTR and RTS together on open, which gives that both-equal state. DTR# low with RTS# high puts IO0 low. RTS# low with DTR# high puts EN low.
- esptool timing: EN is held low for 100 ms, then IO0 is held low for a further 50 ms (DEFAULT_RESET_DELAY) after EN is released. EN rises with tau = 10 ms (10 k × 1 uF) and reaches VIH_nRST = 0.75 × 3.3 = 2.475 V at **13.9 ms**. The strap hold is tH ≥ 1 ms (WROVER-E ds v2.4 Table 5), so IO0 must stay low until 14.9 ms. Margin 50 - 14.9 = **35 ms (3.4×)**. Pass.
- EN low level: a saturated NPN gives Vce(sat) ≤ 0.2 V against VIL_nRST max 0.6 V (ESP32 ds v5.3 Table 5-3). Margin 0.4 V. Discharge time is 1 uF through about 20 Ω: 20 us, against tRST ≥ 50 us. The 100 ms hold gives a 2000× margin. Power-up: tSTBL ≥ 50 us, and EN takes 13.9 ms, so the margin is large. Espressif's own recommendation for this RC is 10 k / 1 uF.
- A port-open glitch (when the host sets DTR and RTS in two separate ioctls) can briefly turn on Q2/Q4. 1 uF behind an NPN discharges in microseconds, so a reset on open is possible with some terminal programs (Arduino/PlatformIO style). This is the known DevKitC behaviour and not a board fault. **Note.**

**CH340C to CH340B (C81010) swap (DIGITAL-13): it is a drop-in on both boards. No layout or netlist change.**

| SOP-16 pin | CH340C | CH340B | Lite U6 | core U8 | Verdict |
|---|---|---|---|---|---|
| 1 to 6 | GND, TXD, RXD, V3, UD+, UD- | same | wired | wired | identical |
| 7 | NC ("must be suspended") | RST#, input, active low, built-in pull-up | unconnected | unconnected | OK: the internal pull-up holds it out of reset. Optional 100 nF to GND for noise, not needed |
| 8 | OUT# (output) | NC ("must be suspended") | unconnected | unconnected | OK |
| 9 to 12 | CTS#, DSR#, RI#, DCD# (inputs) | same | unconnected | unconnected | identical (internal pull-ups) |
| 13 / 14 | DTR# / RTS# outputs | same | auto-program | auto-program | identical, so the auto-program circuit is unchanged |
| 15 | R232 (input, built-in pull-down) | TNOW (output, high while transmitting) | unconnected | unconnected | OK: an output left open; it must **not** be tied to GND or VCC (it is not) |
| 16 | VCC | VCC | +3V3 | +3V3 | identical |

Source: WCH CH340 datasheet V3D §4 pin table (SOP16 column) and §5.1 ("CH340C/N/K/E/X/B chips have built-in clock generators"; "the CH340B chip also provides an active low-level external reset input pin"). The CH340B's internal EEPROM only holds the USB serial number and strings. Host drivers (VID 0x1A86 / PID 0x7523) and esptool behaviour are the same.

LCSC on 2026-10-28: C81010 = CH340B, SOP-16, stock 4 627. Note that C84681 (CH340C) showed stock 109 248 on the same LCSC API call, so "EOL at JLC" may be a JLC basic/extended-library status rather than a real EOL. Either part works, and the CH340B costs nothing in design terms.

---

## Block 4: lite-main, ESP32-WROVER-E pin use (DIGITAL-14, DIGITAL-15, DIGITAL-6)

Firmware expectations (`hifi_board.h` and the HIFI_LITE Kconfig defaults in `0001-hifi-receiver-hooks.patch`):
- `dac_config = model=ES8388,bck=27,ws=25,do=26,di=35,sda=33,scl=32,i2c=16,mck=0`
- buttons on 13/36/19 with `pull:false`
- `14=ir:nec,22=green:1`
- PD_PG 4, PVDD_EN 18, AMP_SDZ 21, AMP_FAULTZ 23, ADC GPIO34 for VBUS

**Every one matches lite-main.net** (U5 pins 12/10/11/7, 9/8, 16/4/31, 13, 36, 26, 30, 33, 37, 6).

| GPIO (pad) | Net / load | Boot/strap behaviour | Margin / verdict |
|---|---|---|---|
| 0 (25) | I2S_MCLK: ES8388 MCLK in, R21 10 k pull-up, Q3 collector | 10 k pull-up, so 1 at reset gives SPI boot. ES8388 MCLK is an input. In normal use the MCLK output only exists after boot, and an EN or system reset returns the IO_MUX to input, so no clock is present when the straps latch | Pass. 3.3 V × 10 k/(10 k + 45 k internal) is moot: the external 10 k dominates. esptool pulls it low through Q3, sinking 0.33 mA |
| 2 (24), 5 (29), 15 (23) | unconnected | internal default pulls (2 = 0, 5 = 1, 15 = 1) | Pass |
| 12 (14) MTDI | unconnected | internal pull-down gives VDD_SDIO = 3.3 V, matching the N8R8's 3.3 V flash/PSRAM (WROVER-E ds v2.4 Table 1/4) | Pass. Nothing on the net can pull it high |
| 16/17 (27/28), 6 to 11 (17 to 22) | NC | PSRAM/flash | Pass |
| 34 (6) | VBUS_SENSE 100 k/10 k + 100 nF | input-only, ADC1_CH6 | 20 V → 1.82 V against the 11 dB range limit of about 2.45 V (IDF 4.4 recommended): 25 % margin. A 28 V EPR contract would read 2.55 V, past the linear range but below the 3.6 V maximum |
| 39 (5) | PVDD_SENSE 100 k/10 k + 100 nF | ADC1_CH3 | as above |
| 35 (7) | I2S_DIN ← ES8388 ASDOUT | input-only, driven | Pass |
| 36 (4) | BTN_VOLDN, R60 10 k pull-up, SW1 to GND | input-only, no internal pull, so the external 10 k is required (fitted) | Pass, but see DIGITAL-6 (errata GPIO-3.11, 80 ns glitch on ADC1 power-up) |
| 13 (16), 19 (31) | BTN_VOLUP / BTN_PLAY, 10 k pull-ups | n/a | Pass (firmware `pull:false` relies on the 10 k) |
| 14 (13) | IR_IN, R63 10 k pull-up, IRM-3638T OUT | ADC2 pin, used as digital only | Pass |
| 4 (26) | PD_PG (CH224A open-drain PG, R4 10 k), also the Q4 emitter | n/a | Pass |
| 18 (30) | PVDD_EN, R8 100 k to GND, R7 10 k to Q4 base | 0 V at reset | Pass. Q4 conducts only when PVDD_EN = 1 **and** PG = 0 (hardware AND). PVDD_EN low with PG high puts V_EB = 3.3 V against the MMBT3904's 6 V V_EBO: 45 % margin |
| 21 (33) | AMP_SDZ_IO, R52 100 k to GND, R54 100 k series to SDZ | 0 V at reset, so the amp is in shutdown | **DIGITAL-14** (below) |
| 23 (37) | AMP_FAULTZ, R53 10 k to +3V3 | input | Pass |
| 22 (36) | LED_DRV, 1 k to red LED D2 | n/a | I = (3.3 - 1.9)/1 k = 1.4 mA, far below the 40 mA IOH limit. Firmware names it "green": cosmetic note |
| 25/26/27 (10/11/12) | I2S LRCK/DOUT/BCLK to ES8388 | ADC2 pins used digitally | Pass |
| 32/33 (8/9) | I2C SCL/SDA, 2.2 k to +3V3; ES8388 (CE = GND gives 0x10) and CH224A via 0R R2/R3 (0x22 or 0x23) | n/a | Pass: no address clash. 5 V pull-ups: none. CH224's internal CFG2/3 pull-ups accept 3.3 V (CH224 ds v2.1 §5.2.2) |
| 1/3 | U0TXD/U0RXD to CH340 | console | Pass |

ADC2: no analog use of an ADC2 pin, so Wi-Fi does not conflict. ADC1 is only on 34 and 39. **Pass.**

**MCLK on GPIO0, firmware side:** `output_i2s.c` at ee1720c, lines 363 to 370: with `mck=0` it routes I2S0 CLK_OUT1 to GPIO0 by writing `PIN_CTRL = 0xFFF0`. Line 302 sets `use_apll = true`, so MCLK is APLL-derived (256 fs: 11.2896 MHz at 44.1 kHz, 12.288 MHz at 48 kHz). README l.206/208: "mck=0|1|2 … by default GPIO0 is used as MCLK". ES8388 is a recognised model (README l.210). **Supported.** Precedent: the Espressif LyraT uses GPIO0 for ES8388 MCLK with the same auto-program transistors.

**DIGITAL-14 (lite-main, major): TPA3118 SDZ is driven through 100 k.** TI SLOS708G §6.3: VIH(SDZ) ≥ 2 V and IIH ≤ **50 uA** at VI = 2 V. To reach 2 V through R54 = 100 k from a 3.3 V GPIO (VOH min 2.64 V at 40 mA, about 3.2 V at uA), the input current must be ≤ (3.2 - 2.0)/100 k = **12 uA**. The datasheet allows 50 uA, so a part at the limit sits near 3.2 - 50 uA × 100 k = < 0 V, i.e. clamped somewhere around 1 V: the amp never leaves shutdown, or it chatters. Typical leakage is far lower, so most boards will work on the bench; but this is a spec violation by 4×. TI's 10 V/ms SDZ slew limit (§6.1 abs max) is probably why a series resistor was used, but 100 k into roughly 5 pF gives τ = 0.5 us and does not meet it either.
- **Fix:** R54 100 k → **10 k** (C25804, already on the BOM). The worst-case drop is 50 uA × 10 k = 0.5 V, so SDZ ≥ 2.7 V against VIH 2 V: 0.7 V margin. Add **C = 47 nF (C1622 0603 X7R) from SDZ to GND**: τ = 0.47 ms, initial slew 3.3 V/0.47 ms = 7 V/ms < 10 V/ms. Shutdown delay is τ·ln(3.3/0.8) = 0.67 ms, acceptable for a pop-free mute because firmware also mutes I2S first. Keep R52 100 k as the at-reset pull-down (it is on the GPIO side; with R54 = 10 k the SDZ node still sits at 0 V when the GPIO is high-Z).

**DIGITAL-15 (lite-main, note): IR receiver supply.** R65 100R + C80 4.7 uF from +3V3: f_c = 1/(2π·100·4.7 u) = 339 Hz. At about 1.2 mA receiver current the drop is 0.12 V, giving 3.18 V. The IRM-3638T minimum supply is 2.7 V (if it matches the IRM-H638T's 2.7 to 5.5 V; the IRM-3638T number is unverified), so the margin is 0.48 V. The output is pulled to +3V3 by R63 10 k, so it stays in the 3.3 V domain. Pass.

---

## Block 5: core, ESP32 pin use and firmware support (DIGITAL-3, DIGITAL-16, DIGITAL-17)

The pin census is in Block 2b. Strap and boot behaviour per pin:

| GPIO | Load | At reset | Verdict / margin |
|---|---|---|---|
| 0 | ETH_CLK: Y2 via 33R, LAN8720A CLKIN, R31 10 k pull-up, Q5 | must read 1 | **Fails today (DIGITAL-1)**. With the fix, OE is held low, Y2's output is tri-stated, R31 pulls high, and the pin reads 1 |
| 2 | I2S_BCK: ES9018K2M DATA_CLK (tri-stated while RESETB is low; master mode off by default), PCM5102A BCK (input), ES7243E SCLK (slave by default: reg 0x00 MSC = 0, ES7243E ds rev 2.0 §7) | internal pull-down, 0 | Pass: nothing drives it at reset, so download mode (0/0) works |
| 5 | I2S_DOUT_MAIN → ES9018K2M DATA2 (input) | internal pull-up, 1 | Pass (only affects SDIO slave timing) |
| 12 (MTDI) | I2S_DOUT_SUB → PCM5102A DIN (input, no pull) | internal pull-down, 0 → VDD_SDIO 3.3 V | Pass. **Never add a pull-up here.** A 1 would select 1.8 V for the 3.3 V flash and the module would not boot |
| 15 (MTDO) | LCD_DC → FFC J3 pin 9 | internal pull-up | Pass. A panel pulling it low only silences the ROM log (DIGITAL-9) |
| 4 | I2S_LRCK | n/a | Pass |
| 13/14 | I2C SDA/SCL (2.2 k to +3V3) | n/a | Pass (Block 7) |
| 18/23 | MDIO (1.5 k pull-up) / MDC | n/a | Pass. squeezelite `eth_config=model=lan8720,mdc=23,mdio=18` |
| 19/21/22/25/26/27 | RMII TXD0/TX_EN/TXD1/RXD0/RXD1/CRS_DV | n/a | **Pass: matches the fixed ESP32 EMAC map** (squeezelite README l.506 to 518, WROVER-E pin table) |
| 32/33 | LCD SCLK/MOSI via 33R (R402/R403 marked DNP in parts.json, so FFC_SCK/FFC_MOSI are open on the Plus build) | n/a | Pass |
| 34 | ISENSE (INA186 via 1 k/10 nF), ADC1_CH6 | input-only | Pass |
| 35 | IR_IN (U18 + FFC pin 15, R58 10 k) | input-only | Pass. If the front panel also fits an IR receiver, two outputs are wired together: keep only one (note) |
| 36 | I2S_DIN ← ES7243E SDOUT/AD2, R120 100 k to GND (sets AD2 = 0) | input-only | Pass. ES7243E address 0010 AD2 AD1 AD0 = **0x10** |
| 39 | EXP_INT (TCA open-drain, R36 10 k) | input-only | Pass. Errata GPIO-3.11 glitches cause only an extra expander read |
| 16/17, 6 to 11 | NC | PSRAM/flash | Pass |

LAN8720A straps (DS00002165B pin tables): RXER/PHYAD0 is open, internal pull-down, PHY address 0. LED1/REGOFF is open, pull-down, so the internal 1.2 V regulator is on (VDDCR has 1 uF + 100 nF, DIGITAL-8). LED2/nINTSEL is open, pull-up, which selects REF_CLK-in mode (Y2 drives XTAL1/CLKIN, XTAL2 open). **Correct.** RXD0/RXD1/CRS_DV (MODE0 to 2) are internally pulled up (about 50 uA, 66 k equivalent) to MODE = 111, all-capable auto-negotiation. The ESP32 GPIO25 to 27 have no pull at reset, and the straps latch when firmware releases nRST. So the firmware must configure the EMAC pins **without** pull-downs before setting PHY_RST, or a ≤ 45 k ESP32 pull-down would bring MODE to about 1.3 V (undefined). IDF's EMAC init does not enable pulls. CLKIN spec: ±50 ppm and 150 ps p-p jitter, against YSO110TR ±10 ppm + ±20 ppm over temperature (30 ppm, 40 % margin) and 0.7 ps phase jitter (12 k to 20 MHz). Pass. Magjack HR911105A: pins 4/5 (centre taps) on +3V3E with 10 uF + 3 × 100 nF, 49.9 Ω terminations to +3V3E: that is the LAN8720A reference arrangement. Pins 9 to 12 (link/activity LEDs) are unconnected: there is no link LED (note).

**Firmware support at squeezelite-esp32 ee1720c (sources: `README.md` and `components/squeezelite/output_i2s.c`, `components/services/gpio_exp.c` in that commit):**

| Part / function | Upstream support | Verdict |
|---|---|---|
| ES8388 (Lite) | `model=ES8388` built-in sequence (README l.126 to 129, l.210) | Supported |
| PCM5102A | generic `model=I2S` (README l.143) | Supported, but only on the single I2S data line (see DIGITAL-3) |
| ES9018K2M | not a named model. `model=I2S` plus `dac_controlset` JSON register writes over I2C at `i2c=72` (0x48, decimal required, README l.208) | Supported generically. Its power-on defaults (32-bit I2S, auto input select) already play I2S without I2C |
| ES7243E line-in (ADC) | **none**: squeezelite is a player, and I2S RX capture or line-in routing is not implemented | Custom code needed (I2S0 RX shares BCK/WS with TX, so it is possible) |
| LAN8720 RMII | `eth_config=model=lan8720,mdc=,mdio=,rst=` (README l.504 to 526). README l.523: a reset pin is "recommended to avoid that GPIO0 (50 MHz input clock) locks the esp32 in download mode", which confirms DIGITAL-1's mechanism | Supported, but `rst=` must be a native GPIO (README l.341: expander GPIOs "cannot be used for … Reset"). The PHY_RST/OE line on the expander must therefore be driven by the custom hifi component before network init |
| TCA9555 / TCA9539 expander | `gpio_exp_config=model=pca9535,addr=…,intr=39,base=100` (README l.323 to 341; gpio_exp.c l.114 registers "pca9535" with low-level interrupt). The register map is identical | Supported for buttons, rotary and outputs |
| Rotary encoder on the expander | README l.341: "Buttons, rotary encoder, amplifier control and power are supported" on expanders | Supported. Timing: EC11 at 5 rev/s × 20 pulses × 4 edges = 400 edges/s, 2.5 ms per edge. One INT-triggered I2C read at 400 kHz is about 90 us plus task latency of about 1 ms: **2.5× margin**. The 10 nF + 10 k RC (τ 0.1 ms) is light next to the README's suggested about 470 nF. Acceptable because the expander does no debouncing and the firmware decodes quadrature |
| LCD on SPI with CS on the expander | display_config requires `cs=<gpio>` (README l.261), and README l.341 excludes expander GPIOs for "CS, D/C" | **DIGITAL-16** |
| CH224Q/CH224A over I2C | not in upstream. The hifi component (`hifi_pd.c`) already reads it on Lite | Custom (exists for Lite) |
| Second I2S data line (sub DAC) | `output_i2s.c` uses one `CONFIG_I2S_NUM` for both DAC and SPDIF (l.238, l.335 to 336, l.385 to 386) | **DIGITAL-3** (major) |
| MCLK with Ethernet | README l.526: GPIO0 is taken by REF_CLK, so the DAC's MCLK must come from elsewhere. Core gives the ES9018K2M its own 50 MHz (Y1) and needs no ESP32 MCLK. ES7243E takes MCLK from BCLK (reg 0x02 bit 7 = 1) and PCM5102A uses its PLL (SCK tied to GND) | Pass |

**DIGITAL-16 (core, minor, firmware):** LCD_CS sits on TCA P17 with R57 10 k pull-**up**, so the panel is deselected at reset. squeezelite's display driver needs a native CS GPIO, and none is free (Block 2b). It is the only device on that SPI bus, so: **change R57 to a 10 k pull-down to GND** (CS permanently asserted), firmware leaves P17 as an input, and the display is configured with `cs=-1`. ESP-IDF `spi_bus_add_device` accepts `spics_io_num = -1`. Whether squeezelite's `display_config` parser accepts `cs=-1` is unverified; otherwise it needs a one-line patch. ST7789/ILI9341 tolerate a permanently low CS (each command is framed by DC).

**DIGITAL-17 (core, note): the PCM5102A takes BCK at 64 fs from the ESP32** (32-bit slots). Its PLL needs BCK = 32, 48 or 64 fs (TI SLAS859 §8.3.x). ESP32 I2S at 16 or 32 bit gives 32 or 64 fs. Pass. ES9018K2M at 64 fs: DATA_CLK = 2 × i2s_length × FSR must match the register 1 setting (32-bit default). Firmware must use 32-bit slots or set reg 1 to 16-bit.

---

## Block 6: RF, PCB antenna placement (DIGITAL-18, DIGITAL-19)

Method: `pcbnew` (KiCad 10.0.6) on both boards. The antenna end of the module is taken as the module body beyond the pin-1/pin-38 row: about 7 mm of the 31.4 mm length, matching the WROVER-E outline (ds v2.4 §10.1 and Fig. 5 "Keepout Zone"). I sampled zone fills on every copper layer on a 0.5 mm grid and listed tracks and pads inside that rectangle. Script: `/tmp/rev/digital/rf.py`, `rf2.py`; the method is described here so it can be re-run.

Espressif guidance (ESP Hardware Design Guidelines, ESP32, release master, §1.4.8 and Fig. 23/25): put the antenna **outside** the base board, with the feed near the edge. If it cannot overhang, keep the antenna end ≤ 1 mm from the board edge and the module side ≤ 2 mm from the edge, and **cut the board away** beside and under the antenna (Fig. 25: a clearance ≥ 15 mm wide along the edge, 6.19 mm deep). Never place the antenna mid-board with a hollow on four sides. Keep ≥ 15 mm clearance to the housing in all directions. Put dense ground vias near (not under) the antenna.

**lite-main (U5, rot 90°, antenna toward the left board edge):**
- Board edge x = 22.815, module end x = 23.37: the antenna is **0.55 mm inside** the edge (guideline ≤ 1 mm). Pass, with 0.45 mm to spare.
- Rule area `ANT_KEEPOUT`, F.Cu + B.Cu (both layers of the 2-layer board), x 22.865 to 30.065, y 115.823 to 140.823, with no tracks, vias or fills. Measured result: **no copper of any kind inside the antenna rectangle on either layer.** Pass.
- Clearance beside the antenna along the edge: the keepout extends only **2.2 to 2.5 mm** past each side of the module (module y 118.3 to 138.3). GND pour on F.Cu and B.Cu starts right at the keepout edge, and the board continues 58 mm on one side and 12 mm (to the corner at y = 150.37) on the other. The guideline asks for ≥ 15 mm cleared along the edge or an overhang. **Shortfall: 15 - 2.5 = 12.5 mm.**
- Metal near the antenna: H4 (M3 plated hole, screw) is 8.2 mm from the antenna rectangle. The enclosure (PETG, 106 × 96 mm against a 100 × 90 mm board, `enclosure/out/lite/fit.md`) puts the wall about 3 mm from the antenna end, against a recommended 15 mm.
- Noise sources: J1 (USB-C) is 73 mm away, the TPS54302 buck U2/L5 55 to 59 mm, the SY8089 U3/L6 47 to 52 mm, the class-D U8 73 mm and its inductors 67 to 90 mm. All are far. Pass.
- **DIGITAL-18 (lite-main, minor):** expected impact is a few dB of detuning and range loss, not a failure. **Fix:** extend `ANT_KEEPOUT` (both layers) from y 140.823 down to the board corner at y 150.373, so the antenna sits in a cleared corner (11.8 mm beside the module on that side). Move H4 out of that strip, or use a nylon screw there. Better still, if the outline allows, cut a 7 × 32 mm notch (x 22.8 to 30.1, y 116 to 150.4) so the antenna is effectively overhanging. Contingency: no board change. Measure the RSSI/throughput in the enclosure at first bring-up, and if the board is short on range, the WROVER-IE variant (same footprint, u.FL) is the drop-in fallback.

**core (U7, rot -90°, antenna toward the right board edge):**
- Board edge x = 154.637. Module end (courtyard) x = 156.16, so the module **overhangs 1.5 mm**, but about 5.4 mm of the about 7 mm antenna end (x 149.2 to 154.6) still lies **over the board**.
- **No antenna keepout rule area exists on core.** Measured: zone fill **GND on F.Cu, In1.Cu and B.Cu, and +3V3 on In2.Cu covers x 149.2 to 153.7/154.2 at the antenna's centre line (y 200.7)**. That is copper under the antenna on **all four layers**. Tracks inside the antenna area: `/I2S_DIN` (F.Cu, x = 149.29, y 190.17 to 192.92) and `/RMII_TXD1` (F.Cu, 149.29, 208.6 to 210.84, continuing to 141.98, 218.15).
- Metal: H3 (M3 plated) is 6.2 mm from the antenna. No enclosure exists yet for core.
- Noise sources: LAN8720A, Y2, the bucks and the amps are all ≥ 55 mm away. Pass.
- **DIGITAL-19 (core, major):** a ground and power plane under a PCB inverted-F antenna shorts its radiating section to the plane: typically 10 to 20 dB of efficiency loss and a detuned resonance. Wi-Fi will associate on the bench and drop out at room distance. Ethernet is unaffected, but Wi-Fi is the product's main network. **Fix (pick one):**
  1. **Board notch (preferred, cheapest):** an Edge.Cuts cut-out x 148.9 to 154.64, y 190.0 to 211.4, so the whole antenna end overhangs into air. Reroute I2S_DIN and RMII_TXD1 west of x = 148.5. Pull the In1/In2/F/B pours back to the notch edge with a via fence at ≤ 3 mm pitch along it.
  2. Or a rule area on **all four layers** over x 148.9 to 154.64, y 188 to 213.5 (2.5 mm past each module side): no tracks, vias or fills, plus the same reroute. This is worse than (1) because the FR4 stays, but it is acceptable per Fig. 25 if the strip also extends ≥ 15 mm along the edge.
  
  Keep H3 or use a nylon screw. The enclosure, when designed, must keep 15 mm of plastic-free space around the antenna or use a window.

---

## Block 7: clocks, I2S/RMII signal integrity, I2C buses (DIGITAL-4, DIGITAL-5, DIGITAL-20)

Trace lengths come from `pcbnew` track sums.
- core: ETH_CLK 95.7 mm (2 vias) + ETH_OSC 19.0 mm; RMII TXD0 95.7, TXD1 109.1, TX_EN 126.0, RXD0 83.9, RXD1 84.8, CRS_DV 82.3 mm; MDIO 88.2, MDC 96.2; I2S BCK 104.8 (6 vias), LRCK 113.7, DOUT_MAIN 111.2, DOUT_SUB 58.5, DIN 52.7; DAC_MCLK 12.3 + 10.3; I2C SCL 216.8, SDA 194.8.
- lite: I2S 45.6 to 50.0, MCLK 54.5, I2C SCL 96.0, SDA 119.7 (+3.4 to CH224A).

**RMII clock on core (sim):** `sims/digital/eth_clk.cir` (+ `.bounds.json`, run with hwde `sim_run.py`). Y2 (Rout 15 Ω) feeds 19 mm, then R60 33 Ω, then a T: 30 mm to the LAN8720A CLKIN (4 pF) and 65 mm to ESP32 GPIO0 (5 pF), with Q5 (5 pF) and R31 on the way. Z0 = 50 Ω, 6.0 ps/mm.

| Case | CLKIN 0.99 to 2.31 V transit | GPIO0 0.825 to 2.475 V transit | Ringback min while high | Overshoot |
|---|---|---|---|---|
| tr = 1 ns (`eth_clk.cir`) | 1.33 ns | 0.67 ns | CLKIN 2.87 V (margin 0.56 V over VIH 2.31), GPIO0 3.00 V (margin 0.52 V over 2.475) | 3.49 V |
| tr = 0.4 ns (`eth_clk_fast.cir`) | 1.27 ns | 0.61 ns | 2.85 V / 2.82 V | 3.61 V at GPIO0 (10 mV over VDD + 0.3; absorbed by the clamp diodes, warning) |

The edges are monotonic, with no ringback into the threshold band, and duty cycle is unaffected. **Pass**, with 0.52 V of noise margin. The clock reaches the PHY about 0.4 ns before the ESP32 (65 vs 30 mm). RMII RX timing at the ESP32: the PHY's output delay plus about 0.5 ns of trace, sampled on a clock that arrives 0.6 ns late, adds 0.6 ns of setup margin and removes 0.6 ns of hold. With the LAN8720A's minimum clock-to-out (DS00002165B RMII timing, not re-extracted here) of about 2 ns, hold margin stays ≥ 1.4 ns. The ESP32 EMAC input hold time is not published (unknown). TX: the ESP32's clock-to-out plus 0.6 to 0.8 ns of trace against the 20 ns period. Pass.

**DIGITAL-20 (core, minor): 82 to 126 mm RMII data lines with no series termination.** ESP32 pads (about 25 to 40 Ω at drive strength 2) into about 50 Ω lines with 0.5 to 0.75 ns one-way delay mildly under-damp. By the same model as the clock sim, ringback stays above threshold, but it is a known source of CRC errors on long RMII runs. **Fix/contingency:** move U9/J2/Y2 next to U7, so RMII runs < 30 mm (the real fix, a layout move). Or, at minimum, add 0402 22 Ω series resistors (fit 0R if not needed) at the ESP32 end of TXD0/TXD1/TX_EN and at the PHY end of RXD0/RXD1/CRS_DV. Check Ethernet at 100 Mb/s with iperf for packet errors at bring-up.

**I2S fan-out (core):** BCK 104.8 mm with 6 vias to three loads (ES9018K2M DATA_CLK, PCM5102A BCK, ES7243E SCLK) at 3.072 MHz (64 fs at 48 kHz). The edge (about 2 ns) is longer than 2× the flight time (2 × 0.63 = 1.26 ns), so the line is electrically short to marginal. No double-clock risk is expected. Note: a 33 Ω series resistor at GPIO2 would be cheap insurance; the PCM5102A PLL and the ES9018K2M DPLL both lock to BCK.

**ES9018K2M MCLK (DIGITAL-4):** Y1 has 10.3 + 12.3 mm with R71 33R. The MCLK requirement for PCM normal mode is MCLK > 192 × FSR: 50 MHz/192 = **260.4 kHz**. Margins: 48 kHz (9.2 MHz needed) 5.4×; 96 kHz 2.7×; 192 kHz (36.9 MHz) 1.36×; 384 kHz fails. ESP32 squeezelite output is ≤ 96 kHz, so this passes. DVDD: internal DVDD is allowed up to MCLK = 50 MHz, and the part runs at exactly 50 MHz ±30 ppm (DIGITAL-4, minor).

**PCM5102A clocking:** SCK pin 12 = GND, so the internal PLL runs from BCK (3-wire mode). FMT = GND (I2S), FLT = GND (normal-latency filter), DEMP = GND. XSMT = DAC_EN (see DIGITAL-2 for its power-up state). Pass.

**ES7243E clocking:** MCLK pin 20 = GND. The firmware must set reg 0x02 bit 7 MCLK_SEL = 1 (MCLK from BCLK) and power the clocks on (reg 0x01). Its MCLK spec max is 49.2 MHz, so N/A. SCLK max is 26 MHz against 3.072 MHz. Pass. It is a slave (default MSC = 0).

**I2C buses.**

| Bus | Devices (7-bit address) | Pull-up | C estimate | Rise time 0.8473·R·C | Fast-mode limit 300 ns |
|---|---|---|---|---|---|
| lite | ESP32, ES8388 0x10, CH224A 0x22 or 0x23 | 2.2 k to +3V3 (R22/R23) | traces 120 mm × 0.1 pF/mm = 12 pF + 3 × about 6 pF = **30 pF** | 56 ns | 81 % margin. Max C at 400 kHz = 300 ns/(0.8473 × 2.2 k) = 161 pF |
| core | ESP32, ES7243E 0x10, TCA9555 0x20 (0x74 if TCA9539), CH224Q 0x22 or 0x23, ES9018K2M 0x48, + FFC to the panel | 2.2 k to +3V3 (R34/R35) | traces 217 mm × 0.12 pF/mm = 26 pF + 5 × about 6 pF = 30 pF + FFC about 10 pF + panel (unknown, allow 30 pF) = **96 pF** | 179 ns | 40 % margin (61 pF without the panel: 114 ns) |

- No address clashes on either bus. The ES7243E (0x10) is on core only, so it does not meet the ES8388.
- Sink current 3.3 V/2.2 k = 1.5 mA, against 3 mA for VOL ≤ 0.4 V (TCA9555 SDA spec, I2C standard). Pass, with a 2× margin.
- Voltage domains: all pull-ups go to +3V3. The CH224 CFG2/CFG3 have internal pull-ups and accept 3.3 or 5 V inputs (CH224 ds v2.1 §5.2.2). The rail they pull to internally is not stated. If it is above 3.3 V, the about 100 k internal pull-up against 2.2 k shifts the high level by at most 2.2/102 × 1.7 V = 37 mV (3.34 V < 3.6 V ESP32 maximum). Pass.
- ES9018K2M I2C runs 0 to 400 kHz with Cb ≤ 400 pF. ES7243E: 100/400 kHz.
- The panel on J3 must **not** add its own pull-ups below 4.7 k: the parallel combination would exceed 3 mA.

---

## Block 8: core user interface, IR, S/PDIF, USB (DIGITAL-21)

- **Buttons and encoder on the expander:** BTN_1/BTN_2 have R55/R56 10 k to +3V3, with the switches to GND. ENC_A/B/SW have R52 to R54 10 k plus C37 to C39 10 nF (τ = 0.1 ms) and EC11 common C/E/F/G to GND. Pull-ups present: pass (the TCA9539 swap removes the internal pull-ups, so these 10 k are then the only ones, which is fine). Encoder timing margin 2.5× (Block 5).
- **IR (U18 IRM-H638T/TR2, LCSC C91447: 2.7 to 5.5 V, 1.2 mA, 38 kHz):** R400 100R + C400 4.7 uF from +3V3 gives f_c = 339 Hz and supply 3.3 - 0.12 = 3.18 V against the 2.7 V minimum: 0.48 V margin. Output pulled to +3V3 by R58 10 k (3.3 V domain) into GPIO35 (input-only, external pull present). Pass.
- **TOSLINK (Ultra U19 PLR237/T8BK, LCSC C17223620):** OPT_VCC is +3V3 through FB3 + C126 100 nF. The output goes to ES9018K2M GPIO1 (SPDIF input 4, reg 8 gpio1_cfg = 4'd9 / input-select per reg 21) with R70 10 k to GND. ES9018K2M ds v3.7: S/PDIF needs MCLK > 386 × FSR, so FSR ≤ 50 MHz/386 = **129.5 kHz**. 44.1/48/88.2/96 kHz decode (margin 1.35× at 96 kHz); **176.4/192 kHz S/PDIF will not lock** (DIGITAL-21, note: few TOSLINK sources send 192 k, and many TOSLINK receivers are limited to 96 k anyway). Fix if wanted: none on this hardware. Document "optical up to 96 kHz". The PLR237's supply range (3.3 V operation) is **unverified**; the LCSC listing gave no range. If it is a 5 V-only part, run OPT_VCC from +5V **and** add a divider (e.g. 10 k/20 k) into GPIO1, because the ES9018K2M I/O is DVCC = 3.3 V.
- **USB (core):** J1 A6/B6 → D+, A7/B7 → D- (both orientations). U2 USBLC6-2SC6 on D+/D- with its VBUS pin on +5V (correct pinout: 1/6 = I/O1, 3/4 = I/O2, 2 = GND, 5 = VBUS). CH340C/B direct, with no series resistors. Pass. Lite has no USB ESD array on D+/D- (U6 is directly at the connector through 71 to 73 mm of trace). CH340 HBM ESD is not specified in the V3D datasheet. **Note:** a USBLC6-2SC6 (C2687116, already on the core BOM) at J1 on Lite would match core. Its absence is not a functional fault.
- **CH224Q PG (core)** is unconnected; the power path uses its own sensing. CH224A PG (Lite) goes to GPIO4 with R4 10 k. Pass.
