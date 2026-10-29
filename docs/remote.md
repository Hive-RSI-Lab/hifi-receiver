# Hands-off bring-up: the board measures itself, the Hive reaches it

The person's flow (2026-10-28, 21:47 and 22:05): plug it into power, run one
script on the laptop that sets up Wi-Fi, leave it plugged in at home, and the
Hive brings it up, updates its firmware and tests it on its own. A scope is
for later; whatever the board can measure about itself at little cost, it
should.

This page is the design. It says what was chosen, why, what it costs per
tier, and what is still needed from the person or a warden. The code is in
`firmware/components/hifi` (board side) and `bench/` (laptop and Hive side).

## The flow

1. **Plug in.** The USB-C charger powers the box. For setup, a USB-C cable
   from the laptop instead (5 V is enough for everything but the amp).
2. **Run one command** on the laptop, with the box on its USB-C port:

   ```sh
   python3 bench/provision.py
   ```

   It finds the board's CH340 serial port, flashes the current firmware if
   the board is blank or older (JLC ships the ESP32 empty), asks for the
   Wi-Fi name and password, sends them over the serial console, and waits
   until the board says it is on Wi-Fi and connected to the relay. It ends
   by printing the board's id and "leave it plugged in".
3. **Move it to its charger**, speakers on or off, and leave it.
4. **The Hive does the rest** over the relay: reads its state, runs its
   self-test, updates its firmware (with rollback), plays test tones and
   captures what the amplifier puts out, and writes a results page
   (`python3 bench/bench.py remote bringup`).

## The remote channel: the board dials out

**Decision:** the board connects outward to an MQTT broker over TLS, and the
Hive connects outward to the same broker. Neither side opens a port.

**Why not the Hive reaching it on the LAN** (an egress grant to the board's
IP): that only works while the Hive's machine is on the same home network as
the box, with a fixed IP for the box (mDNS doesn't pass the sandbox's proxy).
It breaks the day either moves. The board's HTTP API stays, and the bench
still uses it on a LAN (`--url`), but it isn't the channel the Hive relies on.

**Why MQTT:** ESP-IDF has a maintained MQTT client (`esp-mqtt`) with TLS, so
the board side is small; brokers are cheap or free; the broker keeps the
board's last status (a retained message) and announces when it drops off
(the last-will message), so the Hive sees "online/offline" without polling.
The Hive's side is a small client in plain Python (`bench/hifibench/mqtt.py`),
which tunnels through the sandbox's HTTPS proxy with `CONNECT`.

**What the broker is:** any MQTT 3.1.1 broker with TLS and username/password
logins. The recommended one is a free hosted cluster (HiveMQ Cloud
Serverless or EMQX Serverless: free up to 100 connections and several GB a
month, which is far more than a few boxes use). It needs an account, which
is the person's to make (see "Still needed"). A self-hosted Mosquitto works
the same, but the Hive's machine would have to accept inbound connections.

### Security

The broker's login keeps strangers off the topics. Commands are also
**signed**, so a leaked broker password still can't drive the amplifier or
flash firmware:

- The Hive holds an **Ed25519 signing key**. Its public half is compiled into
  the firmware (`firmware/components/hifi/core/hifi_keys.h`, up to two keys
  so a key can be rotated). There is no secret to carry to the box at setup.
- Every command is signed; the board drops any whose signature doesn't
  check, whose `dev` isn't its own id, or whose `id` isn't larger than the
  last one it took (replays). Once its clock is set (SNTP), it also drops
  commands whose `ts` is more than 10 minutes off.
- Replies and status aren't signed. The broker's login protects them.

### Topics and messages

`<prefix>` is `hifi` unless set; `<dev>` is the board's Wi-Fi MAC in
lower-case hex without colons (`240ac4123456`).

| Topic | Who publishes | What |
|---|---|---|
| `<prefix>/<dev>/status` | board, retained, QoS 1 | `{"online": true, "model", "board", "fw", "ip", "rssi", "uptime_s", "ota": {"slot", "state"}}`; the last will is `{"online": false}` |
| `<prefix>/<dev>/cmd` | Hive, QoS 1 | a signed command |
| `<prefix>/<dev>/reply` | board, QoS 1 | `{"id": N, "status": 200, "body": {...}}` |
| `<prefix>/<dev>/event` | board, QoS 1 | `{"event": "boot" \| "ota_valid" \| "rolled_back" \| "ota_failed", ...}` |

**A command** is two lines: the JSON envelope, then the base64 Ed25519
signature of exactly the first line's bytes.

```
{"v":1,"dev":"240ac4123456","id":1761700000123,"ts":1761700000,"method":"POST","path":"/api/v1/tone","body":{"type":"sine","freq_hz":1000,"level_dbfs":-30}}
hW3x...base64 of 64 bytes...==
```

- `id`: an integer that only goes up. The Hive uses milliseconds since 1970,
  so it keeps going up across Hive runs.
- `method`, `path`, `body`: any call of the [test API](test-api.md),
  exactly as over HTTP. The board answers on `reply` with the same `id`, the
  HTTP status and the JSON body.
- A command that fails its checks is answered `401` (bad signature or wrong
  `dev`) or `409` (`id` not new, or `ts` too far off) with no effect.

### When the box shares the Hive's network

The person's tower, where the Hive runs, is on their home LAN
(192.168.1.0/24). Whether the boxes will be on the same network isn't known,
so both paths work and the box doesn't need to know which one the Hive uses:

- **Through the broker** (any network): as above. The box's retained status
  carries its LAN `ip`.
- **Direct on the LAN** (same network): every `remote` command takes
  `--url http://<ip>` in place of the relay:
  `python3 bench/bench.py --url http://192.168.1.40 remote bringup`. Same
  calls, same results page, and `ota` waits on `GET /api/v1/ota` instead of
  the relay's events. The Hive's sandbox can't see mDNS, so it uses the IP
  from the box's relay status or from `provision.py`'s last line; a DHCP
  reservation on the router keeps it fixed. It needs an egress grant to
  `<ip>:80`, which isn't possible yet (grants are loopback-only; hive task
  035439-806d2d41de adds host:port grants).
- **Which to use:** the broker when it's reachable; the LAN when the broker
  is down or not set up yet. The LAN path isn't signed (as in squeezelite-esp32; [firmware.md](firmware.md)), so it is
  only for a box on the person's own network.

## Firmware updates the Hive pushes, with rollback

**Partition table:** two app slots plus the factory recovery app, so a new
image is written beside the running one and the old one stays bootable:

| Partition | Size | Holds |
|---|---|---|
| `recovery` (factory) | 1.75 MB | squeezelite-esp32's recovery app (1.52 MB built) |
| `ota_0`, `ota_1` | 3.06 MB each | the main app (2.77 MB built, with the relay) |

The old table had one 4 MB app slot, so an update could only be written by
rebooting into recovery, and a bad image had nothing to fall back to but
recovery. No board has been flashed yet, so changing the table now costs
nothing; once boards are in the field it can only change over USB.

**The update** goes over the same channel, so nothing has to host the image:

1. `POST /api/v1/ota/begin {"size", "sha256", "version"}`: the board opens
   the slot it isn't running from.
2. `POST /api/v1/ota/chunk {"off", "data"}` (base64, up to 4 KB) in order;
   a chunk at the wrong offset is refused with the offset it expects, so the
   sender resumes there.
3. `POST /api/v1/ota/finish`: the board checks the SHA-256 of what it wrote
   against `begin`'s, sets the new slot to boot and restarts.

Each of these is a signed command like any other, so only the Hive can
update the board. The same calls work over HTTP on a LAN and over the
console.

**Rollback** (ESP-IDF's app rollback, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`):

- A new image boots **on trial**. It must, within 120 s: start our
  component, join Wi-Fi, reach the relay (when one is set up), and pass the
  self-test items that don't depend on the charger (codec and PD sink on
  I2C, enough heap).
- Then it marks itself good and publishes `ota_valid`.
- If a check fails or time runs out, it marks itself bad and restarts into
  the previous image, which publishes `rolled_back` with the reason.
- If it crashes or hangs (the watchdog) before marking itself good, the
  bootloader rolls back on the next start by itself.
- The factory recovery app stays as the last resort, and USB flashing always
  works.

## The board measures its own output

**Decision:** tap each speaker output through a resistor divider into the
codec's spare ADC input, so the board can record what its amplifier actually
puts out. With test tones from the board itself, that measures gain, level,
frequency response, THD+N up to clipping, noise and DC offset on every
channel, with the speakers on or off, with no bench gear.

**The circuit** (per channel, on the **+** leg of the bridge-tied output,
after the output filter):

```
 SPK_x_P ──[100k]──┬──[1µF]── ADC input (LIN2 / RIN2 on Lite)
                   ├──[10k]── AGND
                   └──[470p]─ AGND        (Lite; core uses 150k for 28 V)
```

- **Why one leg, not the differential:** the codec has two spare inputs, and
  one leg of each channel captures both channels at once. Each leg carries
  half the output plus PVDD/2 of DC, which the capacitor blocks, so level,
  gain, clipping, frequency response and noise all read directly; THD reads
  the leg's own distortion, which is where a class-D stage's comes from. A
  true differential reading would need an op-amp or a second capture pass.
- **Divider:** 1/11 on Lite (20 V PVDD: a leg swings ±10 V, so ±0.91 V at the
  ADC) and 1/16 on core (28 V: ±0.88 V). The 470 pF with the divider's 9 kΩ
  is a 37 kHz low-pass; the output's LC filter has already taken out the
  switching carrier, and the codec's own decimation filter takes the rest.
- **Line in stays usable:** the ES8388 selects LIN1/RIN1 (the RCA input) or
  LIN2/RIN2 (the taps) for its ADC; the analog DJ-direct path doesn't go
  through that switch, so the board can watch its outputs while it plays
  the DJ deck.

**The firmware call:** `POST /api/v1/capture {"source": "loopback", "frames":
4096}` records from the ADC and returns 16-bit samples (base64), their
scale in volts at the speaker leg, and per-channel RMS, peak, DC and clipped
counts. `"source": "line"` records the RCA input instead, which tests that
input with any source plugged in. The bench analyses the samples with the
same code it uses for a USB audio interface (`hifibench/dsp.py`).

### Added cost per tier

JLC basic parts (0402 resistors and capacitors) carry no extended-part fee;
an SMT joint is about $0.0017.

| Tier | What is added | Parts | Assembly | Per unit at 100 |
|---|---|---|---|---|
| Lite (`lite-main`) | 2 × (100k, 10k, 470 pF, 1 µF) into the ES8388's unused LIN2/RIN2 | $0.01 | 16 joints, $0.03 | **≈ $0.04** |
| Plus (`core`) | the same 8 passives, plus a second TS5A23157 (already on the board's BOM, so no new extended-part fee) to switch the ES7243E's inputs between the RCA jacks and the taps; its select needs a control line (the TCA9555's 16 pins are all used, so a free ESP32 pin or the ES9018K2M's GPIO) | $0.30 | 26 joints, $0.04 | **≈ $0.34** |
| Ultra (`core` + `ultra-front`) | Plus's, and 8 more passives and one more TS5A23157 for zone B's taps | $0.61 | 52 joints, $0.09 | **≈ $0.70** |

Plus and Ultra cost more because their ADC (ES7243E) has one stereo input,
which the RCA jacks use, so the taps need a switch. Both are well under 2%
of each tier's unit cost.

**What the taps can't measure:** output current, so not power into a real
load, and not the amplifier's temperature (Lite has no sensor; product.md
asks for the chip temperature at 1/8 power). Those still need the bench's
dummy loads and a thermocouple once (`docs/bench.md`). Power into the
speakers can be read off the leg voltage only if their impedance is known.

### What else the board reports about itself

Already in the firmware: the PD contract, VBUS, the power sequencer's
state, amp fault/clip/latched, the governor's input estimate (measured by
the INA186 on `core`), Wi-Fi signal, heap and uptime. The relay status
message carries the summary, so the Hive sees a box drop off, reboot or
roll back without asking.

## The Hive's side

- `python3 bench/bench.py remote list`: boards the broker has a status for.
- `python3 bench/bench.py remote --dev ID info|state|selftest`: the same calls
  as over USB or the LAN.
- `python3 bench/bench.py remote --dev ID ota build/squeezelite.bin`: pushes
  an image and waits for `ota_valid` or `rolled_back`.
- `python3 bench/bench.py remote --dev ID bringup`: the unattended run:
  self-test, power and network state, the loopback checks (gain per channel
  against the expected, a level step to the clipping point or `--max-dbfs`,
  a coarse frequency response from stepped sines, the noise floor), and a results page in
  `bench/results/`.

**Quiet by default:** the box may sit at home with the speakers connected.
Unattended tones stay at or below −30 dBFS (about 40 mW into 4 Ω: audible
but not loud) unless the run is given `--loud`, which is for a box on dummy
loads or with the speakers unplugged.

The relay settings come from `HIFI_RELAY` (a JSON file path) or
`~/.config/hifi-receiver/relay.json`:

```json
{"url": "mqtts://xxxx.s1.eu.hivemq.cloud:8883", "user": "hive", "password": "...",
 "prefix": "hifi", "key": "~/.config/hifi-receiver/hive-signing.key"}
```

The board's own login (`board` user) is set by `provision.py` from the same
file's `board_user`/`board_password`, or compiled into the image.

## Still needed

- **From the person:** a broker account with two logins, `hive` and `board`.
  In hand as of 2026-10-29: the person is making a HiveMQ Cloud account, and
  autobox writes it to `~/.config/hifi-receiver/relay.json` (mode 600) on
  the person's machine.
- **In the Hive** (filed by the lead, first in line on project hive):
  project runs reading that file (task 035439-a352565016, a secrets mount)
  and egress grants to a host and port, not only loopback (task
  035439-806d2d41de), for the broker's port 8883 and a LAN box's port 80.
  Until both land, everything runs against the bench's own broker and mock
  board (`bench.py mock --relay`, and `mock.serve` for the LAN path in the
  tests).
- **The signing key:** made with `bench.py remote keygen` into the same
  secrets place once it exists; its public half goes into firmware builds
  as `HIFI_HIVE_PUBKEY`.
- **Board change:** the loopback taps on `lite-main` and `core`, in the
  review and rebuild task 261029-020622. Its worker sends the core's switch
  select pin and any divider change for `firmware/components/hifi/esp/hifi_board.h`.
- **Not yet checked on hardware:** all of it. The logic is tested on a PC
  (firmware host tests, the bench's tests with a broker and mock board) and
  the firmware builds with ESP-IDF v4.4.8; the ES8388 ADC routing, I2S
  receive, the rollback and TLS to the broker need the first board.
