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
`ninja -j4` at nice 19. The Hive's signing key and a built-in relay come
from the environment, never from git (docs/remote.md):
`HIFI_HIVE_PUBKEY=<hex>` (comma-separate two while rotating), and optionally
`HIFI_RELAY_URL`, `HIFI_RELAY_USER`, `HIFI_RELAY_PASS`. Without
`HIFI_HIVE_PUBKEY` the board takes no command from the relay.

With the hive-fw module: `eval "$(fw env firmware)"` (it reads `fw.json`)
and then `firmware/tools/build.sh`. The outputs are `build/recovery.bin` (the factory
app), `build/squeezelite.bin` (the main app, for either OTA slot), the bootloader and
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
~/esp/tools/python_env/idf4.4_py3.10_env/bin/pip install "setuptools<70" "protobuf>=3.20,<4" "grpcio-tools>=1.48,<1.49"   # cspot's nanopb generator
```

`build.sh` finds that install by itself. The Hive's `hive-fw` module is
making a shared copy; when it lands, set `IDF_PATH` and `build.sh` uses it.

**What has been checked (2026-10-28):** the full build, both images, on
this machine with ESP-IDF v4.4.8: `squeezelite.bin` 2.69 MB in its 4 MB
slot, `recovery.bin` 1.38 MB in its 2 MB slot; the hooks link
(`target_hifi`, `hifi_dsp_process`, `hifi_http_register`, `hifi_sink_start`
in `squeezelite.elf`). **Not yet:** a boot. Espressif's QEMU (esp-develop
9.2.2) gets through the PSRAM test and then asserts in ESP-IDF's own startup
(`esp_vfs_console_register`), before any of our code runs; the hive-fw
module is looking at booting squeezelite in QEMU. QEMU needs
`CONFIG_ESP32_REV_MIN_0`; the board's config keeps rev 1, which the
WROVER-E's rev-3 chip meets.

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
- **`components/squeezelite/equalizer.c`:** calls `hifi_dsp_process` (the
  power governor) on every output block, after the equalizer and loudness,
  so it measures and limits exactly what reaches I2S.
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
`git -C ~/fw-build/squeezelite-esp32 -c core.fileMode=false diff -- . ':!sdkconfig' ':!sdkconfig.defaults' > firmware/patches/0001-hifi-receiver-hooks.patch` (the board's sdkconfig files are copied in, not patched).
