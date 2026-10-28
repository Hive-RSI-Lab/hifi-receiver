# Receiver firmware

The firmware for the receiver's ESP32-WROVER-E. It is
[squeezelite-esp32](https://github.com/sle118/squeezelite-esp32) (Spotify
Connect, AirPlay, Bluetooth, Lyrion/LMS, buttons, IR, OTA, Wi-Fi setup) with
our own component added: power sequencing and the power governor, test
signals, the test API, and the web app. Why it is built this way, and what
each part does, is in [`docs/firmware.md`](../docs/firmware.md); the API is
[`docs/test-api.md`](../docs/test-api.md).

| Path | What |
|---|---|
| `upstream.lock` | the squeezelite-esp32 commit we build on, and the ESP-IDF it needs (v4.4.8) |
| `patches/` | our changes to upstream: one patch, a few dozen lines of hooks (below) |
| `components/hifi/core/` | the logic, in plain C: PD contract, power sequencer, governor, tones, test API, console parser |
| `components/hifi/esp/` | the board code on ESP-IDF: pins, I2C, ADC, tasks, HTTP, mDNS, console |
| `board/lite-main/sdkconfig.defaults` | the build configuration for the Lite board |
| `test/` | host unit tests for `core/` |
| `tools/fetch.sh`, `tools/build.sh` | get upstream, patch it, build |

## Test it on a PC (no board, no ESP-IDF)

```sh
make -C firmware/test
```

gcc with AddressSanitizer and UBSan. It runs the power sequencer through
power-up, brown-out, faults and idle; the governor against a 65 W charger;
every tone's level and timing; and every API call and console command
against a mock board.

## Build it

You need ESP-IDF **v4.4.8** (squeezelite-esp32's CI uses the same). Then:

```sh
firmware/tools/build.sh            # builds into ~/fw-build/squeezelite-esp32
```

It runs `fetch.sh` (clone upstream at the pinned commit with its submodules,
apply `patches/`, copy `components/hifi` and the board's defaults in, and
build the web app from `app/` if it is there), then `idf.py reconfigure` and
`ninja -j4` at nice 19. The outputs are `build/recovery.bin` (the factory
app), `build/squeezelite.bin` (the main app, OTA slot 0), the bootloader and
the partition table.

**Installing ESP-IDF v4.4.8 on a machine whose Python is new** (the Hive's is
3.15, which v4.4's `install.sh` can't use):

```sh
git clone -b v4.4.8 --recursive --depth 1 --shallow-submodules \
    https://github.com/espressif/esp-idf ~/esp/esp-idf-v4.4.8
export IDF_TOOLS_PATH=~/esp/tools
python3 ~/esp/esp-idf-v4.4.8/tools/idf_tools.py install xtensa-esp32-elf esp32ulp-elf cmake ninja
uv venv --seed --python 3.10 ~/esp/tools/python_env/idf4.4_py3.10_env
~/esp/tools/python_env/idf4.4_py3.10_env/bin/pip install -r <(grep -v windows-curses ~/esp/esp-idf-v4.4.8/requirements.txt | grep -v gdbgui)
~/esp/tools/python_env/idf4.4_py3.10_env/bin/pip install --no-deps gdbgui==0.13.2.0   # export.sh checks it's there
```

`build.sh` finds that install by itself. The Hive's `hive-fw` module is
making a shared copy; when it lands, set `IDF_PATH` and `build.sh` uses it.

## Flash it

Over the board's USB-C (the CH340C console; DTR/RTS do the boot-mode dance):

```sh
cd ~/fw-build/squeezelite-esp32 && idf.py -p /dev/ttyUSB0 flash monitor
```

That writes the bootloader, partition table, recovery and the main app. Later
updates go over the network (`docs/test-api.md`, "squeezelite-esp32's own
endpoints").

## What the patch changes in upstream

`patches/0001-hifi-receiver-hooks.patch`, against `upstream.lock`'s commit:

- **`main/Kconfig.projbuild`:** a `HIFI_LITE` board choice carrying
  lite-main's codec (`dac_config` with MCLK on GPIO0), the ES8388 register
  set (upstream's, with the speaker output capped at −3 dB), the three
  buttons, IR on GPIO14, the LED on GPIO22 and the `hifi-lite` target and
  hostname.
- **`components/targets`:** registers `target_hifi`, so our component starts
  at boot.
- **`components/squeezelite/equalizer.c`:** calls `hifi_dsp_process` on every
  output block (the power governor).
- **`components/squeezelite/decode_external.c`:** a test-signal source beside
  Bluetooth/AirPlay/Spotify (`hifi_sink_*`).
- **`components/wifi-manager`:** `/` goes to the web app and squeezelite's
  page to `/squeezelite/`; room for 48 URI handlers; calls
  `hifi_http_register`.
- **`main/esp_app_main.c`:** the hostname defaults to `hifi-lite` without a
  MAC suffix.

Every hook is a weak symbol, so the patched tree still builds without our
component. To move to a newer upstream: change `upstream.lock`, run
`fetch.sh`, fix what doesn't apply, and regenerate the patch with
`git -C ~/fw-build/squeezelite-esp32 diff > firmware/patches/0001-hifi-receiver-hooks.patch`.
