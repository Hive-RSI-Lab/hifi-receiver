# Review round 2 (review-3): system and field use (SYSTEM)

Domain: what goes wrong when a person uses the box at home for a year, on
all three boards and in the firmware's handling of the hardware. Boards
read: `boards/lite-main/kicad/gen/root.py`, `boards/core/kicad/gen/blocks*.py`,
both `.net` files. Firmware read: `firmware/components/hifi/{core,esp}`,
`firmware/board/*/{partitions.csv,sdkconfig.defaults}`, `firmware/patches`.
Then `docs/review-2/firmware-requirements.md`, `dispositions.md` and the other
round-2 reviews, so this review doesn't repeat them. Where a peer review
already has a field issue, it is listed under "Concurs" and not counted again.

**Counts:** 4 major (2 new, 2 repeat), 5 minor (1 repeat, 4 new), 6 notes.
Two of the majors (R3-SYSTEM-1, -2) are already fixed in the working tree
(the coordinator's governor change). Tests confirm both. The other two
(R3-SYSTEM-3, -4) are open.

Tests (`docs/review-3/sims/system/`, each exits non-zero on a FAIL):

| Test | Checks | Result now |
|---|---|---|
| `run_gov.sh` (`gov_burst.c` + the real `hifi_gov.c`) | input power against the charger contract: a 0 dBFS 45 Hz drop after a quiet passage (over-time at most 150 ms), and steady 0 dBFS (input at most the contract), Lite and core, 45-140 W contracts | PASS (working tree); FAIL at HEAD ac484b1: 1.39-1.64x for 507-597 ms, core at 20 V/65 W 104 W steady |
| `thermal_lite.c` (+ `hifi_gov.c`) | duty L1: Lite 30 s output at most 13 W/ch into 4 ohm after 180 s at -3 dBFS | PASS (working tree: 13.1 W); FAIL at HEAD: 20.2 W/ch, no limit |
| `fw_duties.py` | the round-1 firmware duties for field use are in the source: L1, L4, C1, C2-OTW, C5, saved input | FAIL: 5 of 6 missing (L1 now present) |

Run with:
`sh docs/review-3/sims/system/run_gov.sh`;
`cc -I firmware/components/hifi/core docs/review-3/sims/system/thermal_lite.c firmware/components/hifi/core/hifi_gov.c -lm && ./a.out`;
`python3 -I docs/review-3/sims/system/fw_duties.py`.

## Findings

### R3-SYSTEM-1 (major, both, new; fixed in the working tree, confirmed): the governor let the input pass the charger's contract

At HEAD the governor (`hifi_gov.c`) had two faults:

- **It couldn't see clipping.** `channel_w()` capped each channel's power at
  the clip power, and the limiter took its gain from that capped figure.
  When the program is louder than the rail, the gain left the peaks still
  clipping. On core that happens on every 20 V contract (0 dBFS is 25.8 V
  peak against a 19.0 V clip), and most chargers have no 28 V.
  - Core at 20 V/3.25 A, 0 dBFS: **104.4 W in, 1.61x the 65 W contract**,
    while its own estimate said 58.5 W.
  - Core at 20 V/5 A: 106.6 W.
  - Lite at 15 V/3 A: 60.6 W against 45 W.
- **Its 1 s window was slower than a charger.** A 0 dBFS 45 Hz drop after
  quiet music ran 1.39-1.64x over the contract for 507-597 ms. That excess
  is 9-21 J; the PVDD bulk holds 0.08 J (Lite 690 uF, 20 to 13 V) to
  0.43 J (core 1410 uF, 28 to 13 V). The charger trips or droops, VBUS
  falls under the 13 V floor, and the amp goes off.

Fix as applied: the limiter takes the unclipped power, `tau_s` 0.05 s and
`attack_s` 0.01 s, plus R3-POWER-3's peak limit. Re-run of `run_gov.sh`:

- every steady case is at or under 0.9x the contract (core 20 V/65 W: 58.4 W);
- the worst burst is core on 65 W at 1.08x for 5 ms (0.027 J against
  0.163 J of bulk).

Bring-up check: a 0 dBFS 45 Hz drop on a 65 W charger, scoping VBUS.

### R3-SYSTEM-2 (major, lite-main, repeat of THERMAL-3; fixed in the working tree, confirmed): duty L1 (Lite thermal governor) was not in the firmware

`firmware-requirements.md` L1 says Lite must hold 13 W/ch over 30 s into
4 ohm, or the TPA3118 reaches its latched 150 C shutdown in the box. At HEAD,
`hifi_gov_cfg_lite()` set no `thermal_out_w`, so the default was 0, which
means no limit. `thermal_lite.c` at HEAD: **20.2 W/ch at -3 dBFS, held for
good**. The working tree sets `thermal_out_w` 26 W and `thermal_tau_s` 30 s;
the last 30 s now average 13.1 W/ch (PASS). One residue stays: the 30 s
exponential average lets 20.2 W/ch (1.55x L1) through for the first ~31 s
of a loud passage. That is minor against THERMAL-3's margin (1/8 power,
Tj 94-106 C against the 150 C trip), but the thermal reviewer should
confirm it from the chip's thermal time constant. If the first-minute peak
matters, set `thermal_tau_s` to 10 s.

Lite's DJ direct needs no governor (pass). FLX4 at 2.1 Vrms, divided to
0.62 Vrms at LIN1, then the mixer at -3 dB, LOUT1 capped at -4.5 dB and
26 dB in the amp, gives 5.2 Vrms, **6.8 W/ch into 4 ohm** (4.5 W into 6 ohm).
That is 52 % of L1, and the input is 18 W, well inside any 20 V charger.
R3-AUDIO-4 has the same number.

### R3-SYSTEM-3 (major, core, repeat of DIGITAL-1; open): duty C1 (PHY clock off before a restart) is not in the firmware, so a reset can leave the box dead until it is unplugged

The hardware half of DIGITAL-1 holds: the TCA9539's RESET is on ESP_EN,
and R45 2.2k holds Y2's OE low. The firmware half doesn't exist. C1 says to
write P05 (PHY_RST, Y2 OE) low before any restart. Instead,
`hifi_main.c` `restart_cb`, `hifi_net.c:82` (OTA) and the panic path call
`esp_restart()` with P05 high; there is no `esp_register_shutdown_handler`
in `board_core.c` (`fw_duties.py`: FAIL).

That alone is harmless: a software reset doesn't re-read the straps. The
danger is the resets that move nothing on EN but do re-read them: a
**brown-out reset** and an RTC watchdog reset. A brown-out happens when
+3V3 dips under the ESP32's 2.43 V (`BROWNOUT_DET_LVL_SEL_0`) but stays above the
TCA9539's POR, for example on a charger hiccup or a VBUS sag (R3-SYSTEM-1,
R3-POWER-3). The expander then keeps PHY_RST high, so the 50 MHz clock is
on GPIO0 while the strap is read, and the ESP32 can boot into ROM download
mode. The box is then silent and off the network until someone unplugs it.
Lite is immune: its GPIO0 is MCLK, which the ESP32 drives itself and stops
on any reset.

Fix:
1. Firmware: `esp_register_shutdown_handler()` in `board_core.c` that writes
   port 0 with P05, P03 and P00/P01 low (PHY clock, PVDD and amps off)
   before every `esp_restart`.
2. Hardware, for the brown-out and watchdog cases no handler sees: an
   open-drain voltage supervisor on ESP_EN, 2.9-3.0 V threshold, SOT-23
   (Diodes APX803S-29 / TI TPS3839 class; pick the LCSC part at fix time).
   Any +3V3 dip then pulls EN low, which resets the TCA9539 too, and R45
   holds OE low through the strap read. Espressif's hardware design
   guidelines recommend a supervisor on EN when the supply can sag.

Contingency if there is no room: a DNP footprint for it beside C32.

### R3-SYSTEM-4 (major, core, new; open): DJ direct has no thermal limit, and OTW is never acted on

On core the FLX4 reaches the amps through the TS5A23157 with no DAC, so it
gets no volume, no governor and no thermal limit (`firmware.md` "Not
covered"). The thermal limits from THERMAL-2 / duty C2 exist only in the
digital path: Plus 19 W (9.6 W/ch into 4 ohm), Ultra 4.4 W/ch. C2's other
half, "OTW_CLIP low over 200 ms: -3 dB every 2 s", isn't implemented
either: `EXP_AMP_OTW` is read only in the self-test (`fw_duties.py`: FAIL).
The INA186 current is read (`vbus_i_a`) but feeds nothing.

Numbers. The FLX4 at its rated 2.1 Vrms gives 11.6 Vrms at the speaker
(R3-AUDIO-12), 33.6 W/ch for a sine into 4 ohm. DJ program sits 8-10 dB
under its peak, against a sine's 3 dB, so 6.7-10.6 W/ch average. Against
the budgets:

- Ultra (4.4 W/ch): **1.5-2.4x over**.
- Plus (9.6 W/ch): 0.7-1.1x.

At the budget each chip is ~2.0 W and Tj 110 C. At 1.5-2.4x the chip
reaches OTE, FAULT latches, the firmware retries after 1 s, and after 3
faults in 60 s it latches the amps off. The party stops until someone calls
`amp/reset`.

Fix (firmware; no board change):
1. Make the governed path the default line-in route: ES7243E, then I2S RX,
   then the DSP, then the DAC. That is also R3-AUDIO-3's fix, which feeds
   the subs. Keep DJ direct as an opt-in "zero latency" mode.
2. In DJ direct, keep a 60 s average of ISENSE x VBUS. When it passes the
   C2 thermal budget divided by 0.88 (Ultra one zone ~14 W in, Plus ~26 W),
   or OTW stays low for 200 ms, switch DJ_DIRECT off into the governed path.
   Hold the amps in RESET across the switch (C5).
3. Implement C2's OTW step-down in the digital path.

### R3-SYSTEM-5 (minor, both, repeat of AUDIO-17 and AUDIO-9; open): the pop duties L4 and C5 are not implemented

- **L4 (Lite):** "SDZ high only ... at least 2.5 s after the codec's VMID is
  enabled". `hifi_power.c` releases SDZ 600 ms after PG (settle 200 ms +
  soft start 400 ms), whatever the codec is doing. Squeezelite sets up the
  ES8388 only when its output starts, which on first set-up is after
  Wi-Fi joins. So the amp is running when VMID ramps.
  - Estimate: 1.65 V ramp, tau 0.25 s, through C60 1 uF into the TPA3118's
    30 kohm (SLOS708G table 1, 26 dB): tau 30 ms, 0.15 V peak at the input,
    **~3 V peak at the speaker**.
  - It is a 3-5 Hz thump (1.5 W peak into 6 ohm): seen on the cone,
    faintly heard. Every cold start does it.
- **C5 (core):** "hold both amps in RESET across a DJ_DIRECT change".
  `board_core.c hifi_bsp_route()` flips EXP_DJ_DIRECT with the amps running.
  AUDIO-9's ~0.8 V step at the speaker is a click at every Input press.

Fix:
- L4: gate `amp_on` on a codec-ready flag. Set it in `hifi_bsp_route()` once
  `codec_apply()` succeeds, plus 2.5 s.
- C5: in `hifi_bsp_route()`, when the input changes, clear AMP_A_RST/AMP_B_RST,
  wait 20 ms (CMUTE), write DJ_DIRECT, then release both resets in one write.

### R3-SYSTEM-6 (minor, core Plus, new): Plus's speaker terminals and right tap don't match how a person wires a stereo amp

Plus plays L on zone A (J6) and R on zone B (J11), and both chips play both
channels (`HIFI_SPLIT`: one input pair, AMP_IN_L/R, into both). J7 (A_R) and
J10 (B_L) are live too. The thermal limit (19 W) assumes one loaded channel
per chip. A person who wires the pair to J6 + J7, zone A's own "L" and "R",
puts both channels on one chip at twice the split's heat. That is THERMAL-2
territory: OTW, then OTE.

Also, the right speaker tap (R136/R137) sits on A_SPK_R+ (J7). On Plus that
output is unloaded, so the loopback "spk_r+" measures a channel nobody
hears (netlist: R136 pin 1 on /A_SPK_R+ with J7; zone B has no taps).

Fix (for the binding-post task):
- On Plus fit or wire only the J6 and J11 posts, marked L and R.
- Silk J7/J10 "Ultra only".
- Firmware: report taps on Plus as L only.

### R3-SYSTEM-7 (minor, lite-main, new): the sub-out RCA has the weakest ESD path

The usual ESD route into an RCA input is the plug's centre pin touching
before its shell.

- Lite J3: centre, then R36 100R, then C47 10 uF (a short at ESD speeds),
  then ES8388 LOUT2. An IEC 61000-4-2 8 kV contact pulse (150 pF/330 ohm)
  puts ~8 kV/(330+100) = **~19 A** peak into the codec's pin diodes.
- Core sub out: R92 470R + C71 1 nF, ~10 A.
- Lite line in: R30 15k, 0.5 A. Core line in: R100 4.7k, ~1.6 A.

Fix: a bidirectional 5 V TVS (SOD-523/SOD-882, about 10 pF, e.g. Nexperia
PESD5V0S1BB class) from each of SUB_L/SUB_R to GND at J3. Bidirectional,
because the sub out swings ±1.4 V about 0 V after C47/C48. Or raise
R36/R38 to 470R (still 340 kHz with 1 nF of cable).

### R3-SYSTEM-8 (minor, core, new): the firmware asks for a voltage without knowing the source offers it

`hifi_ch224_request()` writes VREQ 20 V (or 28 V) whenever the status
register shows any PD contract. It never checks the source's capabilities,
which the CH224Q has at 0x60-0x8F, and it writes once (`vreq_sent`). Two
cases follow:

- A laptop USB-C port at bring-up, 5 V PDO only: what the CH224Q does when
  asked for a voltage the source doesn't have (Reject, Hard Reset loop,
  stays at 5 V) isn't in WCH's datasheet (known-unknowns power:5). A
  Hard Reset loop would reboot the board every few seconds on the laptop.
- An EPR charger with a non-EPR cable: 28 V is asked for and never
  arrives, and there is no fall-back to 20 V.

Fix: read 0x60-0x8F (SRCCAP), request only an offered fixed PDO (28 V, else
20, else 15 V), retry 20 V if EPR isn't active 2 s after asking for 28 V,
and use that PDO's voltage and current for the contract. This sits beside
R3-POWER-4 (no VBUS reading on core) and R3-POWER-5 (EPR step).

### R3-SYSTEM-9 (minor, both, new): the input choice is lost on every reboot

`hifi_main.c` starts `input = HIFI_INPUT_STREAM` and never saves it. Any
charger blip, OTA reboot or crash during a DJ set brings the box back on
the stream input with the FLX4 silent, until someone presses Input or opens
the app. Fix: save `hifi_input` to NVS in `op_set_input`/`toggle_input` and
restore it in `hifi_init`. On core, release the amps only after the switch
is set (C5).

### Notes

- **N1 (both): bring-up on a laptop.** One USB-C port carries both power
  and the CH340. A laptop gives 5 V only, so PG stays high, Lite holds PVDD
  off in hardware, and the amp can't be tested while on the serial console.
  The amp is tested on a charger, through the API over Wi-Fi.
  - Lite's TPS54302 runs at 100 % duty from 5 V (+5V ≈ VBUS − 0.3 V; UVLO
    falling 3.6 V, SLVSDG6C table 5.5), so the ESP32 and codec run.
  - Core's +5V is 4.35-4.6 V (POWER-13). That passes for the ESP32/PHY.
- **N2 (both): volume at boot.** The volume is restored from NVS (default
  30) and capped by `max_volume`. A source may push its own (Spotify
  Connect at 100 %) up to `max_volume`, whose default is 100. Suggest a
  first-boot `max_volume` of 80 and an app prompt.
- **N3 (core): Ethernet surge/ESD.** HR911105A magnetics (1500 Vrms) with
  their own Bob-Smith termination, and the shield on GND. The board floats
  (isolated PD charger), so a common-mode surge returns through the RCA
  shields to an earthed sub or laptop. There is no line-side TVS, which is
  usual for consumer gear (WiiM/Arylic have none). Accepted.
- **N4 (both): IR in sunlight and LED light.** The IRM-3638T has its own AGC
  and the 100R/4.7 uF supply filter from the Everlight note. NEC frames are
  decoded by squeezelite's RMT, and learned codes are filtered by address.
  Nothing more is needed; check at bring-up under a dimmed LED lamp.
- **N5 (both): Wi-Fi in the box.** The box is PETG (no RF loss to speak
  of). Core has an antenna keep-out on all 4 layers and 2 mm wall gap on E.
  Lite has a 2.5 mm side clearance (DIGITAL-18, contingency WROVER-IE).
  Accepted; bring-up reads RSSI in the closed box.
- **N6 (both): speaker-wire shorts, firmware bricking.** Immune. TPA3118 and
  TPA3220 OC latch FAULT; the firmware drops SDZ/RESET for 1 s, retries,
  and latches after 3 in 60 s. Two OTA slots plus a factory recovery app,
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, and a trial judge
  (`hifi_boot.c`, 120 s) that rolls back a bad image.

### Concurs (found by peer reviews this round, not counted here)

- R3-POWER-4: core has no VBUS reading; `hifi_ch224_vbus()` assumes 20 V.
- R3-POWER-3: average-only governor and charger peak current. Its peak
  limit is in the working tree; `run_gov.sh` confirms it.
- R3-AUDIO-3: core subs are silent with the FLX4. In the person's own flow
  (FLX4 on RCA in, subs on RCA out) the subs get nothing on Plus/Ultra. The
  ES7243E clock list (rev 5.0 sec 3: 64 fs and up) suggests BCLK must go to
  64 fs (32-bit slots) for MCLK-from-BCLK.
- R3-AUDIO-2: Lite sub-out thump on power loss (no mute on C47/C48).
- R3-AUDIO-7: earth loop through the RCA sleeves (earthed laptop + earthed
  sub).

## The person's flow, end to end

| Step | Lite | core (Plus/Ultra) |
|---|---|---|
| USB-C charger plugged in | CH224A asks 20 V (R1 120k); PG low gates Q4 (netlist: Q4 pin 2 = PD_PG = U5 GPIO4); firmware sees real VBUS (R13/R14) and PVDD (R17/R18); amp after 600 ms. Pass (pop: R3-SYSTEM-5) | CH224Q asks 28 V (R2 210k); non-EPR chargers rely on the firmware's 20 V request (R3-SYSTEM-8); PG gates Q2 (netlist: Q2 pin 2 = PD_PG). VBUS guessed (R3-POWER-4) |
| Laptop for bring-up | 5 V only: PVDD off by PG, console works (N1) | same, but VREQ to a 5 V-only source is unknown (R3-SYSTEM-8) |
| Wi-Fi set-up | AP mode, web app; codec set up only when squeezelite starts (R3-SYSTEM-5) | same, plus Ethernet |
| Speakers | DB128V 4-way; any 4-8 ohm; OC latch and retry (N6) | which terminals on Plus (R3-SYSTEM-6) |
| FLX4 on RCA in | 15k/10k, -4.2 dBFS, DJ direct 6.8 W/ch (pass); input lost on reboot (R3-SYSTEM-9) | DJ direct: no thermal limit (R3-SYSTEM-4), subs silent (R3-AUDIO-3), click on switch (R3-SYSTEM-5) |
| Subs on RCA out | ES8388 LOUT2 through 10 uF/100R; thump on power loss (R3-AUDIO-2); ESD (R3-SYSTEM-7) | PCM5102A, ground-centred, 470R/1nF; fine on the stream path |
| Charger unplugged mid-play | VBUS falls ~2 V/ms at 1.5 A on 690 uF; the 10 ms poll drops SDZ under 13 V within ~3-13 ms; the amp's UVLO catches the rest; sub thump (R3-AUDIO-2) | hardware PG cuts PVDD (POWER-6 holds, R404/Q3 fast-off); firmware learns within 500 ms; soft thump (R3-AMP-6) |
| Laptop unplugged (bring-up) | board browns out with the laptop; nothing is latched | a brown-out with PHY_RST high can leave it in download mode (R3-SYSTEM-3) |
| Charger swapped for a weaker one | re-plug, new contract read (0x50 every 500 ms); governor re-budgets (R3-SYSTEM-1, fixed) | same current; VBUS guessed (R3-POWER-4) |
| Multi-port charger re-shares power | PG high during the renegotiation: PVDD off in hardware; then the new contract | same |

## Field issues searched, and what our board does

| Issue (comparable products) | Our board | How, with the number |
|---|---|---|
| PD charger trips at volume (Fosi/Aiyima PD amps, "cuts out on bass") | had it, fixed | R3-SYSTEM-1: 1.6x for 0.6 s and 1.6x steady at HEAD; now ≤1.08x for 5 ms (`run_gov.sh`) |
| Charger lacks the asked-for voltage (PD trigger boards; CH224 PG stays high) | Lite immune; core has it | Lite measures VBUS, start floor 14 V; core R3-SYSTEM-8 / R3-POWER-4 |
| Pop at power-on (ESP32-A1S/LyraT ES8388 boards, squeezelite-esp32 issues) | has it (minor) | R3-SYSTEM-5: ~3 V subsonic at the speaker on Lite |
| Pop at power-off / on the sub out (WiiM Amp sub-out complaints) | has it | R3-AUDIO-2 (Lite), R3-AMP-6 (core, soft) |
| Pop on source switch | core has it | R3-SYSTEM-5 (C5 missing), AUDIO-9 0.8 V |
| Hum, laptop-powered DJ controller + earthed sub | has it (minor) | R3-AUDIO-7 |
| Wi-Fi drops inside a box | accepted | N5; PETG, keep-outs |
| Overheating in a closed box (TPA3116/3118 boards, WiiM Amp) | Lite fixed; core DJ open | R3-SYSTEM-2 (13.1 W/ch), R3-SYSTEM-4 (Ultra DJ 1.5-2.4x) |
| Speaker-wire short at the terminals | immune | N6, OC latch + 1 s retry, latch after 3 |
| Dead after lightning/ESD on Ethernet | accepted | N3 |
| Dead after a glitch until replugged (ESP32 strap/oscillator on GPIO0, LAN8720 boards) | core has it | R3-SYSTEM-3 |
| Firmware update bricks it | immune | N6, rollback + recovery |
| IR fooled by sunlight/LEDs | immune | N4 |
| Volume jumps to full at boot | partly | N2: restored volume, but a source may set 100 % |
| Box forgets its input after a power blip | has it | R3-SYSTEM-9 |

## Round-1 fixes checked (on the board files and firmware)

| Id | Holds | How |
|---|---|---|
| POWER-6 | yes | core.net /power_in/PD_PG = Q2.2, R20, U1.10; R404/D5/Q3 fast-off present (blocks.py) |
| POWER-3/4 | yes (Lite) | R6 220k, C19 4.7 uF in root.py; Q4 emitter on PD_PG (lite-main.net) |
| POWER-13 | Lite yes; core partly | Lite PG gate + v_start 14 V on measured VBUS; core trusts the status bits (R3-POWER-4) |
| THERMAL-3 (L1) | HEAD no; working tree yes | `thermal_lite.c`: 20.2 W/ch at HEAD, 13.1 W/ch now |
| THERMAL-2 (C2) | partly | thermal_out_w 9/17 W, Plus 19 W in code; the OTW step-down is missing (R3-SYSTEM-4) |
| AUDIO-17 (L4) | no | R3-SYSTEM-5 |
| AUDIO-9 (C5) | no | R3-SYSTEM-5 |
| DIGITAL-1 (C1) | hardware yes, firmware no | TCA9539 ~RESET = ESP_EN, R45 2.2k; no shutdown handler (R3-SYSTEM-3) |
| AUDIO-6 (L3) | yes | ES8388_OUT1_FULL 27 (-4.5 dB), mixer LIN -3 dB (0x18) |
| AUDIO-7 (L7) | yes | R30/R33 15k, R31/R34 10k (root.py) |
| L2 fault recovery | yes | 1 s retry, latch after 3 in 60 s (duty says 10 min; stricter in practice) |
| DIGITAL-11 (L9) | yes in function | two OTA slots of 3.06 MB + 1.75 MB recovery, rollback on; the offsets differ from L9's table |
