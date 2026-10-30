# Firmware

What runs on the receiver's ESP32-WROVER-E, why it is built this way, and
what is known and not known about it. How to build, test and flash it is in
[`firmware/README.md`](../firmware/README.md); the test API is
[`test-api.md`](test-api.md).

## The approach: squeezelite-esp32 plus one component

[product.md](product.md) chose
[squeezelite-esp32](https://github.com/sle118/squeezelite-esp32) as the
streaming core: it already does Spotify Connect (cspot), AirPlay 1, the
Bluetooth A2DP sink, Lyrion/LMS with multi-room sync, buttons, IR, OTA and
Wi-Fi setup on exactly this module, and it already supports the ES8388
codec Lite uses.

So the firmware is upstream at a pinned commit (`firmware/upstream.lock`,
master-v4.3 `ee1720c`, ESP-IDF v4.4.8) plus:

- **one patch** of hooks (`firmware/patches/`), small enough to carry to a
  newer upstream: a board choice in Kconfig, a boot hook, a DSP hook on
  every output block, a test-signal source, HTTP registration, and hooks on
  the volume, the now-playing display calls, IR codes and button actions;
- **one component**, `components/hifi`, which is ours: the power path, the
  governor, test signals, the test API and the web app.

Forking the whole tree was the alternative. It was rejected: a patch over a
pinned commit says exactly what we changed and keeps upstream's fixes one
`upstream.lock` edit away.

Inside the component, the logic (`core/`) is plain C with no ESP-IDF in it,
so it is tested on a PC (`make -C firmware/test`) with a mock board. The
board code (`esp/`) is thin: it reads pins, I2C and the ADC, and drives
GPIOs and squeezelite.

## Who owns what on lite-main

| Function | Pins | Owner |
|---|---|---|
| ES8388 I2S (MCLK 0, BCLK 27, LRCK 25, DOUT 26, DIN 35), I2C (SDA 33, SCL 32) | | squeezelite (`dac_config`, `dac_controlset` in the Kconfig patch) |
| Buttons: vol+ 13, vol− 36, play 19 | | squeezelite (`actrls_config`): short = vol+/vol−/play-pause, long = next/previous/power |
| IR receiver | 14 | squeezelite decodes (`set_GPIO 14=ir:nec`); `hifi` sees each code first and keeps the learned ones |
| Status LED | 22 | squeezelite (`set_GPIO 22=green:1`): blinks during Wi-Fi setup |
| PD contract: CH224A at I2C 0x22, PG | 4 | `hifi` |
| PVDD switch enable | 18 | `hifi` |
| Amp SDZ, FAULTZ | 21, 23 | `hifi` |
| VBUS ÷ 11 | 34 (ADC1 ch 6) | `hifi` |
| PVDD ÷ 11 (PVDD_SENSE, after the switch) | 39 (ADC1 ch 3) | `hifi`: confirms the switch closed |
| ES8388 input routing (DJ direct) and sub level | I2C | `hifi`, after squeezelite's codec init |

The pin map is the Lite board's (`boards/lite/README.md`), agreed with its
designer on 2026-10-28.

## Power

### The sequencer (`core/hifi_power.c`)

| State | PVDD | Amp | Leaves when |
|---|---|---|---|
| `wait_pd` | off | off | PG says the contract is good and VBUS has sat in 14–25 V (the start window) for 200 ms → `soft_start` |
| `soft_start` | on | off | 400 ms later and PVDD read up (≥ 85 % of VBUS) → `on` (lite-main's gate RC holds PVDD off for 80–160 ms and has it at 90 % 110–210 ms after PVDD_EN, at 15–20 V); PVDD still down 1 s after PVDD_EN → `no_pvdd` |
| `on` | on | on when enabled, not faulted and there is audio | PG drops → `wait_pd`; VBUS < 13 V (the run floor) → `undervolt`; > 25 V → `overvolt`; PVDD under 85 % of VBUS for 50 ms → `no_pvdd` |
| `undervolt`, `overvolt` | off | off | VBUS back in 14–25 V for 1 s → `soft_start` |
| `no_pvdd` | off | off | 1 s later with the supply good → `soft_start`; the third inside a minute holds it here until `POST /api/v1/amp {"reset": true}` or a re-plug |

- **9 V and 12 V contracts leave the amp off.** lite-main's PVDD switch
  gets VBUS × 0.31 of gate drive (R5 100k over R6 220k): 2.2–2.3 V at
  9 V, which an AO4485 at its 2.5 V maximum threshold doesn't turn on
  at, and 3.7 V at 15 V (`docs/review-2/second-read.md`). The amp starts
  at 14 V or more and, once on, stays on down to 13 V: a 15 V contract
  (14.25 V at −5 %) sagging under load and cable drop doesn't cycle it,
  and a 12 V contract (12.6 V at most) is still kept out.
- **A laptop port can't power the amp.** On a 5 V port PG never says the
  contract is good, and lite-main ANDs PG with PVDD_EN in hardware too.
- **PVDD is confirmed, not assumed.** Lite reads PVDD after the switch on
  GPIO39 (R17 100k over R18 10k, ~1.8 V at 20 V). The amp leaves shutdown
  only once the switch is seen closed, so a switch that never closes (Q1
  open, its gate drive too weak) or that opens or sags while playing (Q1
  half on, burning) shows as `no_pvdd` instead of a silent amp. A board
  with no PVDD sense (core) passes NaN and runs on time alone.
- **Order:** the amp goes into shutdown before PVDD opens and comes out
  after it closes.
- **Faults:** FAULTZ low while the amp runs (after 50 ms of blanking) holds
  it off for 1 s, then retries. Three faults inside a minute latch it off
  until `POST /api/v1/amp {"reset": true}` or a re-plug.
- **Idle:** after 10 minutes of digital silence the amp goes into shutdown
  (PVDD stays up); the first non-silent block wakes it.

### The PD contract (`core/hifi_pd.c`)

The CH224A's status register (0x09) says which protocol was negotiated and
its current register (0x50) the contract's maximum current in 50 mA steps
(PD only). VBUS from the ADC, snapped to the nearest fixed PD level, gives
the voltage. The CH224A can't report the source's full capability list
(registers 0x60–0x8F are CH224Q-only), and its voltage register is
write-only. The firmware never writes it: Lite's resistor asks for 20 V.

### The governor (`core/hifi_gov.c`)

A 65 W charger shuts its output off if the box draws more than it offers,
and you hear that as a dropout. Two Lite channels at full sine into 4 Ω would
draw about 94 W. So the governor holds the 1 s average input power under
90% of the contract:

1. On every output block, after squeezelite's volume, equalizer and
   loudness (the last thing before I2S), it takes each channel's mean
   square. The order matters: the API's EQ allows +12 dB a band, and a
   governor that measured before the EQ would see a sixteenth of the bass
   power the amp actually gets.
2. It turns that into speaker power through the analog chain: the ES8388's
   full scale (≈0.9 Vrms) × the −3 dB cap × the TPA3118's 26 dB, which is
   17.98 V peak for a 0 dBFS sine, into 4 Ω, capped at the amp's clipping
   power for the supply (95.3% swing, measured on hwde's made board).
3. Input power = speaker power / 0.88 + 2.5 W for the rest of the board.
4. If the average would pass the budget, it applies the gain that puts it
   on the budget: 50 ms attack, 2 s release, a linear ramp inside each
   block, never more than −26 dB.

On a 65 W charger, a 0 dBFS sine on both channels is held at −2.1 dB; music,
which averages about 1/8 of its peaks, never reaches it. The host tests check
both.

**Not covered:** DJ direct is analog (line in → codec mixer → amp), so the
governor can't see it. The codec's −3 dB cap and the line in's −6 dB
attenuator limit it; a DJ deck at full level into 4 Ω could still trip a
65 W charger. Plus/Ultra read the input current from an INA186 (shown in
`/state`, not yet fed back into the governor); their DJ direct bypasses
the DAC too, so the governor can't act on it there either.

### Numbers to calibrate at bring-up

These are estimates from datasheets and the made amp board. The bring-up
(plan item 10) measures them and they go in `hifi_gov_cfg_lite()`:

- `vfs_peak_v`: the amp output for a 0 dBFS 1 kHz sine (`hifi tone sine 1000 0 both 5`, with `"force": true` over the API).
- `p_idle_w`: the input power with the amp on and silent (`hifi tone silence`).
- `efficiency`: input against output power at 10–40 W.
- The ES8388's full scale and whether the −3 dB cap keeps a 0 dBFS sine out
  of clipping at 20 V.

## Test signals

`POST /api/v1/tone` (or `hifi tone ...` on the console) plays a sine, a log
sweep, pink noise or silence at 48 kHz. They enter squeezelite's output
buffer the way the Bluetooth sink does (`hifi_sink_*` in the patch), so they
go through the same I2S path, governor and codec as music. While one plays,
the volume is unity so the level is what was asked for; the previous volume
comes back after. At most 100 ms is queued, so `stop` is prompt (with a
10 ms fade).

## Volume, now playing and the remote

These are the parts the app needs beyond the test API's first version. Each
one is a small hook in the patch, with the logic in `core/`.

- **One volume** (`core/hifi_vol.c`). Every source sets squeezelite's
  output gain through `set_volume()`: LMS (`audg`), Bluetooth (AVRC),
  AirPlay and Spotify. The patch hands that gain to `hifi_volume_hook` first.
  - The receiver keeps it as `volume`, caps it at `max_volume` and holds it
    at 0 while muted.
  - The app, and the remote and buttons when nothing streams, set the gain
    directly with `hifi_output_gain()`.
  - The volume and the maximum go to NVS once they have been still for 3 s.
    At boot the saved volume is applied once the output is up, unless a
    source has already set its own. Without that, a phone that doesn't send
    a volume would play at squeezelite's initial gain.
  - **In DJ direct** the volume goes on the codec's LOUT1/ROUT1 instead.
  - The curve, and what a source does and doesn't learn back, are in
    [test-api.md](test-api.md#post-apiv1volume).
- **Now playing** (`core/hifi_meta.c`). The AirPlay, Bluetooth and Spotify
  sinks call squeezelite's displayer:
  - `displayer_control` when a source takes over, plays, pauses or lets go;
  - `displayer_metadata` with artist, album and title;
  - `displayer_timer` with elapsed and duration.

  The patch passes the same calls to `hifi_display_*` before the
  displayer's "no display" early return, so Lite gets them with no screen.
  Text is cut at a whole UTF-8 character.

  LMS has no text path: it draws its now-playing as display graphics. For
  LMS, `playing` is only `{"state": "play"}`. Reading LMS's track would
  need its JSON-RPC from the app or the box. That is left for later.
- **The remote** (`core/hifi_ir.c`). The patch shows squeezelite's
  `ir_handler` each decoded frame first (`hifi_ir_hook`):
  - While learning, the frame binds to the action.
  - A learned code runs its action. Repeats closer than 250 ms are a held
    button.
  - Any other code goes on to squeezelite (LMS's remote, its default map).

  Board buttons pass through `hifi_button_hook` for one reason: vol+/vol−
  step the receiver's own volume when nothing streams or in DJ direct.
  Without it they would go to LMS, which may not be there. Everything else
  stays squeezelite's.

## The DJ input and the subs (ES8388)

- **Speaker level cap.** The first-spin board review (task 261029-020622)
  caps the speaker outputs (LOUT1/ROUT1) at −4.5 dB, code 27, and the line-in
  bypass at −3 dB, because 0 dB overdrives the amp. `hifi` writes those
  codes after squeezelite's codec init, in both modes.
- **DJ direct** routes LIN1/RIN1 into the output mixers at −3 dB and takes the
  DAC out of the mix (DACCONTROL17/20 = 0x58, analog inputs powered with
  ADCPOWER = 0x38). **Stream** restores squeezelite's setting (0xB8, 0xFC).
  These register values are from the ES8388 datasheet and are **not yet
  checked on a board**.
- **Subs:** LOUT2/ROUT2 carry the same stereo signal as the speakers, full
  range (the subs use their own crossover on Lite). Their level is −9 dB
  down to −45 dB in 1.5 dB steps (about 0.35 Vrms at full scale), set from
  squeezelite's codec init on; off powers the pair down. The −9 dB ceiling
  keeps the line's negative swing inside what the power-fail mute's
  2N7002s pass cleanly (docs/review-4/als.md R4-ALS-1).

## Plus and Ultra: the core board

Plus and Ultra share the core board (`boards/core`) and one firmware,
built twice: `firmware/board/core` selects the Kconfig board `HIFI_PLUS`,
`firmware/board/ultra` `HIFI_ULTRA` (`HIFI_BOARD=core` or `ultra` for
`firmware/tools/build.sh`). The board code is `esp/board_core.c` behind the
same interface as Lite's (`esp/hifi_bsp.h`); the pins are in
`esp/hifi_board_core.h`, from `boards/core/architecture/blocks.md`.

| Function | Pins | Owner |
|---|---|---|
| I2S0 to the ES9018K2M (BCLK 2, LRCK 4, DATA 5) and from the ES7243E (DIN 36) | | squeezelite (`dac_config` `model=I2S`, no I2C of its own) |
| The shared I2C (SDA 13, SCL 14, port 1) | | squeezelite's system I2C (`i2c_config`); `hifi` talks to every part on it |
| Ethernet: LAN8720 over RMII (clock in on GPIO0), MDC 23, MDIO 18 | | squeezelite (`eth_config`) and ESP-IDF's EMAC |
| IR receiver (Ultra: the front board's, over the FFC) | 35 | squeezelite (`set_GPIO 35=ir:nec`), as on Lite |
| PD contract: CH224Q at 0x22 | I2C | `hifi`: once a PD contract is in, asks for 28 V when the charger offers EPR, else 20 V |
| Input current: INA186, 0.25 V/A | 34 (ADC1 ch 6) | `hifi` |
| I/O expander (TCA9555 at 0x20, or the review's TCA9539 at 0x74–0x77: the first that answers) | I2C, INT on 39 | `hifi` |
| PVDD_EN, AMP_A_RST, AMP_B_RST, DAC_EN, PHY_RST, DJ_DIRECT, LED | expander P00–P07 | `hifi` |
| AMP_FAULT, AMP_OTW, knob A/B, knob push, two buttons | expander P10–P16 | `hifi` |
| Sub DAC (PCM5102A): I2S1 as a slave on GPIO2/4, data on 12 | 12 | `hifi` |

- **Boot:** the expander's outputs come up with the DAC out of reset and
  the Ethernet clock on (the network starts after our component), PVDD
  and both amps off. Then the ES9018K2M gets a 16-bit I2S word (register 1
  bits 7:6), since squeezelite's 16-bit build clocks 16-bit words and the
  Sabre resets expecting 32.
- **Power:** the same sequencer with core's numbers (`hifi_power_cfg_core`):
  start at 14 V, run 13–30 V (inside the TPA3220's 7–32 V), 100 ms soft start
  (PVDD ramps 10–90 % in about 47 ms). At 14 V, Q1's gate sees VBUS/2 = 7 V,
  so core doesn't need Lite's 15 V contract floor for its switch to turn on. The board has no VBUS divider, so VBUS is
  taken from the contract: 28 V in EPR, 20 V in another PD contract, 5 V
  without one. A charger whose highest voltage is under 20 V would read as
  20 V here; bring-up should check what the CH224Q does then.
- **Amps:** the order is Lite's (reset before PVDD opens, out of it after).
  Zone B (Ultra) runs off zone A's oscillator and only syncs if both leave
  reset in the same write, so turning zone B on or off puts both in reset
  for one 10 ms step. The governor counts twice the amps while zone B
  plays.
- **Amp heat** (`thermal_out_w`, `thermal_out2_w`): the TPA3220s have no
  heatsink and cool into the board and the vented box. Plus runs both
  (`HIFI_SPLIT`: L on zone A, R on zone B, one channel per chip), and its
  governor holds the 60 s average speaker power to 19 W, 9.6 W per channel
  into 4 ohm. Ultra holds 9 W with one zone and 17 W with both. Peaks
  still reach 77 W per channel. The OTW back-off and FAULT retry in
  `docs/review-2/firmware-requirements.md` C2 sit behind it.
- **Knob and buttons** (`core/hifi_exp.c`, host-tested): the knob is the
  volume, 2 a click; while a source streams, each click goes to it as the
  buttons do on Lite. Knob push: play/pause (hold: power). Button 1: source
  (hold: mute). Button 2: next (hold: previous). The product page's
  "preset" button needs presets, which don't exist yet. Ultra reads the
  knob on the core's expander through the FFC; its front-board buttons
  and screen are not built yet (below).
- **LED:** on with a network, a slow blink without one (setup), a fast
  blink with the amps latched off by faults.
- **Subs:** I2S1 runs as a slave on I2S0's clocks (the GPIO matrix routes
  GPIO2/4 into its TX clock inputs), so the sub DAC costs one pin. It
  starts once the GPIO matrix shows squeezelite has put I2S0's bit clock on
  GPIO2, because that setup turns the pins' inputs off. Each output block, after
  the equalizer and the governor, goes to it as the mono sum of the mains
  at the sub level (−40 to 0 dB), full range: the subs' own crossover does
  the rest, as on Lite. Nothing waits on it: if its DMA is full, the block
  is dropped.
- **DJ direct** is the TS5A23157 in front of the amps: the line in goes to
  them directly, with no volume, governor or sub.

## Network, app and console

- **Web app** at `/` (from `app/`), squeezelite's own page at `/squeezelite/`.
- **Hostname** `hifi-lite`, so `http://hifi-lite.local` works. mDNS also
  advertises `_hifi._tcp` for the bench scripts.
- **Console** over the USB-C serial port: `hifi state`, `hifi selftest`,
  `hifi tone ...`, beside squeezelite's own commands.
- **Names:** AirPlay, Bluetooth and the setup access point are `hifi-lite`
  until renamed (`POST /api/v1/name`).
- **The box's address** (`/api/v1/state` `net.ip`, the self-test's
  `network`, mDNS, the relay's status, the trial boot) is the active
  interface's: the default route's, else any up interface but the setup
  access point. Wi-Fi station or Ethernet alike (`hifi_net_ip()`), so
  Plus/Ultra on a cable and QEMU's open_eth report one.

### In QEMU (hive-fw's `fw qemu`)

`fw qemu` boots the main app (`ota_0`) with Ethernet on QEMU's open_eth and
drives the test API (hive-fw's `hifi-lite` target holds the QEMU settings).
Two things it has to work round:

- **`read_vbus()` and `read_pvdd()` hang:** `adc1_get_raw()` never returns
  in QEMU (it has no SAR ADC). fw shims it (`"adc1"` in its qemu settings);
  at 0 the power sequencer sits in `wait_pd` with the amp off, as on a
  laptop port.
- **The factory image (squeezelite's recovery app) skips Ethernet,** so a
  boot of `factory` never gets an address: boot `ota_0`.

## Remote bring-up, updates and self-measurement

Designed in [remote.md](remote.md); here is where it lives in the code.

- **`core/hifi_remote.c`:** a signed command off the relay is checked
  (Ed25519 against the build's `HIFI_HIVE_PUBKEY`, the board's own id, ids
  only going up, the clock once SNTP has set it) and run through the test
  API. `esp/hifi_relay.c` is the MQTT client (esp-mqtt, TLS with ESP-IDF's
  certificate bundle) and the libsodium check; it publishes the retained
  status, the replies and the events.
- **`core/hifi_ota.c` + `core/hifi_sha256.c`:** the update session (begin,
  chunks in order, the hash, commit); `esp/hifi_relay.c` writes it with
  `esp_ota_*` into the slot that isn't running.
- **`core/hifi_boot.c`:** the trial of a new image. `esp/hifi_relay.c` asks
  it every 2 s and marks the image good or bad
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), keeping the reason in NVS
  (`hifi_rb_from`, `hifi_rb_why`) for `/api/v1/ota` and the `rolled_back`
  event.
- **`core/hifi_capture.c`:** stats of a capture. `esp/hifi_main.c` routes the
  ES8388's ADC to LIN2/RIN2 (the taps) or LIN1/RIN1 (the line in), with the
  PGA at 0 dB and the ALC off, and reads I2S, which squeezelite now opens full
  duplex (`di=35` in the board's `dac_config`; the patch makes it parse
  `di`). The routing goes back to the input in use afterwards.
- **Partition table:** two 3.06 MB app slots plus the 1.75 MB recovery app
  (`board/lite-main/partitions.csv`).

**Unchecked on hardware:** the ES8388 ADC register values, I2S full duplex
with squeezelite's DMA settings, the rollback on a real flash, and the relay
over TLS. lite-main rev A has no taps, so `capture` with `loopback` answers
409 there; the review task (261029-020622) adds them. `HIFI_HAS_TAPS=1` in
the build or `hifi_taps=1` in NVS turns it on for that revision.

**The LAN and console are not signed.** As in squeezelite-esp32 itself,
anyone on the home network can call the HTTP API, firmware updates
included. Only the relay, which reaches past the home network, demands a
signature.

## Hi-res

The build is squeezelite-esp32's 16-bit one (`DEPTH=16`): 16-bit samples at
up to 96 kHz into the ES8388. That is above the ES8388's ≈96 dB anyway, so
Lite loses nothing. Plus/Ultra's Sabre DAC would gain from the 32-bit build
(`DEPTH=32`), which costs RAM and drops squeezelite's equalizer; decide that
in the Plus firmware task.

## What is not done, and what is unchecked

- **Nothing has run on hardware.** No board or dev kit was on the bench.
  The build is checked to compile and link (`firmware/README.md`); the logic
  is checked on a PC. Everything that touches pins, the codec or the PD sink
  is unchecked until bring-up.
- **To check first at bring-up:** that MCLK on GPIO0 plays with the
  auto-program transistor on the same pin; the CH224A's I2C idle levels
  (lite-main's R2/R3 can be removed if they sit above 3.6 V); the DJ-direct
  register values; FAULTZ's behaviour in shutdown.
- **Unchecked until a board:** volume, now playing and the remote. That
  means the ES8388 LOUT1/ROUT1 volume and mute in DJ direct, which phones
  send AVRCP metadata, and NEC decoding of real remotes on GPIO14.
- **Not built yet:** LMS's track text (see above). On Plus/Ultra: line-in
  capture from the ES7243E and the loopback taps (`capture` answers 404:
  the ADC's registers aren't set up), a crossover and per-sub delay for
  the subs, Ultra's optical input (the ES9018K2M's S/PDIF select), and
  Ultra's front board (its TCA9535 buttons, the LCD), whose pin map
  (`boards/ultra-front`) isn't on main yet.
- **Plus/Ultra, unchecked until a board:** the ES9018K2M's 16-bit word
  length at 32 BCLK per frame; I2S1 as a slave on I2S0's pins (the
  architecture's assumption: blocks.md; QEMU has no I2S, so neither
  squeezelite's I2S0 nor the sub's I2S1 starts there); the CH224Q's answer to a voltage
  request; the knob's direction (swap `enc_a`/`enc_b` in `board_core.c`
  if it turns the wrong way); the LAN8720 on the oscillator's clock.
