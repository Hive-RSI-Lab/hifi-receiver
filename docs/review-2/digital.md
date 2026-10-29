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
| DIGITAL-1 | core | **blocker** | GPIO0 (strap) gets the 50 MHz RMII clock at reset. Y2's OE comes from TCA9555 P05, which powers up at about 1.65 V (100 k internal pull-up against R45's 100 k pull-down), and it stays high across every ESP32 reset because the TCA9555 has no reset pin |
| DIGITAL-2 | core | **major** | The same TCA9555 pull-up against the 100 k pull-downs leaves PVDD_EN (1.10 V on the 2N7002 gate, Vth min 1.0 V), AMP_A/B_RST, DAC_EN (1.65 V) and DJ_DIRECT undefined at power-up and during any ESP32 reset |
| DIGITAL-3 | core | **major** (firmware) | Main DAC (GPIO5) and sub DAC (GPIO12) are on two separate I2S data lines that share BCK/LRCK. squeezelite-esp32 drives only one I2S data output, so this needs custom I2S1-slave code with a sample-aligned start |
| DIGITAL-4 | core | minor | ES9018K2M MCLK = 50 MHz sits exactly on the internal-DVDD limit (ESS: an external DVDD is needed when MCLK > 50 MHz). PCM tops out at 192 kHz (MCLK/192 = 260 kHz) |
| DIGITAL-5 | core | minor | LAN8720A nRST (10 k / 100 nF, tau 1 ms) is released about 0.9 ms after OE, but the oscillator may take up to 3 ms to start. Firmware must issue an MDIO soft reset, or C47 goes to 1 uF |
| DIGITAL-6 | lite-main | minor | Vol-down button on GPIO36 (SENSOR_VP): errata GPIO-3.11 pulls GPIO36/39 low for about 80 ns whenever SAR ADC1 powers up, and firmware reads ADC1 (GPIO34/39) |
| DIGITAL-7 | core | note | The auto-program transistor Q5's collector sits on the 50 MHz ETH_CLK net (adds C_ob of about 5 pF) |
| DIGITAL-8 | core | note | LAN8720A VDDCR has 1 uF + 100 nF (datasheet: 1 uF + 470 pF); RBIAS is 12 k (datasheet: 12.1 k 1 %, 0.8 % low) |
| DIGITAL-9 | core | note | GPIO15 (MTDO strap) goes to the FFC as LCD_DC. If a panel pulls it low, the ROM boot log is silenced (harmless) |
| DIGITAL-10 | core | note | With the TCA9555 internal pull-ups, LED_STATUS glows faintly (about 15 uA) and LCD_BL defaults on until firmware runs |

(More findings are added below as each block is finished: antenna/RF, I2C capacitance, flash
budget, the spare control line, and firmware support.)

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
