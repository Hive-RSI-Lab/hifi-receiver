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
  every output block, a test-signal source, and HTTP registration;
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
| IR receiver | 14 | squeezelite (`set_GPIO 14=ir:nec`) |
| Status LED | 22 | squeezelite (`set_GPIO 22=green:1`): blinks during Wi-Fi setup |
| PD contract: CH224A at I2C 0x22, PG | 4 | `hifi` |
| PVDD switch enable | 18 | `hifi` |
| Amp SDZ, FAULTZ | 21, 23 | `hifi` |
| VBUS ÷ 11 | 34 (ADC1 ch 6) | `hifi` |
| ES8388 input routing (DJ direct) and sub level | I2C | `hifi`, after squeezelite's codec init |

The pin map is the Lite board's (`boards/lite/README.md`), agreed with its
designer on 2026-10-28.

## Power

### The sequencer (`core/hifi_power.c`)

| State | PVDD | Amp | Leaves when |
|---|---|---|---|
| `wait_pd` | off | off | PG says the contract is good and VBUS has sat in 9–25 V for 200 ms → `soft_start` |
| `soft_start` | on | off | 100 ms later → `on` (lite-main's switch ramps PVDD in about 6 ms) |
| `on` | on | on when enabled, not faulted and there is audio | PG drops → `wait_pd`; VBUS < 9 V → `undervolt`; > 25 V → `overvolt` |
| `undervolt`, `overvolt` | off | off | VBUS back in range for 1 s → `soft_start` |

- **A laptop port can't power the amp.** On a 5 V port PG never says the
  contract is good, and lite-main ANDs PG with PVDD_EN in hardware too.
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
65 W charger. Plus/Ultra measure VBUS current with an INA180 and will
govern every path.

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

## The DJ input and the subs (ES8388)

- **DJ direct** routes LIN1/RIN1 into the output mixers at 0 dB and takes the
  DAC out of the mix (DACCONTROL17/20 = 0x50, analog inputs powered with
  ADCPOWER = 0x38). **Stream** restores squeezelite's setting (0xB8, 0xFC).
  These register values are from the ES8388 datasheet and are **not yet
  checked on a board**.
- **Subs:** LOUT2/ROUT2 carry the same stereo signal as the speakers, full
  range (the subs use their own crossover on Lite). Their level is 0 to
  −45 dB in 1.5 dB steps, up to +4.5 dB; off powers the pair down.

## Network, app and console

- **Web app** at `/` (from `app/`), squeezelite's own page at `/squeezelite/`.
- **Hostname** `hifi-lite`, so `http://hifi-lite.local` works. mDNS also
  advertises `_hifi._tcp` for the bench scripts.
- **Console** over the USB-C serial port: `hifi state`, `hifi selftest`,
  `hifi tone ...`, beside squeezelite's own commands.
- **Names:** AirPlay, Bluetooth and the setup access point are `hifi-lite`
  until renamed (`POST /api/v1/name`).

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
- **Not built yet:** absolute volume, mute and max volume in the API, the
  now-playing metadata, IR remote learning, and the Plus/Ultra board code
  (Sabre, sub DAC, Ethernet, INA180, display).
