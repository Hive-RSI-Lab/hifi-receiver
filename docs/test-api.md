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
          "sub": {"count": 2, "crossover": false, "per_sub": false, "max_db": -9.0},
          "zones": 1, "eth": false, "pdo_v_max": 20,
          "channel": true, "led": false, "backlight": false}}
```

`channel`, `led` and `backlight` say whether this model answers
`POST channel`, `POST led` and `POST backlight` (a 404 where false): Lite
has the speaker side and neither of the others; Plus and Ultra have the LED
test; only Ultra has the backlight.

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
 "sys": {"heap_free": 81234, "heap_min": 60123, "psram_free": 3912345},
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
- **`audio.source`:** `idle`, `lms`, `airplay`, `spotify`, `bt`, `test` or
  `line` (Plus/Ultra: the line in through the ADC is playing).
  `input` is `stream`, `dj_direct` (Lite: the ES8388's analog bypass from
  the RCA line in) or `line` (Plus/Ultra). `tone` is the running tone (below) or `null`.
- **`audio.dj_guard`** (Plus/Ultra only): `{"in_w", "limit_w", "tripped"}`,
  DJ direct's thermal guard: the 60 s input power it sees, the amps'
  thermal budget as input power, and why it last moved DJ direct to `line`
  (`"power"` or `"otw"`; `null`: it hasn't since boot).
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

### `GET /api/v1/state/<section>`

One top-level section of the state, as `{"<section>": {...}}`: `power`,
`amp`, `governor`, `audio`, `net`, `sys`, `ir`, `controls` or `update`
(`{}` when the board has none of it now; any other name is a 404). The
whole state runs near 1000 characters on a Lite, which a serial line cuts
at; a section stays at or under 850 even at its worst (`audio`, with the
longest track text all in characters that need escaping; with plain text it
is about 670), so a bench test reads the part it saves. Track text is kept
to 59 bytes of title and 47 each of artist and album, cut at a whole
character, and control characters in it are stored as spaces.
Over the console: `api state/audio`, or `hifi state audio`.

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
  A `sine` above 5 kHz stays at or below −16 dBFS unless `"force": true`: a
  steady high tone heats core's output Zobels ([boards/core-zobel.md](boards/core-zobel.md)).
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
volume and governor don't act on it (the deck's own level does). The subs
get the line in through the ES7243E, about 10 ms behind, at the sub level.
If the amps would overheat (60 s input power over their thermal budget, or
OTW_CLIP low for 200 ms), the input moves to `line` by itself
(`audio.dj_guard`).
`{"input": "line"}` (Plus/Ultra; Lite answers 409) plays the line in
through the ES7243E and the digital path: volume, EQ, the governor and the
subs apply, about 0.1 s behind.
`{"input": "stream"}` goes back to the streamed audio. Reply:
`{"ok": true, "input": "..."}`.

## `POST /api/v1/sub`

`{"level_db": -6}` sets the sub outputs' level, −30 to +6 dB; `{"on": false}`
turns them off (either or both). Lite: the ES8388's LOUT2/ROUT2, 1.5 dB steps
up to +4.5 dB, so the level it reports is the one it could set. Plus/Ultra:
a digital gain on the sub DAC's stream, −40 to 0 dB in 0.5 dB steps (more
would clip). A level above the board's `info.caps.sub.max_db` is a 400, not
clamped. Kept across restarts. Reply: the full `/state`.

## `POST /api/v1/channel`

`{"channel": "stereo"|"left"|"right"|"mono"}`: which side the speaker output
plays. Lite only (404 on others). A stream (and a test tone) gets it in the DSP,
after the governor: `left` or `right` plays that side on both outputs,
`mono` plays (L + R) / 2 on both. The ES8388's cross-mix registers do
nothing, so the codec stays stereo. DJ direct is analog and can't cross
sides: `left` or `right` plays that side on its own output only (the other
is silent) and `mono` plays stereo. SUB OUT (LOUT2/ROUT2) follows the
mode as played. `stereo` is the default and the old behaviour. Kept
across restarts. Shown as `audio.channel` in `/state`. Reply: the full
`/state`; 400 on a bad value.

## `POST /api/v1/led` and `POST /api/v1/backlight`

`{"mode": "test"|"auto"}` lights the board's LEDs (Ultra: the front LED too)
for a few seconds, then hands them back. `{"mode": "on"|"off"|"auto"}` sets
Ultra's front-panel backlight, shown as `controls.backlight` in `/state`.
Not kept across restarts. Only on models whose `info.caps` has `led` or
`backlight` true (404 elsewhere); reply `{"ok": true}`, 400 on a bad mode.

## `POST /api/v1/eq`

`{"gains": [0, 0, 2, 0, 0, 0, 0, 0, -1, 0]}`: squeezelite-esp32's 10-band
equalizer (bands in `info.caps.eq_bands`), whole dB, `eq_min` to `eq_max`.
Applied at once and kept. Reply: the full `/state`.

## `POST /api/v1/loudness`

`{"boost_db": 6}`, `{"heat_cap_w": 12}`, or both; each is optional, and a
body with neither is a 400. Only boards whose `info.caps.loudness` is there
take it (Lite); elsewhere it is a 404 and `caps.loudness` is missing.
- **`boost_db`:** 0 to `caps.loudness.boost_max_db` (9), a number. Digital
  gain before the power governor, with a lookahead peak limiter (1 ms
  lookahead at 48 kHz, 25 ms hold, 0.15 s release) that keeps the boosted
  signal under the governor's clip ceiling, so a loud master gets louder
  without hard clipping. While Boost is on, the audio runs 48 frames late;
  turning Boost up from 0 crossfades into that delay over 4 ms, with no
  gap. The ceiling is the amp's clip point from the lower of the charger
  contract and the measured PVDD, 0.5 dB under the datasheet swing; while
  audio plays it ignores moves under 2% (PVDD read noise). Test tones and
  line-in at unity skip it, and after one Boost ramps up from unity again.
- **`heat_cap_w`:** sustained watts a channel the governor allows, from
  `heat_min_w` to `heat_max_w` (15), default `heat_def_w` (9). Above 9 W
  the amp may cut out when it gets hot; a fan helps. Lite's TPA3118 has no
  warning pin, so a trip is forgiven (not counted towards the amp's
  3-trips latch) only when the cap is above the default, the amp had been
  on at least 5 s, the governor's heat estimate is above the default, and
  no other trip was forgiven in the last fault window. A forgiven trip
  lowers the cap by 2 W (saved, shown in `heat_cap_w`), holds the gain 3 dB
  down for 60 s and keeps the amp off 15 s. Any other trip (a short trips
  at once, or again soon) counts and latches as before. The cap tops out
  at 15 W: the thermal model reaches the 150 C shutdown near there, so
  more needs a fan, a heatsink and a measurement.

Both are checked before either is set, and kept across restarts. Shown as
`audio.boost` (`db`, `limit_db`: the gain the limiter is taking off now,
`limiting`, `limit_events`) and `governor.heat_cap_w` / `heat_w` in
`/state`, with `amp.hot` when the warning pin is low. Reply: the full
`/state`; 400 on a bad value.

## `POST /api/v1/name`

`{"name": "Kitchen"}`, 1–32 bytes of UTF-8 (`\u00e9` counts as two): the
player name in AirPlay, Bluetooth, Spotify and LMS. The services take it at
the next restart. The hostname stays `hifi-lite`. Reply: the full `/state`.
A 400 for a control character (`\r`, `\u001f`, DEL), or `"` or `\`, which
would reach squeezelite's command line. A body with `\u0000`, an unknown
escape, bad hex or a lone surrogate in any string is refused whole ("body
must be a JSON object"), on every POST.

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

Runs every check the board can do on itself in about 3 s and returns them.
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
  {"name": "loopback",      "pass": true,  "value": "stereo: L +0.1/-43.4, R -42.1/-0.1 dB"},
  {"name": "psram",         "pass": true,  "value": "8 MB"},
  {"name": "heap",          "pass": true,  "value": "81234 B free"},
  {"name": "network",       "pass": false, "value": "not connected"}]}
```

`pass` is true only if every check passes. A check that can't run (the amp
on a 5 V port) fails with the reason in `value`; on a 5 V port that is
expected, and the bench script knows it.

After a remote link drop since boot, a `link_drop` check follows `network`:
it always passes, and its value is how long ago and why
(`"120s ago: TCP gave up: keepalive or resends"`, see `diag/net`).

On a board with loopback taps (the Lite; see `capture`), `loopback` checks
the speaker path end to end: a 1 kHz tone at -50 dBFS on the left side, then
the right, each read back on both taps. A speaker plugged in gives a faint
beep. The check measures the bare path, not the user's sound settings: for
the measurement it routes stereo and skips the EQ (Boost already skips test
tones), then puts the user's channel mode and EQ back. Its value is the mode
it measured in (always `stereo`), then for the left tone and the right tone
what the left and right taps read, in dB against the level the path should
give (`"stereo: L +0.1/-43.4, R -42.1/-0.1 dB"`: the left tone reaches the
left output only, the right tone the right). A fed output must read within
3 dB of 0 dB; the other must read 20 dB down or more. The quiet legs read the
taps' noise in the 1 kHz bin, with the capture's mean and slope taken out
first so a DC offset still settling after boot doesn't leak into it; expect
about -40 to -60 dB.
With a stream, a test tone, a connected source or DJ direct playing, the
check is skipped and passes (`"skipped: a stream plays"`, `"skipped: a source
is connected"`), and the same if a source connects while it runs; a second
self-test that starts while one runs skips it too (`"skipped: another
self-test runs it"`). With the amp off it fails (`"amp not on"`).

Plus and Ultra have no `vbus_matches` or `codec_i2c`; their board checks
are `expander_i2c` (the I/O expander at 0x20, or 0x74–0x77 for the
TCA9539), `dac_i2c` (the ES9018K2M at 0x48 answers and took the 32-bit word
length), `adc_i2c` (the ES7243E at 0x10 or 0x14; "set up" once the line in
has configured it) and `amp_otw` (OTW_CLIP high). Ultra adds `front_i2c`
(the front board's TCA9535 at 0x27, through the FFC).

## Diagnostics: `diag`

What the board knows about its own health and performance, for the
self-test reports (`projects/hifi-lite-lab/reports/`) and long soaks. All of
them are read-only and safe while the board plays, except `diag/echo`, which
sends filler up the bench link. Each reply stays under 900 characters, so it
fits a remote bench's reply line; the task list pages. On the console:
`hifi diag [tasks [from]|net|audio|ota|echo [bytes]]`.

### `GET /api/v1/diag`

```json
{"uptime_s": 5321,
 "boot_ms": {"app": 812, "ip": 4210, "link": 6034},
 "heap": {"free": 61234, "min": 38012, "largest": 31744, "total": 290000},
 "psram": {"free": 3911000, "min": 3700000, "largest": 3866624, "total": 4192000},
 "reset_reason": "restart", "resets": "restart,panic,power_on", "boots": 14,
 "nvs": {"nvs": {"used": 210, "free": 294, "total": 504, "namespaces": 6, "used_pct": 41.7},
         "settings": {"used": 96, "free": 1920, "total": 2016, "namespaces": 1, "used_pct": 4.8}},
 "power": {"state": "on", "vbus_v": 20.08, "vbus_i_a": 0.31, "pvdd_v": 19.9, "pg": true,
           "proto": "pd", "contract_v": 20.0, "contract_a": 3.25,
           "amp_on": true, "amp_fault": false, "amp_faults": 0, "amp_temp_c": null},
 "restart_cause": "api_reboot by esp_timer at 0x400e9a10 after 329 s"}
```

- `boot_ms`: ms from the app's start (the `esp_timer` clock, which starts
  with the app: the ROM's and the bootloader's time before it isn't counted)
  to the hifi code starting, the first IP and the
  remote link first up; `null` until each happens. `ip` stays `null` when
  the station had its IP before the diag handlers came up (a few seconds
  after boot), since the real time isn't known then.
- `heap` is internal, byte-addressable RAM (what `malloc` of an internal
  buffer can use; the IRAM-only part isn't counted), `psram` the PSRAM (`null` with none fitted): free
  now, the least free since boot, the largest free block and the size.
- `resets` is the reset history, newest first, up to 8 reasons, kept in NVS
  (namespace `hifi_diag`) with the `boots` counter. The reasons are
  `power_on`, `external`, `restart` (a software restart: an update, `restart`
  from the API), `panic`, `interrupt_watchdog`, `task_watchdog`, `watchdog`,
  `deep_sleep`, `brownout`, `sdio` and `unknown`.
- `restart_cause` (only when `reset_reason` is `restart`) names who restarted
  it: every `esp_restart` is link-wrapped to keep, in RTC memory across the
  restart, a note of why (when the path set one), the calling task, the
  call's address and the uptime: `<note> by <task> at 0x<pc> after <n> s`.
  With no note it reads `esp_restart by ...`: feed the address to
  `xtensa-esp32-elf-addr2line -e squeezelite.elf` to name the caller.
  `unrecorded` means a software restart that didn't pass the wrapper.
  `GET /api/v1/state/sys` carries it too; here it is left out when the reply
  would pass the page. Notes: `api_reboot`, `api_factory_reset`,
  `factory_reset`, `update_done`, `lab_offline` and `lab_stuck` (the lab's
  own watchdog), `ip_change` (the
  station's address changed, `esp_app_main.c`) and `squeezelite_exit`.
- `nvs` is each NVS partition's entries (32 bytes each): how full the flash
  key store is, for wear.
- `power` is the snapshot's power fields (`GET /api/v1/state`); a
  measurement the board can't make is `null` (`amp_temp_c` on a board with no
  sensor wired).

### `GET /api/v1/diag/tasks`, `GET /api/v1/diag/tasks/N`, `POST` with `{"from": N}`

Every FreeRTOS task: its stack high-water mark (the least free stack it has
had, in bytes), its core (`null`: either), priority and CPU load, busiest
first. The first page takes a new 1 s sample (the call blocks for it); a page
with `from` reads the same sample. `next` is where the next page starts, `n`
when the list is done. `diag/tasks/N` is the page from N with no body, for a
remote bench that takes only bodiless lines from a lane that doesn't hold the
board; `diag/tasks/0` takes a new sample like the first page. A start past 48
(the most the list holds) is a 400; a board running more than 48 tasks
answers 507 with the count in `error`, and a sample that fails (no memory
for it) a 503: try again. Another client's first page in between replaces
the sample, so pages read across it can mix two samples.

```json
{"n": 31, "from": 0, "sample_ms": 1000, "idle_pct": [71.2, 88.0],
 "cols": ["name", "stack_free", "core", "prio", "cpu_pct"],
 "rows": [["IDLE1", 788, 1, 0, 88.0], ["output_i2s", 2240, 0, 20, 14.1], ...],
 "next": 18}
```

`cpu_pct` is the share of one core over the sample; `idle_pct` is each
core's idle task, so 100 minus it is that core's load.

### `GET /api/v1/diag/net`

```json
{"wifi": {"rssi": -58, "channel": 6, "bssid": "aa:bb:cc:dd:ee:ff", "phy": "11n",
          "connects": 3, "disconnects": 2, "last_reason": 8, "up_s": 4012},
 "link": {"up": true, "drops": 1, "up_s": 3990, "tx_bytes": 812345, "rx_bytes": 4123456,
          "last_drop": {"kind": "tcp_gave_up", "code": 113, "wifi_down": false,
                        "why": "TCP gave up: keepalive or resends",
                        "ago_s": 3995, "lasted_s": 7210}}}
```

`rssi`, `channel`, `bssid` and `phy` are `null` while the station isn't
associated. `connects` and `disconnects` count the station's associations since the
handlers came up (a few seconds after boot); an association already up then
counts as one connect. `last_reason` is the last
disconnect's `wifi_err_reason_t`. `link` is the remote link: for wss:// the
bytes on the socket, for MQTT `0` (not counted).

`link.last_drop` is why the last up wss:// link ended (`null`: none since
boot, and always on MQTT). The first cause the board saw on that link wins.
`kind` is one of:

| `kind` | `code` | what ended the link |
|---|---|---|
| `closed` | the close frame's status | the endpoint sent a close frame |
| `eof` | 0 | the far end closed the socket (TCP FIN or TLS close_notify) |
| `tcp_gave_up` | `ECONNABORTED` | with Wi-Fi up, TCP's keepalive (30 s idle, then 3 probes 10 s apart) or its resends ran out; lwIP doesn't say which |
| `socket` | the errno | another socket error (`why` names the common ones: reset, timed out, no route) |
| `tls` | mbedTLS's error, positive | a TLS error |
| `read_timeout` | seconds | half a frame (or a TLS record) came, then nothing for that long |
| `write_timeout` | seconds | a send stalled that long: the socket's send buffer stayed full |
| `bad_frame` | 0 | a frame header the board won't take |
| `no_memory` | 0 | no heap for a frame |
| `wifi_down` | 0 or `ECONNABORTED` | the station lost its access point while the link was up (losing it takes the address, and the stack aborts the socket; a blip the station rode out can still run TCP's clock out) |
| `stopped` | 0 | the link was turned off |
| `unknown` | 0 | the link ended with no cause noted |

`wifi_down` says whether the station lost its access point while that link
was up (a disconnect counted, or no access point at the drop), whatever the
cause; it is always `false` for a link over Ethernet. `why` is the one-line
text (the self-test's passing `link_drop` line carries it too, as
`Ns ago: <why>`); `ago_s` is how long ago it dropped and `lasted_s` how long
that link had been up.

### `GET /api/v1/diag/audio`

```json
{"underruns": 0, "rate": 44100, "buffer_ms": 1520, "dma_ms": 46,
 "latency_ms": 1566, "played_s": 3600}
```

`underruns` counts the times the output ran dry while a stream was playing
(once per dry spell): an LMS (squeezelite), AirPlay or Spotify stream. It
counts only once the output has played since it started or was flushed, so
a start that is still filling (Spotify's play comes before its data) or a
seek isn't one. A Bluetooth stream feeds the output its own way and isn't
counted. `buffer_ms` is decoded audio waiting in the output
buffer, `dma_ms` the I2S DMA's fixed delay; `latency_ms` is their sum, from
decode to the pins (on an idle board, the DMA's floor alone). `played_s` is
the time the output has run since boot, silence in a dry spell included. A build
without the output's counters answers `{"underruns": null}`.

### `GET /api/v1/diag/ota`

The last update written over the relay: `{"version", "bytes", "total_ms",
"write_ms", "kbps", "write_kbps", "boot"}`. `total_ms` is from the slot's
open to the commit (the link's chunks included), `write_ms` the part spent
erasing and writing flash, `boot` the boot count when it was written.
`{"version": null}` before the first.

### `POST /api/v1/diag/echo`

`{"bytes": N}` (1 to 1048576, default 131072): sends N bytes of filler up the
wss:// bench link in 8 KB messages (`{"t":"sink",...}`, which the bench drops)
and times it: `{"bytes", "ms", "kbps"}`. The board's upload throughput to the
bench. The test stops after 20 s (checked between messages, so a stalled send
can run over), so on a slow link `bytes` (what was sent) can be less than
asked; the last message is padded to at least 21 bytes, so it can also be up
to 20 more. 409 when the link isn't wss:// or isn't up, 503 when it drops
during the test: a failed send drops the bench link, which then reconnects.
The test runs in the command worker, which queues 4 commands, so a bench
should wait for its reply before sending more.

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

## Party mode: `party`, `party/stats`

Receivers on the same Wi-Fi play in step: one leads and sends what it plays,
the others follow it within about 100 µs (`projects/hifi-lite-lab/party-mode.md`).
Only boards holding the Hive's party key take part, and nothing is opened
on the router: no UPnP, no port forward, broadcast only on the subnet.

`GET /api/v1/party`:

```json
{"on": true, "role": "leader", "key_id": 41, "key": true, "term": 3, "picked": false,
 "leader": "hifi-lite-2", "followers": ["192.168.2.221", "192.168.2.223"],
 "why": "", "bt": true}
```

- `role`: `off`, `listening` (on, looking for a leader), `leader` or
  `follower`. A follower also gives `leader_ip`.
- `picked`: this leader was chosen with `lead`; otherwise the first board
  switched on leads, and the others join it by themselves.
- `why`: why it isn't playing in step yet: `low memory`, `another access
  point` (the leader is on another AP, so their clocks differ), `leader lost`,
  `no party key`. `full: true` when the leader has all the followers it
  takes; `other_ap: true` as in `why`. It needs two announces from one
  address whose TSF moves on with the time between them, so one replayed
  announce doesn't set it, but a recorded run of them played back at its own
  pace can. `skip_s` while a leader this board joined sent it no CLOCK:
  the seconds until it tries that leader again (10 s, doubling up to 60 s
  each time the same one fails again; with more than one such leader, the
  longest wait).
- `bt`: whether the Bluetooth sink is on. It stays on only on a picked
  leader or while Bluetooth plays: connect the phone before switching party on.

`POST /api/v1/party` with `{"op": ...}` replies with the same JSON:

| `op` | From | Does |
|---|---|---|
| `on`, `off` | LAN, USB, Hive | switch party on or off (kept across restarts). `on` without a key is 409. |
| `lead` | LAN, USB, Hive | this board leads; the old leader follows it. 409 when party is off or memory is low, 429 a few seconds after its own last `lead`. |
| `key` | USB, Hive | `{"op": "key", "secret": "<64 hex>"}` sets the party key (all zeros is refused). Never shown back; `key_id` is one byte of its hash. |
| `tone` | USB, Hive | `{"op": "tone", "on": true}`: the leader's sync test tone, for the bench. An idle leader takes the output at 44.1 kHz for it. |
| `pair` | USB, Hive | `{"op": "pair", "n": 2}`: the leader starts a stream packet only with two ready and sends them back to back, so the radio can send both in one exchange. `n` is 1 (the default) or 2; it holds across party off and on until a restart. For the bench's A/B. |

`key`, `tone` and `pair` from the LAN are 403. An image built for 32-bit frames
answers `on` with 409 (party carries 16-bit).

`GET /api/v1/party/stats` (409 when party is off): the clock offsets, the
jitter buffer's counts (`got`, `dup`, `old`, `skip`, `gaps`, `miss`), the
rate controller and the heap, for the bench's `party_run.py`. On a leader,
`net.pk` counts stream packets sent (no resends or clocks; one held and then
dropped as too late is also in `net.hold[1]`) and `net.pair` is the `pair`
setting.

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

The same calls over the USB serial console (115200 8N1, CH340B), for a
board that isn't on a network yet. Each prints the JSON reply on one line.

```
hifi info
hifi state [section]
hifi tone sine 1000 -20 [both|left|right] [seconds]
hifi tone sweep 20 20000 -20 [seconds]
hifi tone noise -20 [seconds]
hifi tone silence [seconds]
hifi tone stop
hifi amp on|off|reset
hifi input stream|dj_direct|line
hifi sub <dB>
hifi channel stereo|left|right|mono
hifi control <action>
hifi volume <0-100>
hifi mute on|off
hifi maxvol <30-100>
hifi ir learn <action>
hifi ir forget [action]
hifi party [on|off|lead|stats]
hifi selftest
```

And any call in one line, as the bench and `provision.py` use it: `api <op>`
is a GET, `api <op> "<json>"` a POST, with the JSON in double quotes and its
`"` and `\` backslash-escaped (the console splits on spaces). The reply is
one line `API {json}`, with `"status"` added on an error:

```
api state
api state/audio
api wifi "{\"ssid\": \"home\", \"password\": \"secret\"}"
API {"ok":true}
```

squeezelite-esp32's own console commands (`help`, `nvsset`, `join`, …) are
there as well; see its README.
