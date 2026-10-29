# The bench: bring-up and test from a laptop

`bench/` finds a receiver board, flashes it, runs its self-test, and measures
power, THD+N, PVDD and chip temperature into dummy loads. It writes a
results page per session. It is plain Python 3 with no packages. pyserial,
esptool and python-sounddevice are used only when they're installed.

Its tests run against a **mock board** (`bench/hifibench/mock.py`): a
simulated Lite behind the same test API. That is how `python3
tools/hwcheck.py test` checks the bench on every merge, and how the web app
can be developed without a board (`python3 bench/bench.py mock`).

## Quick start

```sh
python3 bench/bench.py find                          # boards on USB (CH340) and mDNS (_hifi._tcp)
python3 bench/bench.py --mock bringup --soak-s 300   # the whole bring-up against the mock
python3 bench/bench.py bringup --image firmware/build --wifi 'SSID:password' \
    --capture arecord:hw:1,0 --temp-cmd 'my-thermocouple --celsius'
```

Every command takes `--url http://hifi.local` or `--port /dev/ttyUSB0` to
name the board. Without either, it browses mDNS and then looks for a CH340
on USB.

| Command | What it does |
|---|---|
| `find` | Lists CH340 serial ports and `_hifi._tcp` services. |
| `info` | Prints the board's `info` and `state`. |
| `flash IMAGE` | Runs esptool through the CH340's auto-program pair. IMAGE is an ESP-IDF build dir (`flash_args`) or a merged `.bin` at 0x0. `--dry-run` prints the command. |
| `selftest` | Runs the board's self-test. On a 5 V port it also checks from outside that the amp refuses to start. |
| `tone` | Plays a test tone (`--freq --level --channel --volume`, `--stop`). |
| `calibrate --vrms V` | Plays −20 dBFS, takes the meter's AC volts across the L load, and saves `volts_per_fs` in `bench/rig.json`. |
| `sweep` | Steps 1 kHz up into each load (`--loads 4,6,8`, `--governor on/off/both`) until 10% THD+N, a fault or `max_w`. Add `--soak` for the 1/8-power soak after it. |
| `soak` | Chip temperature at 1/8 of the measured (or predicted) 1% power into `--ohm`. |
| `bringup` | The staged run below. |
| `mock` | Serves the mock board on HTTP and on a pty console, by hand or for the app. |

## The staged bring-up (plan item 10)

1. **Laptop USB, 5 V.** The board is found on its CH340.
   - It is flashed if `--image` is given.
   - Its self-test runs. On 5 V the `pd_contract` item is expected to fail and is marked so.
   - The bench asks for the amp and **must be refused**. On Lite, PVDD_EN
     (GPIO18) is driven by firmware only and isn't ANDed with the CH224Q's
     power-good, so this is the one check that a 5 V port never powers the amp.
   - `--wifi` sets the network over the console.
2. **Each charger** (`--chargers 65w,100w`). The one USB-C port now carries
   power, not the console, so the board is found by mDNS. Its self-test
   runs. On every charger but the last, a governed 4 Ω sweep shows the
   governor holding the charger's budget.
3. **The full sweep** on the last charger: every load, with the governor off
   (the amp's own limit, judged against the prediction) and on (where the
   budget stops it).
4. **The 1/8-power soak** into the first load. The bench waits for the chip
   to cool from the sweep, then holds the tone for `--soak-s` (default 600 s)
   and logs the temperature. product.md asks for this measurement because
   it was never made on the amp board that was built.
5. **By ear and hand:** Spotify, AirPlay, BT, Lyrion, RCA in, DJ bypass, the
   sub outputs, the buttons and the IR remote. Each is answered y/n/skip and
   recorded.

A failed self-test stops the run before any power goes into a load.

Results go to `bench/results/<board>-<mac>-<time>/`:
- `report.md`: the results page, with verdicts first
- `session.json`: everything
- `sweep.csv` and `soak.csv`
- `thd.svg`: THD+N against power, log-log, one line per load (dashed with the governor on)

Mock runs are prefixed `mock-` and ignored by git.

## Wiring

```
 USB-C charger ── board ── L+ ──┬── dummy load ──┬── L−
                                │                │
                               10k              10k        (divider per leg)
                                ├── in 1         ├── in 2   stereo USB audio interface
                               1k               1k
                                │                │
 laptop ── USB ── interface    GND ─────────────GND ── board GND (the RCA shell)
```

- The amp is bridge-tied. Neither speaker terminal is ground, and each
  leg sits at PVDD/2. Never clip a grounded probe across a load.
- Each leg goes through a divider to its own input. The bench records both
  and takes leg+ − leg−. With 10k/1k, the 28 V peak of Plus at full swing
  is under 2.6 V per leg, inside a line input. The interface's input
  capacitors block the PVDD/2 DC.
- The interface's anti-alias filter removes the class-D carrier. Pick one
  with an input that doesn't clip at 2.6 V peak, or use a larger divider.
- `calibrate` sets the whole chain's scale from one true-RMS meter reading.
  No divider arithmetic is needed.
- **Dummy loads:** 4, 6 and 8 Ω, two of each (both channels are driven, except when the charger cannot feed two at the predicted power: then only L, as on Plus into 4 Ω).
  Each must be rated over the 1% power: 50 W for Lite, 100 W for Plus.
  Between steps the tone stops for `rest_s` so they cool.
- **Temperature:** Lite has no sensor on the board (every ADC1 pin is in
  use), so a thermocouple goes on the top of the TPA3118's package.
  `--temp-cmd` runs a command that prints °C. `--temp-manual` asks for
  each reading. The device's `temp_c` is used where a board has one.

Capture: `arecord:hw:1,0` (Linux ALSA), `sounddevice[:NAME]`
(`pip install sounddevice`, any OS), or `wav:FILE` to analyse a recording.

## How it measures

- **Frequency, level, THD, THD+N:** `hifibench/dsp.py`. The board's clock
  and the interface's differ by tens of ppm, so the tone's frequency is
  measured: zero crossings, then a four-parameter sine fit (IEEE 1057) on
  whole cycles. THD is harmonics 2–10. THD+N is everything left after the
  fundamental and DC, over the fundamental. Both are tested to 0.02% on
  synthetic tones.
- **Power** is the fundamental's RMS volts squared over the load, per
  channel, with both channels driven.
- **P at 1% / 10%** is interpolated where THD+N crosses, in log THD+N.
- **Predicted** is product.md's table. **Model at held PVDD** is the same
  fit, (PVDD·R/(R+Rs))²/2R with Rs 0.3 Ω (TPA3118D2) or 0.51 Ω (TPA3220, from the TPA3221 datasheet), at
  the lowest PVDD the board reported. A sagging charger then reads as
  supply, not amp. The verdict passes at ≥ 85% of the predicted 1% power,
  governor off.
- **Soak verdict:** it passes when the chip stays under 100 °C (`temp_limit_c`)
  for the whole soak with FAULTZ high.

## The test API it needs

The firmware worker owns the API and documents it in
[test-api.md](test-api.md); the bench follows that page. The bench was
written against this proposal, which the mock implements:

- **Discovery:** mDNS `_hifi._tcp`, TXT `board`, `fw`, `api`.
- **Endpoints:** `GET /api/v1/info` and `state`. `POST amp`, `volume`,
  `governor`, `tone`, `tone/stop`, `sweep`, `selftest` and `wifi`.
- **Console:** the same ops over the CH340 at 115200, as a line `api <op> [json]`
  answered by `API {json}`. Log lines in between are skipped.
- **The amp refuses (409) on a 5 V source.**
- **`tone`** is a pure sine on both channels at the given dBFS, with no EQ in the path.

When test-api.md differs, change `hifibench/api.py` and the mock together;
the tests cover both.

## The mock

`hifibench/mock.py` models a Lite from product.md's numbers:
- **PD:** CH224Q contracts, and a laptop port is 5 V only.
- **Amp:** bridge-tied clipping at PVDD·R/(R+Rs). PVDD sags through 0.15 Ω
  at 88% efficiency.
- **Charger:** cuts out above its watts, and the amp shows FAULTZ low.
- **Governor:** holds input at 90% of the contract.
- **Thermal:** a first-order model at 22 °C/W with a 40 s time constant, on
  a virtual clock, so a ten-minute soak runs in seconds.
- **Faults:** `/mock/fault` injects `codec`, `amp_fault` and `amp_on_5v`.

It is a check of the bench's logic, not a prediction of the board.

## Plus and Ultra

`--board core` uses the TPA3220 predictions (77/55/43 W at 28 V).
`--chargers 100w,140w` gives the 20 V fallback and the 28 V EPR contract.
The mock can model `core` too, with `Sim("core", charger="140w")`. Ultra's
four channels need a second capture pair or a second pass with the leads
moved. This is not built yet; it belongs with plan item 23.
