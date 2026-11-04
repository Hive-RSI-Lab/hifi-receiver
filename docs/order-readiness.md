# Order readiness: lite-main, core, ultra-front (2026-11-04)

This file records where each hi-fi board stands before the order, and what was
decided about every finding raised in the last round: the gate re-run, the sims,
three independent Opus reviews, the hand checks and the bring-up plans.

**Nothing has been built or run on hardware.** "Firmware builds" means
ESP-IDF builds that boot in QEMU. Every sim number is a model estimate.
Ordering spends money, so it is the person's call. See `boards/core/fab/ORDERING.md`
and each board README's order section.

## Gates, re-run from scratch

- On main 5b7f956, `python3 tools/hwcheck.py test --all` passed (rc 0) with the firmware build and QEMU boot turned on.
- Each board passed erc, drc_routed, verify and dfm.
- Each tier passed `fw check-all` (hifi-lite, hifi-plus, hifi-ultra) and the fw constraints check.
- All firmware host tests, the product tests and the tools tests passed.
- CI sets `HWCHECK_FW=skip`, so CI alone doesn't cover the firmware.
- Since 5b7f956 no schematic, pcb or gerber has changed. The changes are firmware, docs, bench, and the Lite CPL and distributor BOMs (PR 165).

## Fab pack

I regenerated each board's fab from main in a scratch tree and compared it with what's committed:

| Board | Gerbers | BOM | CPL | prebuy |
|---|---|---|---|---|
| core (Plus and Ultra) | same (drill comment lines differ only) | same | same | same |
| ultra-front | same | same | same | same |
| lite-main | same | same | J2/J3 Y moved 0.79 mm (PR 165) | same |

- **lite-main J2/J3:** the LCSC model has pad 3 0.8 mm off pad 1 in Y, while the real jack and our footprint have them level. So no single offset fits all three pads.
  - The CPL now places the jacks at the footprint origin.
  - They are through-hole, so their holes locate them. Check them in JLC's preview.
- **core bom_cpl** exits 1 only for the TPA3220 stock shortage, which is known: JLC has none, so it's pre-bought per ORDERING.md.

## Decisions

### Core C53 and C55
These were decided in `boards/core/README.md` (PR 160): no copper change.
- C53 is a fifth cap on a pin group that four others already cover.
- C55 is 13 % over the loop limit. If bring-up shows DAC noise from the digital supply, the next-spin fix is a 100 nF 0402 beside pin 18.

### Core Q1 soft start at 153 C (one stacked corner)
`docs/sims/core/pvdd_switch.py` puts Q1 at 153 C against its 150 C abs max, but only when three worst cases stack:
- C4 derated to 125 nF;
- VBUS at 29.4 V;
- Ta at 60 C.

Taking any one of them away puts it under the limit:
- Ta 50 C: 143 C.
- Typical C4: 142 C.
- Nominal C4: 123 C.

**Accepted for this order.** The soft start runs once per plug-in and lasts tens of ms. Plug-in happens at room temperature, not inside a hot enclosure.
- **Next spin:** C4 to 330 nF (or 2 x 220 nF), for about 30 % less peak power in Q1.
- **Bench:** see `docs/bringup/core.md`, PVDD ramp.


### SI findings (`docs/sims.md`, PR 164)
No copper change. Each is a model estimate with a bench gate in the bring-up plan (PR 167).
- **Lite U8 PVDD HF loop:** 38.5 V against 30 V abs max, but only for a 7.5 A (OCP) edge in 3 ns through one cap, i.e. an output short. At the rated 2.1 A it is about 26 V. **Accepted with a bench gate:** U8 PVDD peak ≤ 27 V at 20 V into 4 Ω near clip (lite-main.md 9.9). Fallback: a 15 V contract. Lite's voltage is set by the R1 strap, so that means an R1 change or a new CH224A request write in firmware (Lite doesn't request a voltage today). Next spin: 1 µF caps at the pins with GND straight to the EP.
- **Core U15/U16 PVDD HF loop:** 37.5 V against 37 V at OCP-level current; about 3.6 V of spike at the governed peak. Existing R5-PA-3. Gate ≤ 35 V (core.md 6.8); fallback a 20 V contract.
- **Core ETH_CLK at U9 CLKIN:** ring-back fails only for edges under 1.45 ns; Y2 gives "4 ns max". Measure at bring-up (core.md 6.9).
- **I2S BCK overshoot:** core 0.006 V under the warning line; review-5's 33 R never made it onto the board. Firmware sets core's BCK/LRCK/DOUT drive to CAP_1 (PR 168; Lite already uses CAP_0). Ultra-front SCK: 0.13 V margin, pass.
- **Lite has no Zobel:** +38.6 dB open-load peak. The risk accepted in core-zobel.md; reported, not passed.
- **R3-POWER-6 hot plug:** Lite VBUS 31.6-32.2 V (core 44 V) into a non-compliant source. Unchanged, known.

## Independent reviews (three Opus agents, repo only, on 5b7f956)
None found a reason a board would be dead.

### lite-main (a3f33)
| # | Finding | Disposition |
|---|---|---|
| 1 | No hardware power limit (PLIMIT tied to GVDD); clipping draws 80-88 W against a 60 W contract | **Explained.** By design: the firmware governor plus the volume cap hold it. Bench: the 9.2 power sweep. A PLIMIT divider is a next-spin option. |
| 2 | D1 SMBJ22A clamps above 30 V parts | **Known**, POWER-9 / R3-POWER-6. Compliant sources never reach it. |
| 3 | Q1 turns off slowly (τ 0.3-0.47 s) | **Explained, no change.** `board_lite.c` drives SDZ low before PVDD_EN, so the amp is in shutdown while Q1 turns off. |
| 4 | SDZ comment says 100k, R54 is 10k | **To fix** (comment only), in the docs PR from thread hifi-docs. Not landed yet. |

### core (a0cd7)
| # | Finding | Disposition |
|---|---|---|
| 1 | FFC must be type B | **Covered.** ultra-front README and enclosure README spell out type B and how to check the listing. |
| 2 | D1 SMBJ30A clamps above protected parts | **Known**, POWER-9 (justified). |
| 3 | OV cut at 31-35 V | **Known**, POWER-8 (justified). Slow cut-off for gross faults. |
| 4 | Q1 partly on if PVDD_EN rises on a 5 V contract | **Explained.** Q2 gates PVDD_EN on PG (POWER-6), and firmware raises it only after the contract. Bench: core.md 3.5 and 5.1. |
| 5 | ES7243E MCLK tied to GND | **Handled** by firmware (reg 0x02 = 0x80, 64 fs). |
| 6 | `architecture/blocks.md` stale (TCA9555 at 0x20); the board has a TCA9539 at 0x74-0x77 | **To fix** in the docs PR from thread hifi-docs (not landed yet). `docs/bringup/core.md` already flags it. |
| 7 | Bottom-side CPL rotations | **Covered.** cpl_verify passes; ORDERING.md tells the assembler to check the bottom preview. |
| 8 | RBIAS 12k not 12.1k | **Accepted**, cosmetic (review-3 pass). |

### ultra-front (acaae)
| # | Finding | Disposition |
|---|---|---|
| 1 | FFC must be type B | **Covered**, as core 1. |
| 2 | LCD SPI at 16 MHz, over the ST7789's 15.15 MHz | **Fixed** in firmware: 13.33 MHz (80/6). |
| 3 | LCD_DC on the GPIO15 strap | **Known**, DIGITAL-9. At worst the ROM log is silent. |
| 4 | BL net named PWM but on/off | **Known**, R5-UF-4. Cosmetic. |
| 5 | LCD VCI 3.33-3.40 V against 3.3 V op max | **Known**, R5-UF-N2. Abs max 4.6 V. |
| 6 | Unused TCA9535 pins share one R19 | **Explained.** Firmware keeps them inputs; even if one were an output, the fight is a few mA through R19. |

## Hand checks (not in CI), on 5b7f956
`check_esd`, `check_body_clash`, the 3D clash, and `check_model_paths`/`check_model_fit` on each board, plus the core + ultra-front stack fit (`enclosure/ultra.py`) on the current core pcb.
- **0 errors on every board.** ultra-front is fully clean, and the stack fit passes.
- Every warning is accepted with a reason in PR 170: the ESD docs and the board READMEs' "Hand checks, 2026-11-04" notes.
  - **ESD corridor warnings** (a newer hwde check): core ISNS_P, and 8 nets on Lite. All are open-circuit, sub-nH coupling estimates. CC1/CC2 fall under Lite's CC exemption.
  - **Banana pairs 0.20 mm apart:** the enclosure holes set their axes. Check for lean at assembly.
  - **IR receivers and the core WROVER antenna** stand past the edge by design.
  - **Core U3 3D model path is broken.** 3D only, no fab impact. Fix it at the next board edit.
  - **Lite L1-L4 have no 3D model.** heights.json carries 5.2 mm.

## Firmware (QEMU and host tests only)
- All three tiers build. hifi-lite, hifi-plus and hifi-ultra boot in QEMU, and the host tests pass. **None of it has run on a real ESP32 on these boards.**
- Bring-up additions, from the gap lists in the plans:
  - PR 168: Ultra LCD SPI at 13.33 MHz (under the ST7789's 15.15 MHz); the backlight's first minute by the clock; core I2S BCK/LRCK and sub DOUT at `GPIO_DRIVE_CAP_1` (R5-DAL-N1; the sims show CAP_0 is too slow).
  - PR 169: `hifi i2c <addr> <reg> [val]` (writes refused while audio plays or the amps are on); `hifi led test`; Ultra `hifi backlight on|off|auto`; `controls` in `hifi state` (expander bits, last button/knob/IR and its age, `ir.last_code`).
  - PR 166: `bench.py bringup` stage 1 passes on a correct core at 5 V (G1).
- **Gaps closed:** core G1, G4, G7; ultra-front 1-4 and 6; lite-main 2-3.
- **Gaps still open**, each with a hand workaround in its plan. None blocks the order.
  - **core:** no PVDD/VBUS sense and no amp temperature (G2), so use a meter and a thermocouple. ISENSE gain is compile-time (G3). `fs_vpk` is unset (G5). No zone B taps (G6).
  - **lite-main:** calibration is compile-time (1); no LED test, since squeezelite owns GPIO22 (4, by design); no raw GPIO readout (5); no input current (6, a hardware limit).
  - **ultra-front:** no LCD test pattern (5); `bench.py` has no front-board stage (7).

## Bring-up plans
`docs/bringup/lite-main.md`, `core.md` and `ultra-front.md` (PRs 161-163). They cover:
- a power-off check;
- current-limited first power;
- the rails in order;
- firmware flash;
- each block;
- the SI bench gates (PR 167): core 6.8 PVDD ≤ 35 V and 6.9 ETH_CLK edge; lite 9.9 PVDD ≤ 27 V;
- the thermal and power sweeps, which set the governor.

`docs/bench.md` is the scripted side (`bench.py bringup`).

## Readiness, per board

### lite-main
| Item | State |
|---|---|
| Gates (erc, drc, verify, dfm) | pass on 5b7f956; CI green |
| Hand checks | 0 errors; warnings accepted (PR 170) |
| Fab pack | current; CPL J2/J3 fixed (PR 165) |
| Sims | done (PR 164); U8 PVDD spike has a bench gate |
| Firmware | builds, boots in QEMU; `hifi i2c` and `controls` in state (PR 169) |
| Bring-up plan | on main |
| Review | 4 findings: 3 explained or known; 1 comment fix pending (docs PR) |
| Not proven until the bench | U8 PVDD peak (9.9), amp thermal (waiver), power draw vs governor, no Zobel |
| Needs the person | the order (money) |

### core (Plus and Ultra)
| Item | State |
|---|---|
| Gates | pass on 5b7f956; CI green |
| Hand checks | 0 errors; warnings accepted (PR 170) |
| Fab pack | current; 4-layer, 1 oz inner, epoxy-filled vias (ORDERING.md) |
| Sims | done; Q1 153 C stacked-corner accepted; PVDD and ETH_CLK bench gates |
| Firmware | builds, boots in QEMU; I2S drive CAP_1 (PR 168); `hifi i2c` (PR 169) |
| Bring-up plan | on main, plus bench stage 1 (PR 166) |
| Review | 8 findings: 7 covered, known or explained; 1 doc fix pending (blocks.md) |
| Decisions | C53/C55: no change (PR 160) |
| Not proven until the bench | PVDD spike (6.8), ETH_CLK (6.9), amp cooling and governor, Q1 soft start |
| Needs the person | the order (money); **the TPA3220 pre-buy** (JLC has none; `fab/prebuy.csv`, ORDERING.md) |

### ultra-front
| Item | State |
|---|---|
| Gates | pass on 5b7f956; CI green |
| Hand checks | fully clean; stack fit with core passes |
| Fab pack | current |
| Sims | done; SCK 0.13 V margin, pass |
| Firmware | builds, boots in QEMU; LCD SPI 13.33 MHz (PR 168); backlight, LED and controls (PR 169) |
| Bring-up plan | on main |
| Review | 6 findings, all fixed or known |
| Not proven until the bench | LCD image, FFC type B fit |
| Needs the person | the order (money), together with core (Ultra) |

**"100 % sure to work" can't be promised before a board is built.** What can be said: every automated gate and hand check passes, three independent reviews found no fatal fault, and every residual risk has a bench gate with a written fallback.
