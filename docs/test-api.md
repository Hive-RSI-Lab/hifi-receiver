# The receiver's test API

Every tier's firmware serves this API on port 80, beside squeezelite-esp32's
own pages. The Hive's bench scripts (`bench/`), the web app and a person with
`curl` all use it to see what the box is doing and to make it do one thing at
a time. It is the same on Lite, Plus and Ultra; fields a board can't measure
are `null`, never made up.

- **Finding the box:** it advertises `_hifi._tcp` over mDNS, with TXT records
  `model=hifi-lite`, `board=lite-main`, `fw=<version>` and `api=1`. Its
  hostname is `hifi-lite` (Plus and Ultra: `hifi-plus`, `hifi-ultra`), so
  `http://hifi-lite.local` opens it; if two boxes share a network, ESP-IDF's
  mDNS answers the second as `hifi-lite-2.local`.
- **What else is on port 80:** `/` is the web app (`app/`). squeezelite-esp32's
  own settings page moved to `/squeezelite/`; its JSON endpoints stay where
  they were (below).
  Over USB, the serial console has the same calls (below).
- **Format:** JSON in and out, `Content-Type: application/json`. Every reply is
  an object. An error is `{"error": "<what went wrong>"}` with status 400
  (bad request), 404 (no such path), 409 (not now, e.g. a tone while the amp
  is faulted) or 500.
- **Version:** everything is under `/api/v1/`. A change that breaks a caller
  gets `/api/v2/`.
- **Safety:** nothing here can push the amp past what the power governor
  allows, and tones are capped at −6 dBFS unless `"force": true`.

The code is `firmware/components/hifi/core/hifi_api.c`; its host tests in
`firmware/test/` pin down every reply below.

## `GET /api/v1/info`

What the box is.

```json
{"model": "hifi-lite", "board": "lite-main", "fw": "0.1.0",
 "upstream": "squeezelite-esp32 master-v4.3 ee1720c", "api": 1,
 "name": "Living room", "mac": "24:0a:c4:12:34:56", "hostname": "hifi-lite",
 "flash_mb": 8, "psram_mb": 8,
 "caps": {"line_modes": ["dj_direct"],
          "eq_bands": [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000],
          "eq_min": -12, "eq_max": 12,
          "sub": {"count": 2, "crossover": false, "per_sub": false},
          "zones": 1, "eth": false, "pdo_v_max": 20}}
```

`name` is the player name people see in AirPlay, Bluetooth, Spotify and
LMS. `caps` says what this tier can do, so one web app serves all three:
Plus and Ultra (`"board": "core"`) set `eth` and a `pdo_v_max` of 28, and
Ultra `zones` 2. Their sub outputs have no crossover or per-sub settings
yet (`crossover` and `per_sub` stay false): the PCM5102A gets the sum of
the mains at the sub level.

## `GET /api/v1/state`

Everything the bench logs, in one read. Poll it at up to 10 Hz.

```json
{"uptime_s": 812,
 "power": {"state": "on", "vbus_v": 20.12, "pg": true,
           "contract": {"protocol": "pd", "epr": false, "v": 20.0, "i_max_a": 3.25, "w": 65.0},
           "pvdd_on": true, "pvdd_v": 19.95, "vbus_i_a": null},
 "amp": {"enabled": true, "fault": false, "faults": 0, "latched": false, "temp_c": null},
 "governor": {"budget_w": 58.5, "in_w": 7.9, "out_w": 4.4,
              "gain_db": 0.0, "limiting": false, "limit_events": 0},
 "audio": {"source": "spotify", "input": "stream", "tone": null, "level_dbfs": -23.4,
           "sub": {"on": true, "level_db": 0.0},
           "eq": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
           "volume": 35, "mute": false, "max_volume": 100,
           "playing": {"state": "play", "title": "Teardrop", "artist": "Massive Attack",
                       "album": "Mezzanine", "elapsed_s": 61, "duration_s": 330}},
 "net": {"mode": "sta", "ssid": "home", "hostname": "hifi-lite",
         "ip": "192.168.1.40", "rssi": -61},
 "sys": {"heap_free": 81234, "psram_free": 3912345},
 "ir": {"learning": null, "learned": ["volup", "voldown", "mute"], "last_code": "0x00ff40bf"}}
```

- **`power.state`** is the power sequencer's state:
  - `wait_pd`: no contract good enough for the amp yet (5 V port, or
    negotiating). The box runs, the amp stays unpowered.
  - `soft_start`: PVDD switch closing, amp held off.
  - `on`: PVDD up; the amp may be enabled.
  - `undervolt` / `overvolt`: VBUS outside 13–25 V (Lite) while it was on;
    everything off until VBUS is back in 14–25 V for 1 s.
  - `no_pvdd`: the PVDD switch was commanded on but PVDD didn't come up
    to 85 % of VBUS within 1 s, or fell under it for 50 ms while on (Lite
    reads PVDD on GPIO39). Everything off; it retries after 1 s, and three
    failures inside a minute hold it off until `POST /api/v1/amp
    {"reset": true}` or a re-plug.
- **`contract`** comes from the PD sink (CH224's status and current
  registers) and the VBUS reading. `protocol` is `pd`, `qc3`, `qc2`, `bc` or
  `none`; `v` is VBUS rounded to the nearest fixed PD level; `i_max_a` is
  `null` unless the protocol is PD.
- **`pvdd_v`:** PVDD after the switch, from GPIO39 (PVDD/11) on Lite;
  `null` on Plus/Ultra (the core board has no PVDD sense).
- **`vbus_i_a`, `temp_c`:** `null` on Lite (no sensor). Plus/Ultra
  read the input current from the INA186 (`vbus_i_a`; 0 below about
  0.4 A, where the ESP32's ADC reads nothing); `temp_c` stays `null`
  there too.
- **`vbus_v` on Plus/Ultra** is taken from the contract, not measured (the
  core board has no VBUS divider): 28 V in an EPR contract, 20 V in
  another PD one (once the handshake is in, the firmware asks the CH224Q
  for 28 V if the charger offers EPR, else 20 V), 5 V otherwise. Their
  amps start at 14 V and run 13–30 V.
- **`amp.fault`** is the amp's FAULTZ pin now; `faults` counts them since
  boot. Three faults inside a minute **latch** the amp off until
  `POST /api/v1/amp {"reset": true}` or a re-plug.
- **`governor`:** `budget_w` is 90% of the contract's power. `in_w` is the
  estimated (Lite) or measured (Plus/Ultra) input power, averaged over 1 s;
  `out_w` the estimated speaker power. `gain_db` ≤ 0 is what the limiter is
  taking off now. `limit_events` counts the times it started limiting.
- **`audio.source`:** `idle`, `lms`, `airplay`, `spotify`, `bt` or `test`.
  `input` is `stream` or `dj_direct` (Lite: the ES8388's analog bypass from
  the RCA line in). `tone` is the running tone (below) or `null`.
  `level_dbfs` is the 1 s RMS of what goes to the DAC, relative to a full-
  scale sine. `sub` and `eq` are as set below.
- **`audio.volume`** (0–100), **`mute`** and **`max_volume`** (30–100): the
  receiver's one volume (`POST /api/v1/volume` below). It follows whatever
  source plays: when a phone or LMS moves its own slider, `volume` moves
  with it.
- **`audio.playing`** is `null` when no source holds the player, else
  `state` (`play`, `pause` or `stop`), `title`, `artist`, `album`,
  `elapsed_s` and `duration_s`. Anything not known is `null`. AirPlay,
  Bluetooth and Spotify send all of it. A phone over Bluetooth sends text
  only if it supports AVRCP metadata. LMS sends its track text only as
  display graphics, so for LMS this is `{"state": "play"}` with the rest
  `null`. `elapsed_s` counts on while playing, from the source's last report.
- **`ir`:** `learning` is the action waiting for a remote button
  (`POST /api/v1/ir/learn`), else `null`. `learned` lists the actions that
  have a button. `last_code` is the last code the IR receiver decoded since
  boot, learned or not: the bench's check that the receiver works.
- **`net.mode`** is `ap` while the box runs its own setup access point (no
  Wi-Fi joined yet), else `sta`. Plus and Ultra add **`net.eth`**: true when
  the Ethernet port has an address (then `ip` is that address if Wi-Fi has
  none).
- **`audio.zone_b`** (Ultra only): `{"on": true, "volume": null}`. Zone B's
  amp plays zone A's program at the main volume; it has no level of its own.
- **`update`** appears while a firmware update the app started runs or
  after it ends: `{"state": "downloading", "progress": 42, "error": null}`,
  `state` being `downloading`, `flashing`, `done` (the box restarts into
  it) or `error` (with the reason; the running firmware stays).

## `POST /api/v1/tone`

Plays a test signal, taking over from any stream until it ends or is stopped.
Sample rate 48 kHz. The power governor still applies.

| Body | Plays |
|---|---|
| `{"type": "sine", "freq_hz": 1000, "level_dbfs": -20, "channel": "both", "duration_s": 5}` | a sine; `channel` is `both`, `left` or `right` |
| `{"type": "sweep", "start_hz": 20, "end_hz": 20000, "level_dbfs": -20, "duration_s": 10}` | a log sweep (Farina), both channels |
| `{"type": "noise", "level_dbfs": -20, "duration_s": 5}` | pink noise (−3 dB/octave), RMS at `level_dbfs` |
| `{"type": "silence", "duration_s": 5}` | digital silence, amp on: the noise-floor measurement |
| `{"type": "stop"}` | stops whatever plays |

- `duration_s`: 0.1–600, default 5.
- `level_dbfs`: −90 to −6, or up to 0 with `"force": true`.
- `freq_hz`: 10–24000.

Reply: `{"ok": true, "tone": {...the tone as it will play...}}`. Status 409
if the amp is latched off or the power state isn't `on` (a tone with the amp
unpowered measures nothing), unless `"force": true`.

## `POST /api/v1/amp`

| Body | Does |
|---|---|
| `{"enable": false}` | holds the amp in shutdown (SDZ low), PVDD stays up |
| `{"enable": true}` | lets it run again (the default) |
| `{"reset": true}` | clears a latched fault and the fault count |

Reply: the `amp` object from `/state`.

## `POST /api/v1/input`

`{"input": "dj_direct"}` routes the RCA line in straight to the amp and the
sub outs through the codec's analog bypass: zero latency for the DJ deck.
On Plus/Ultra it is the TS5A23157 switch in front of the amps: the line in
reaches the speakers with nothing digital in the way, so the receiver's
volume and governor don't act on it (the deck's own level does), and the
subs get nothing.
`{"input": "stream"}` goes back to the streamed audio. Reply:
`{"ok": true, "input": "..."}`.

## `POST /api/v1/sub`

`{"level_db": -6}` sets the sub outputs' level, −30 to +6 dB; `{"on": false}`
turns them off (either or both). Lite: the ES8388's LOUT2/ROUT2, 1.5 dB steps
up to +4.5 dB, so the level it reports is the one it could set. Plus/Ultra:
a digital gain on the sub DAC's stream, −40 to 0 dB in 0.5 dB steps (more
would clip). Kept across restarts. Reply: the full `/state`.

## `POST /api/v1/eq`

`{"gains": [0, 0, 2, 0, 0, 0, 0, 0, -1, 0]}`: squeezelite-esp32's 10-band
equalizer (bands in `info.caps.eq_bands`), whole dB, `eq_min` to `eq_max`.
Applied at once and kept. Reply: the full `/state`.

## `POST /api/v1/name`

`{"name": "Kitchen"}`, 1–32 characters: the player name in AirPlay,
Bluetooth, Spotify and LMS. The services take it at the next restart. The
hostname stays `hifi-lite`. Reply: the full `/state`.

## `POST /api/v1/reboot`, `POST /api/v1/reset`

`reboot` restarts the box 0.7 s after replying. `reset` needs
`{"factory": true}`: it erases every setting (Wi-Fi, names, EQ, sub) and
restarts. Both reply with the full `/state` first.

## `POST /api/v1/volume`

`{"volume": 40}`, `{"mute": true}`, `{"max_volume": 70}`, or any of them
together. Each is optional, and a `null` leaves that one as it is.
- **`volume`:** a whole number, 0–100. It is held to `max_volume`, so 80
  with a maximum of 70 sets 70.
- **`max_volume`:** a whole number, 30–100. It is applied first, and a
  volume above it comes down to it. Raising it doesn't turn the volume up.
- **`mute`:** `true` or `false`.

A value out of range or of the wrong type is a 400 and changes nothing.
The reply is the full `/state`. `max_volume` and the volume are kept across
restarts. Mute isn't: the box always starts unmuted.

How it works:
- The volume is squeezelite's output gain. The curve is
  `gain = (volume / 100)³`, which squeezelite already uses for AirPlay and
  Bluetooth, so their sliders read back as the same number.
  - Spotify sends a gain with its own curve (a 4th power). LMS sends its
    own as well. `volume` is that gain read back on our curve.
  - The source gets nothing back. After the app sets 40, the phone's slider
    stays where it was until the phone moves it, which then sets the volume
    again.
- **`max_volume`** caps every source, the buttons and the remote. A phone
  at full scale plays at the maximum.
- **`mute`** holds the gain at 0. A source's volume change while muted is
  remembered, not played. A volume step from the remote or the buttons
  unmutes.
- **In DJ direct** the line in skips squeezelite. The volume goes on the
  ES8388's speaker outputs instead (LOUT1/ROUT1, 1.5 dB steps below their
  −4.5 dB cap), down to −45 dB. Mute takes the line in out of the mix. The
  sub outputs keep their own level.
- **While a test tone plays**, it plays at the level asked for. Volume
  changes are kept and applied when it ends.

## `POST /api/v1/ir/learn`, `POST /api/v1/ir/forget`

`{"action": "mute"}` teaches the remote. The next IR code the receiver
decodes, within 30 s, becomes that action's button.
- **Actions:** `volup`, `voldown`, `mute`, `toggle` (play/pause), `next`,
  `prev`, `input` (stream ↔ DJ direct) and `power`.
- **One button per action:** a code already used for another action moves
  to this one.
- **What a learned button does:**
  - `mute` and `input` act on the receiver.
  - `volup` and `voldown` step the receiver's volume by 5 when nothing
    streams or the input is DJ direct. Otherwise they go to the source, like
    the board's buttons.
  - The rest go to whatever is playing.
  - Held, a volume button steps about 5 times a second. Any other held
    button fires once.
- **Codes nobody learned** go on to squeezelite as before: LMS's own remote
  map and squeezelite's default codes.
- **Storage:** codes are kept across restarts (NVS key `hifi_ir`).
- **Forgetting:** `ir/forget` with `{"action": "mute"}` forgets one button.
  With `{}` it forgets them all.

Both reply with the full `/state`; watch `ir.learning` go back to `null`
and the action appear in `ir.learned`.

The receiver decodes NEC by default (`set_GPIO` `14=ir:nec`), which most
cheap remotes and TVs' universal remotes send. An RC5 remote needs
`14=ir:rc5` in squeezelite's settings.

## `POST /api/v1/control`

`{"action": "volup"}`: what a button press does, sent to whatever is
playing (LMS, AirPlay, Spotify, Bluetooth). Actions: `volup`, `voldown`,
`toggle`, `play`, `pause`, `stop`, `next`, `prev`, `power`, plus the
receiver's own `mute` (toggles it) and `input` (stream ↔ DJ direct).
`volup`/`voldown` step the receiver's own volume when nothing streams or in
DJ direct, as the remote does. (There is no
Bluetooth pairing action: squeezelite-esp32's Bluetooth sink is discoverable
whenever no other phone is connected.)
Reply: `{"ok": true}`.

## `POST /api/v1/selftest`

Runs every check the board can do on itself in about 2 s and returns them.
Nothing in it needs a speaker or a load.

```json
{"pass": false,
 "checks": [
  {"name": "pd_sink_i2c",   "pass": true,  "value": "0x22 acks"},
  {"name": "pd_contract",   "pass": true,  "value": "pd 20.0 V 3.25 A"},
  {"name": "vbus_matches",  "pass": true,  "value": "20.12 V"},
  {"name": "codec_i2c",     "pass": true,  "value": "0x10 acks"},
  {"name": "pvdd_on",       "pass": true,  "value": "on"},
  {"name": "pvdd_sensed",   "pass": true,  "value": "PVDD 19.95 V of VBUS 20.12 V"},
  {"name": "amp_no_fault",  "pass": true,  "value": "FAULTZ high after 200 ms enabled"},
  {"name": "psram",         "pass": true,  "value": "8 MB"},
  {"name": "heap",          "pass": true,  "value": "81234 B free"},
  {"name": "network",       "pass": false, "value": "not connected"}]}
```

`pass` is true only if every check passes. A check that can't run (the amp
on a 5 V port) fails with the reason in `value`; on a 5 V port that is
expected, and the bench script knows it.

Plus and Ultra have no `vbus_matches` or `codec_i2c`; their board checks
are `expander_i2c` (the I/O expander at 0x20, or 0x74–0x77 for the
TCA9539), `dac_i2c` (the ES9018K2M at 0x48 answers and took the 16-bit word
length), `adc_i2c` (the ES7243E at 0x10) and `amp_otw` (OTW_CLIP high).

## Remote bring-up: `capture`, `ota`, `wifi`, `remote`

These serve the hands-off bring-up in [remote.md](remote.md). They work the
same over HTTP, the console and the relay; over the relay every call is a
signed command.

### `POST /api/v1/capture`

`{"source": "loopback", "frames": 4096}` records 64 to 16384 frames of the
codec's ADC at the output's rate: `loopback` is the speaker taps, `line` the
RCA input. The output must be running (a tone or a stream), else 409. 409
too on a board revision without taps.

```json
{"source": "loopback", "rate": 48000, "frames": 4096,
 "channels": ["spk_l+", "spk_r+"], "v_per_fs": [13.970, 13.970],
 "stats": [{"rms_fs": 0.0223, "peak_fs": 0.0316, "dc_fs": 0.0001, "clipped": 0}, {...}],
 "pcm": "<base64: 16-bit little-endian, interleaved L R>"}
```

`v_per_fs` is the peak volts at the tap's source (a speaker leg, or the RCA
jack) for an ADC full scale: the divider times the ADC's full scale, a
datasheet figure until bring-up calibrates it.

### `GET /api/v1/ota`, `POST /api/v1/ota/begin|chunk|finish|abort`

```json
{"slot": "ota_0", "fw": "0.1.0", "state": "valid",
 "update": {"active": true, "size": 2690000, "written": 409600, "version": "0.2.0", "slot": "ota_1"},
 "last_rollback": {"from": "0.2.0", "reason": "relay not reached after 120 s"}}
```

- `begin` `{"size", "sha256", "version"}` → `{"ok": true, "slot": "ota_1", "chunk_max": 4096}`.
- `chunk` `{"off", "data"}` (base64, at most 4096 bytes) → `{"ok": true, "next": N}`; a
  chunk at the wrong offset is 409 with the `next` offset expected.
- `finish` (`{"reboot": false}` to stay up) checks the SHA-256, sets the new
  slot to boot and restarts: `{"ok": true, "reboot": true, "slot": "ota_1"}`;
  400 `sha256 mismatch` drops the update.
- `state` is `pending_verify` while a new image is on trial (remote.md,
  "Rollback").

### `POST /api/v1/wifi`

`{"ssid": "home", "password": "..."}` joins a network and keeps it once
joined. Reply `{"ok": true}`; `/state`'s `net` shows how it went.

### `GET|POST /api/v1/remote`

The relay link. POST `{"url": "mqtts://host:8883", "user", "password",
"prefix": "hifi"}` sets it (an empty `url` turns it off); both reply

```json
{"configured": true, "connected": true, "url": "mqtts://host:8883", "user": "board",
 "prefix": "hifi", "dev": "240ac4123456", "keys": 1, "accepted": 12, "rejected": 0}
```

`keys` is how many Hive signing keys the build trusts; with 0 the board
reports but takes no command from the relay.

## `POST /api/v1/zone`

Ultra: `{"zone": "b", "on": true}` turns zone B's amp on or off (kept across
restarts). Both amps go into reset for 10 ms and come out together, since
zone B runs off zone A's clock. `volume` is refused (400): zone B plays at
the main volume. Plus and Lite answer 404. Reply `{"ok": true}`.

## Wi-Fi and updates from the app

### `GET /api/v1/wifi/scan`

Scans for about 4 s (through squeezelite's network manager) and lists the
2.4 GHz networks in range, hidden ones left out:

```json
{"networks": [{"ssid": "home", "rssi": -48, "secure": true}, {"ssid": "cafe", "rssi": -81, "secure": false}]}
```

409 while the radio is busy (another scan, a join).

### `POST /api/v1/wifi/forget`

Forgets the joined network; the box opens its own setup access point
again. Reply `{"ok": true}`.

### `GET /api/v1/update/check`, `POST /api/v1/update`

The check reads the update manifest at the URL in NVS `hifi_update_url`
(or the build's `HIFI_UPDATE_URL`), 404 when neither is set:

```json
{"latest": "0.2.0", "notes": "Zone B",
 "images": {"hifi-plus": {"url": "hifi-plus-0.2.0.bin", "sha256": "<64 hex>"},
            "hifi-ultra": {"url": "hifi-ultra-0.2.0.bin", "sha256": "<64 hex>"}}}
```

(one `url`/`sha256` at the top level serves every model; a relative `url`
is taken from the manifest's folder) and answers with this model's offer:

```json
{"current": "0.1.0", "latest": "0.2.0", "url": "http://hive.local/fw/hifi-plus-0.2.0.bin",
 "sha256": "<64 hex>", "notes": "Zone B"}
```

`POST /api/v1/update` `{"url": "...", "sha256": "..."}` downloads that
image into the other app slot in the background and restarts into it;
`/state`'s `update` shows how far it got. Without `sha256` it uses the
last check's for the same URL; with none at all only the image's own
appended hash, which ESP-IDF checks, guards it. 409 while an update runs.
The new image is on trial like the relay's (remote.md, "Rollback").

### `POST /api/v1/update/upload`

The image itself as the body (`Content-Type: application/octet-stream`),
the app's "From a file…". It is written as it arrives; the reply is the
full `/state` with `update.state` `done`, and the box restarts a moment
later. 409 while another update runs; 400 if the body stops short or isn't
an app image.

## squeezelite-esp32's own endpoints

squeezelite-esp32 also serves these; the calls above do the same for the
web app.

| Call | Does |
|---|---|
| `GET /scan.json`, then `GET /ap.json` | starts a Wi-Fi scan, then lists what it found |
| `POST /connect.json` `{"ssid": "...", "pwd": "..."}` | joins a network; `GET /status.json` shows how it went |
| `DELETE /connect.json` | forgets the network |
| `POST /config.json` (key `fwurl`), then `POST /reboot_ota.json` | updates the firmware from a URL |
| `POST /recovery.json`, then `POST /flash.json` (the `.bin`) in recovery | updates from a file |

Their exact bodies are what squeezelite-esp32's own page sends
(`components/wifi-manager/webapp` at the pinned commit); this API doesn't
restate them.

## Serial console

The same calls over the USB serial console (115200 8N1, CH340C), for a
board that isn't on a network yet. Each prints the JSON reply on one line.

```
hifi info
hifi state
hifi tone sine 1000 -20 [both|left|right] [seconds]
hifi tone sweep 20 20000 -20 [seconds]
hifi tone noise -20 [seconds]
hifi tone silence [seconds]
hifi tone stop
hifi amp on|off|reset
hifi input stream|dj_direct
hifi sub <dB>
hifi control <action>
hifi volume <0-100>
hifi mute on|off
hifi maxvol <30-100>
hifi ir learn <action>
hifi ir forget [action]
hifi selftest
```

And any call in one line, as the bench and `provision.py` use it: `api <op>`
is a GET, `api <op> "<json>"` a POST, with the JSON in double quotes and its
`"` and `\` backslash-escaped (the console splits on spaces). The reply is
one line `API {json}`, with `"status"` added on an error:

```
api state
api wifi "{\"ssid\": \"home\", \"password\": \"secret\"}"
API {"ok":true}
```

squeezelite-esp32's own console commands (`help`, `nvsset`, `join`, …) are
there as well; see its README.
