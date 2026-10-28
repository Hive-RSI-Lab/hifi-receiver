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
Plus and Ultra set `crossover`, `per_sub`, `eth` and a `pdo_v_max` of 28.

## `GET /api/v1/state`

Everything the bench logs, in one read. Poll it at up to 10 Hz.

```json
{"uptime_s": 812,
 "power": {"state": "on", "vbus_v": 20.12, "pg": true,
           "contract": {"protocol": "pd", "epr": false, "v": 20.0, "i_max_a": 3.25, "w": 65.0},
           "pvdd_on": true, "pvdd_v": null, "vbus_i_a": null},
 "amp": {"enabled": true, "fault": false, "faults": 0, "latched": false, "temp_c": null},
 "governor": {"budget_w": 58.5, "in_w": 7.9, "out_w": 4.4,
              "gain_db": 0.0, "limiting": false, "limit_events": 0},
 "audio": {"source": "lms", "input": "stream", "tone": null, "level_dbfs": -23.4,
           "sub": {"on": true, "level_db": 0.0},
           "eq": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]},
 "net": {"mode": "sta", "ssid": "home", "hostname": "hifi-lite",
         "ip": "192.168.1.40", "rssi": -61},
 "sys": {"heap_free": 81234, "psram_free": 3912345}}
```

- **`power.state`** is the power sequencer's state:
  - `wait_pd`: no contract good enough for the amp yet (5 V port, or
    negotiating). The box runs, the amp stays unpowered.
  - `soft_start`: PVDD switch closing, amp held off.
  - `on`: PVDD up; the amp may be enabled.
  - `undervolt` / `overvolt`: VBUS outside 9–25 V (Lite) while it was on;
    everything off until VBUS is back in range for 1 s.
- **`contract`** comes from the PD sink (CH224's status and current
  registers) and the VBUS reading. `protocol` is `pd`, `qc3`, `qc2`, `bc` or
  `none`; `v` is VBUS rounded to the nearest fixed PD level; `i_max_a` is
  `null` unless the protocol is PD.
- **`pvdd_v`, `vbus_i_a`, `temp_c`:** `null` on Lite (no sensor). Plus/Ultra
  read VBUS current from the INA180.
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
- **`net.mode`** is `ap` while the box runs its own setup access point (no
  Wi-Fi joined yet), else `sta`.

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
`{"input": "stream"}` goes back to the streamed audio. Reply:
`{"ok": true, "input": "..."}`.

## `POST /api/v1/sub`

`{"level_db": -6}` sets the sub outputs' level, −30 to +6 dB; `{"on": false}`
turns them off (either or both). Lite: the ES8388's LOUT2/ROUT2, 1.5 dB steps
up to +4.5 dB, so the level it reports is the one it could set. Kept across
restarts. Reply: the full `/state`.

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

## `POST /api/v1/control`

`{"action": "volup"}`: what a button press does, sent to whatever is
playing (LMS, AirPlay, Spotify, Bluetooth). Actions: `volup`, `voldown`,
`toggle`, `play`, `pause`, `stop`, `next`, `prev`, `power`. (There is no
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
  {"name": "amp_no_fault",  "pass": true,  "value": "FAULTZ high after 200 ms enabled"},
  {"name": "psram",         "pass": true,  "value": "8 MB"},
  {"name": "heap",          "pass": true,  "value": "81234 B free"},
  {"name": "network",       "pass": false, "value": "not connected"}]}
```

`pass` is true only if every check passes. A check that can't run (the amp
on a 5 V port) fails with the reason in `value`; on a 5 V port that is
expected, and the bench script knows it.

## squeezelite-esp32's own endpoints

The web app also uses these, which squeezelite-esp32 already serves; this
firmware doesn't wrap them.

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

Not in this API yet: an absolute volume, mute and maximum volume, the
now-playing metadata, and learning IR remote codes. Volume works through
`control` (`volup`/`voldown`), which goes to whatever source is playing.

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
hifi selftest
```

squeezelite-esp32's own console commands (`help`, `nvsset`, `join`, …) are
there as well; see its README.
