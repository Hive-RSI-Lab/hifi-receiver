# Review round 2 (review-3): AUDIO, small-signal audio chain, both boards

Reviewer: AUDIO, 2026-10-30. Board files on `main` at ac484b1, read-only. Sources: ESS ES9018K2M datasheet v3.7
(2021-04-22), Everest ES8388 rev 5.0 (2018-07), Everest ES7243E rev 5.0 (2020-05), TI PCM510xA SLAS859C,
TI OPA1678 (2022), TI TPA3220 SLASEN3B, TI LP5907, TI TS5A23157, and the Espressif ESP32-LyraT v4 schematic.
All were fetched to `/tmp/ds3/audio/`.

Tests (re-run from the repo root):
- `python3 -I docs/review-3/sims/audio/check_audio_nets.py` reads both `.kicad_pcb` files, checks values and
  connections and computes the level budget. It exits 1 and currently FAILs on R3-AUDIO-1 and R3-AUDIO-2.
- `cd docs/review-3/sims/audio && python3 gen_vref.py && $HWDE_PY $HWDE_SKILL/scripts/sim_run.py --dir .`
  runs 13 ngspice benches with bounds sidecars: VREF rail-dip, crosstalk, DJ-direct and Lite sub-thump. The
  as-built benches fail by design. The fix benches pass, except `fix_rc` and DJ `fix_inm_vref_c100u`, which are
  the rejected options.

## Findings

Counts: 0 blocker, 3 major (all new), 6 minor (5 new, 1 repeat), 3 notes.

### R3-AUDIO-1 (major, core, new): rail noise reaches the speakers through VREF
- **Where.** VREF = +5VA x R91/(R90+R91) with 470R/470R and C64 10 uF. The corner is 1/(2*pi*235R*10uF) =
  **68 Hz**. In the U11 difference stage VREF goes to MAIN_L/R at **gain 1** (MAIN = VREF + 0.463 (DACLB - DACL)).
  MAIN then reaches the TPA3220 at 24 dB (x15.85), and IN1M/IN2M (C219/C221/C317/C319) are AC-grounded to GND.
- **Effect.** Any +5V disturbance below about 100 Hz goes to the speaker at about 0.5 x 15.85 = 8x. +5V also feeds
  the ESP32's 3.3 V buck, the TPA3220 VDD/GVDD and the LP5907. Wi-Fi RX/TX bursts (beacon every 102.4 ms, packet
  bursts while streaming) are the classic "ESP32 ticking" field issue (esp32.com t=9613, squeezelite-esp32 #152).
- **Sim.** `tb_vref_dip_asbuilt`: a 10 mV, 1 ms dip on +5V gives **27 mVpp at the speaker**. The TPA3220's own
  floor is 72-100 uV A-weighted (SLASEN3B 6.x). At 88 dB/W/m and 6 ohm, 13.5 mVpk is about 43 dB SPL peak clicks
  at 1 m. The result scales linearly: 2.7 mVpp at the speaker per mV of dip.
- **Crosstalk.** The same VREF is shared by both channels through R73/R81, giving **47.6 dB at 1 kHz and 29.7 dB
  at 100 Hz** (`tb_vref_xtalk_asbuilt`, measured at the speaker).
- **Reference.** TI's single-supply op-amp guidance (and every single-supply K2M I/V board) decouples the
  mid-rail reference well below the audio band, or buffers it. Here the reference carries both the rail and the
  signal currents.
- **Fix.**
  - **C64 10 uF -> 100 uF 6.3 V X5R 1206 (LCSC C15008, JLC basic; verify stock).** In sim the tick drops to 3.5
    mVpp (8x better; about 6 mVpp with 60 uF after DC-bias derating), separation goes to 67.6 dB at 1 kHz and
    47.6 dB at 100 Hz, and DJ-direct ticks fall from 0.59 to 0.09 mVpp.
  - Optional on top: return the four amp IN_M caps (C219, C221, C317, C319) to VREF instead of GND. In the DAC
    path VREF is then common-mode at the amp (0 mVpp in sim; limited in practice by the amp's CMRR and the 10%
    cap match), but DJ-direct rises to 3.4 mVpp. Do it only with the 100 uF.
  - A 47R/220 uF pre-filter (`fix_rc`) is worse (4.6 mVpp) and was rejected.
- **Bring-up.** Measure +5V ripple and dips with Wi-Fi streaming, and the speaker idle noise.

### R3-AUDIO-2 (major, lite-main, new): sub-out thump at every power loss
- **Where.** ES8388 LOUT2/ROUT2 (pins 15/14) sit at VMID = 1.65 V DC, and C47/C48 10 uF carry them to SUB_L/R
  (R36/R38 100R, R37/R39 47k bleeders). There is no mute.
- **Effect.** When the USB-C is pulled or the charger fails, +3V3A collapses in about 1-2 ms (codec ~20 mA on
  ~6 uF). LOUT2 follows it to 0 and the -1.65 V step passes the coupling cap (tau 10 uF x (47k || 10k) = 82 ms)
  straight into the subwoofer.
- **Sim.** `tb_lite_sub_thump_asbuilt`: **-1.61 V at a 10k sub input**. That is above the 1.41 Vpk of a 0 dBFS sub
  signal, so it is a full-scale thump from a sub on its own mains. Firmware cannot help: the rails are going.
- **Contrast.** Core's sub uses the PCM5102A, which is ground-centred with PowerSense/XSMT muting, so it doesn't
  have this.
- **Reference.** LyraT feeds LOUT2/ROUT2 to an on-board PA that is powered down with the codec. It does not drive
  an external, independently powered amplifier through a DC-blocking cap. A line output to a mains-powered
  amplifier needs a mute.
- **Fix: a power-fail mute shunt on SUB_L/SUB_R (after R36/R38).**
  - Two MMBT3904 (C20526, basic) in inverted mode: collector to GND, emitter to SUB_x, base through 10k from MUTE.
  - MUTE comes from an MMBT3906 (LCSC C2910, verify) collector. Its emitter is on a reservoir of 47 uF fed from +5V
    through a 1N4148W (C81598, verify), with 10k base to +5V and 100k collector to GND.
  - +5V falling below about 4.0 V (reservoir 4.5 V minus Vbe) turns the mute on. That is well before the LP5907
    loses regulation at about 3.35 V.
  - Sim `tb_lite_sub_thump_fix_mute` (5 ohm shunt on 1 ms before the collapse): **35 mV**.
  - The same shunt can be pulled on by a GPIO through a diode for a clean power-up. Contingency: fit the parts with
    0R/DNP options.

### R3-AUDIO-3 (major, core, new; firmware duty): DJ-direct on core leaves the subs silent, and capture can't clock
- **Where.** `board_core.c` `hifi_bsp_audio()` feeds the PCM5102A the sum of squeezelite's mains only. In DJ-direct
  (U14 to LSW) squeezelite plays nothing, so the sub outputs carry nothing.
- **Intended design.** The ADC (U13 ES7243E, which hears the RCA through U20 by default) was to feed the subs.
  Firmware requirement C5 even inverts it in DJ mode (round-1 AUDIO-8). But `HIFI_HAS_CAPTURE 0`: the ES7243E is
  never configured.
- **Clocking.** The I2S runs 16-bit, so BCLK = 32 fs = 1.536 MHz (`dac_init`). The ES7243E takes MCLK from BCLK
  (U13 MCLK pin grounded), and its supported clocks start at 64 fs (ES7243E ds sec. 3). So its init must set
  REG02 MCLK_SEL = 1 (BCLK) and REG04 MULT_PRE = 3 (x8) to get 256 fs. The PCM5102A is fine at 32 fs (SLAS859C
  table 11).
- **Effect.** With the FLX4 on a core board in DJ-direct, the person's subwoofers are silent.
- **Fix (firmware).** Implement the ES7243E init above and run I2S0 RX (the patch already adds RX when `di=36`).
  In DJ-direct, route ADC to the sub path (inverted per C5). Alternatively, run 32-bit slots (BCLK 64 fs) and
  re-set the ES9018K2M word length. No board change is needed.

### R3-AUDIO-4 (minor, lite-main, new): Lite DJ-direct can't get loud
- **Cause.** The round-1 fixes stack: AUDIO-7's 15k/10k divider, AUDIO-6's LI2LOVOL -3 dB cap and the LOUT1VOL
  -4.5 dB cap. They now apply together to the bypass.
- **Numbers.** FLX4 2.1 Vrms gives 0.646 Vrms at LIN1 (with the 20k Rin, ES8388 ds 8.3). After the caps the amp
  sees 0.273 Vrms, so **5.4 Vrms at the speaker vs 13.4 Vrms clip (19 Vpk at 20 V / 6 ohm): 7.8 dB short**. That is
  about 5 W where the streaming path gives 11.9 Vrms.
- **Fix (firmware, hifi_board_lite.h / codec_apply).** In DJ-direct only, set LI2LOVOL to 0 dB (bits 5:3 = 010,
  ES8388 reg 39) and the LOUT1 cap to -1.5 dB (code 29). That gives 0.646 x 0.841 x 20 = 10.9 Vrms, 1.8 dB under
  clip at 2.1 Vrms. Keep the -4.5 dB cap for the DAC path.

### R3-AUDIO-5 (minor, lite-main, repeat_of AUDIO-13 / AUDIO-7): stale firmware scale constants
- `hifi_board_lite.h` still has `LINE_DIVIDER 2.0`. The loaded ratio is now 2.1/0.646 = 3.25.
- `TAP_DIVIDER 11.0` should be 16.0 (loaded 0.0625, requirement L6).
- `HIFI_HAS_TAPS 0`, though the board now has the taps (R66-R69, C81-C84).
- `ADC_FS_PEAK_V 1.27` (0.9 Vrms) should be 1.41 Vpk: the datasheet FS is AVDD/3.3 = 1.0 Vrms.
- **Fix.** Firmware: update the constants and default `HIFI_HAS_TAPS 1`. Otherwise the captures and the
  self-measurement read 3-4 dB wrong.

### R3-AUDIO-6 (minor, core, new): no RF shunt at the core RCA line-in
- **Where.** LIN_L/LIN_R go from J5 straight into R100/R110 4.7k. There is no C to GND. Lite has C42/C45 100 pF
  there.
- **Effect.** The RCA cable is an antenna about 0.5 m from a +20 dBm ESP32. Rectification in U20/U14 and the
  ES7243E/TPA3220 input ESD structures is the standard source of the 217 Hz-style TDMA buzz.
- **Fix.** Add 100 pF C0G 0402 at LIN_L/LIN_R to GND next to J5 (LCSC C1546, basic). An optional 1 nF at
  LDIV_x gives 2.35k x 1 nF = 68 kHz.

### R3-AUDIO-7 (minor, both, new): ground loop with an earthed laptop and an earthed sub
- **Where.** The RCA sleeves are hard on board GND (Lite J2/J3, core J4/J5).
- **Effect.** With an earthed laptop charger (FLX4 on USB) and an earthed subwoofer, our board closes an
  earth loop. For 10 mV of earth difference, about 1 ohm of loop gives I = 10 mA and 1 mV across a 0.1 ohm
  FLX4 cable shield: -66 dB re 2 Vrms. In DJ-direct that is 1 mV x 0.37 x 15.85 = 6 mV of 50 Hz-plus-harmonics
  at the speaker (core), 39 dB SPL at 88 dB/W.
- **Likelihood.** Class-II chargers and subs (the common case) give µV only.
- **Field.** The FLX4's "hum when the laptop charges" (Pioneer DJ community, FS-ELEC note) is inside the FLX4/laptop
  and no receiver input can fix it. A USB isolator or battery does.
- **Fix (contingency, core).** Make the ES7243E pseudo-differential: return C102/C112 (AINxN) to a sleeve-sense net
  at J5 pin 2 through a 4.7k/4.7k mirror, and add a 0603 lift footprint (fitted 0R, option 10R) from J5 sleeve to
  GND. Document "use a USB isolator" for the FLX4. Lite: no codec-side fix (the ES8388 differential mode is mono);
  document only.

### R3-AUDIO-8 (minor, core, new): PCM5102A supply decoupling below TI's typical application
- **Where.** C68/C69/C70 are 1 uF each on CPVDD/AVDD/DVDD. TI SLAS859C fig. (10.1.1) uses 10 uF + 0.1 uF per supply.
- **Effect.** CPVDD is the charge-pump supply, and it shares +3V3A with the ES9018K2M AVCC (whose output scales with
  AVCC).
- **Constraint.** The LP5907 allows <= 10 uF in total; core +3V3A has 6.3 uF now.
- **Fix.** Add 100 nF 0402 beside each of C68/C69. Optionally feed CPVDD from +3V3 (digital) through a 0603 ferrite
  with 10 uF, which TI allows (CPVDD is a separate pin), keeping the charge pump off the DAC reference.

### R3-AUDIO-9 (minor, core, new): sub-DAC XSMT follows the 3.3 V rail, not a pre-regulated one
- **Where.** U12 XSMT = DAC_EN, a TCA9539 output with a 100k pull-down.
- **Effect.** On power loss it falls with +3V3, so mute starts only when the rails are already going. TI SLAS859C
  11.2.2 / fig. 38 recommends that XSMT watch the pre-regulator rail. Ground-centred output keeps this to "some
  pop", not a thump.
- **Fix (optional).** A 100k/47k divider from +5V to XSMT, OR-ed through a BAT54 with DAC_EN, or accept.

### R3-AUDIO-10 (note, core): ESP32 panic/WDT reboot does not reset the TCA9539
- RESET is tied to ESP_EN, so a software reset keeps DAC_EN high.
- The boot write (DAC_EN = 1, AMP_x_RST = 0 in one port-0 write) then puts the amps into their 20 ms ramp-down,
  which is benign.
- An EN reset drives K2M RESETB low, and the K2M outputs are "driven to ground" (v3.7 pin table). The 1.65 V
  common-mode step x about 1.3 % CM gain (1 % resistors) is about 21 mV at MAIN, so about 0.3 V at the speaker
  while the amps ramp down.
- Firmware: before any intentional DAC_EN low, first set soft_start (reg 14) to 0 and put the amps in reset.

### R3-AUDIO-11 (note, core, repeat_of AUDIO-13): tap ratio
- The loaded A_SPK_x+ to ADC ratio from the board values is 0.0265 (150k/10k, 50k bias || 8k ES7243E). Firmware
  requirement C6 says 0.0276 (4 % off).
- Calibrate once at bring-up (already in C6).

### R3-AUDIO-12 (note, both): headroom passes, with margins
- **Core DAC path.** 0 dBFS gives 1.00 Vrms MAIN, then 15.8 Vrms (22.4 Vpk) vs ~26.5 Vpk clip at 28 V / 4 ohm:
  1.4 dB. OPA1678 output 1.03..3.86 V vs 0.8..4.3 V (0.23/0.44 V). + input peak 2.68 V vs 3.10 V (0.42 V).
- **Core FLX4.** 2.1 Vrms gives ADC 0.789 Vrms (-2.1 dBFS) and DJ-direct 11.6 Vrms at the speaker (no clip).
- **Lite.** FLX4 2.1 Vrms is -3.8 dBFS at LIN1. The DAC path is 11.9 Vrms vs 13.4 Vrms clip (1 dB).
- **Sub outs.** Lite: 1 Vrms max into 47k || sub. Core: 2.1 Vrms ground-centred through 470R/1nF (corner 339 kHz,
  TI uses 2.2 nF: fine for a sub).

## Blocks passed, against references

| Block | Reference | Ours | Margin |
|---|---|---|---|
| ES9018K2M pinout (U10) | ES9018K2M v3.7 pin table | all 28 pins + EP match. ADDR = GND gives 0x48, XO open, GPIO1 10k down, RESETB = DAC_EN | exact |
| K2M DVDD | >= 2.2 uF (min 1 uF at 1.2 V) | C56 4.7 uF 0603 | about 2x after bias |
| K2M MCLK | <= 50 MHz with internal DVDD (note 2, p.31) | 49.152 MHz (Y1), 1024 fs at 48 k | 1.7 % |
| K2M power-up | RESETB >= 1 ms after rails | DAC_EN from TCA9539 with 100k pull-down, firmware 20 ms | ok |
| OPA1678 single 5.1 V | VS >= 4.5 V; VCM (V-)+0.5..(V+)-2; VO 0.8 V from rails | see R3-AUDIO-12 | 0.6 V supply, 0.42 V CM, 0.23 V out |
| PCM5102A strapping | SLAS859C 10.1.1: FLT, DEMP, FMT, SCK low (3-wire PLL); CAPP-CAPM 2.2 uF; VNEG 2.2 uF; 470R + 2.2 nF | same; LDOO 2.2 uF; 1 nF filter; XSMT = DAC_EN | ok (decoupling: R3-AUDIO-8) |
| PCM5102A BCK 32 fs | table 11: 1.536 MHz at 48 k allowed | 16-bit, 32 fs | ok |
| ES7243E | FS VDDA/3.3 Vrms, Rin 8k, REFP/REFQ caps, MCLK_SEL from BCLK | 1 uF REFP/REFQ, 4.7 uF AC coupling (4 Hz), single-ended into P with N AC-grounded | ok (clock config: R3-AUDIO-3) |
| TS5A23157 U14/U20 | 0..V+ signal; VIH 0.7 V+ | biased 2.45 V / 1.65 V, S8050 inverters to 5 V logic | about 1.3 V each side |
| ES8388 pinout (U7) | ES8388 rev 5.0 pin table | 28 pins + EP match; CE = GND gives 0x10; HPVDD/AVDD on +3V3A, DVDD/PVDD on +3V3 | exact |
| ES8388 decoupling | LyraT v4: 10 uF + 0.1 uF on VMID/ADCVREF/VREF | 10 uF + 100 nF each | same |
| LP5907 output cap | <= 10 uF total | Lite 5.9 uF, core 6.3 uF | ok |
| Lite line-in RF | | C42/C45 100 pF at the RCA | ok |

## Round-1 fixes checked

- **AUDIO-1:** holds. R73/R75/R81/R83 = 1.3k on the PCB; margins as R3-AUDIO-12.
- **AUDIO-2/3:** hold. U20 biased nodes, Q7 inverter, R133/R134/R135 on the PCB.
- **AUDIO-4:** holds. R54 10k, C26 47 nF.
- **AUDIO-5:** holds. C101/C102/C111/C112 4.7 uF.
- **AUDIO-7:** holds on the board. The firmware scale is stale (R3-AUDIO-5).
- **AUDIO-10:** holds. Y1 49.152 MHz.
- **AUDIO-6:** holds in firmware (-4.5 dB, -3 dB caps), but it now over-limits DJ-direct (R3-AUDIO-4).
- **AUDIO-13 / L6 / L7:** not done in firmware (R3-AUDIO-5).
- **AUDIO-8/C5:** moot until capture exists (R3-AUDIO-3).

## Field issues searched

| Issue | Source | Our board |
|---|---|---|
| ESP32 Wi-Fi current-spike noise / ticking in audio | esp32.com viewtopic t=9613; squeezelite-esp32 #152 | core **has it** through VREF (R3-AUDIO-1). Lite: immune by design (codec VMID internal, analog from LP5907, PSRR > 60 dB) |
| FLX4 / DJ controller hum with laptop charging | community.pioneerdj.com post 22978284154649; fs-elec.com | source-side, n/a. Earth-loop case: R3-AUDIO-7 |
| ES8388 pops (ESP-ADF driver writes undocumented registers, noise between I2S init and enable) | carlosderseher.github.io/snapclient | Lite: firmware sequencing (requirement L4). Power-loss thump on the sub: R3-AUDIO-2 |
| PCM5102A pop at power down / clock halt | TI SLAS859C 11.2 | mostly immune (ground-centred, clock-halt mute); XSMT improvement R3-AUDIO-9 |
| ES9018K2M internal DVDD at high MCLK; single-channel hiss on DIY boards | ES9018K2M v3.7 p.7; squeezelite-esp32 disc. #395 | immune (49.152 MHz, 4.7 uF DVDD) |
| RF demodulation on unbalanced RCA inputs near the radio | general EMC practice; Lite already filters | core has it (R3-AUDIO-6) |
| Turn-on/off thumps on line/sub outs | brief | Lite has it (R3-AUDIO-2); core sub immune; amp-side handled by RESET/CMUTE |

## Known unknowns

- **+5V dip amplitude under Wi-Fi bursts (core SY8303).** R3-AUDIO-1 scales at 2.7 mVpp at the speaker per mV of
  dip. Measure at bring-up.
- **ES8388 LOUT2 behaviour when the output stage is disabled at power-up** (VROI 1.5k/40k to VREF?). Power-on
  thump unverified; measure at bring-up.
- **FLX4 master-out maximum.** 2.0-2.1 Vrms assumed.
