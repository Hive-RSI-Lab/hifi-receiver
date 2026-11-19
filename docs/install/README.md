# The makers' build

A HiFi Lite firmware anyone can flash from a browser with
[ESP Web Tools](https://esphome.github.io/esp-web-tools/): one merged image,
its manifest, and the page here (`index.html`). It is kept on the branch
`h2/makers-installer` and **not published**: no release and no Pages site.
Publishing waits on a licence pick and the person's go.

## What's in it and what isn't

It is the Lite's usual firmware (`board/lite-main`) with every lab and private
part taken out, from a config of its own, `firmware/board/lite-makers/`:

| Left out | Why |
|---|---|
| Spotify Connect (`CONFIG_CSPOT_SINK` off, no key) | cspot needs a Spotify developer app's client id and secret built into the image to fetch a Web API token, and anyone could read them out of a public file. Spotify's terms don't allow sharing them, so the build leaves Spotify out rather than ship a Spotify that connects and plays nothing. Build your own with your own key (below) to have it. |
| The Hive's public key (`HIFI_HIVE_PUBKEY`) | It is what lets the lab's agents command and update a board over the relay. A makers' board takes no such command. |
| The relay and update endpoints (`HIFI_RELAY_*`, `HIFI_UPDATE_URL`) | the lab's dial-in. (The lab boards get theirs in NVS at bring-up, not from the image.) |
| The LAN signature | made with the bench's signing key (`tools/sign_fw.py`). Without it, a board refuses LAN updates; update it by USB from the page. |
| Lab tools (`CONFIG_HIFI_LAB` off) | the bench's test hooks (`lab_*.c`, `hifi_lab_esp.c`). |
| Wi-Fi credentials | none are built in. The board starts its setup network the first time. |
| AAC (Helix, `lib/libhelix-aac-sbr.a`) | RealNetworks' RPSL/RCSL licence doesn't fit with the GPL, so the image can't carry it. The board plays no AAC; MP3, FLAC, ALAC, Vorbis, Opus and PCM all play, and AirPlay and Bluetooth bring their own codecs. |
| squeezelite's `-R` resampler (`lib/libresample16.a`) | Upstream records no origin or source for it. The Lite doesn't use it. |

The other prebuilt codec archives upstream links (`libogg`, `libvorbisidec`,
`libopus`, `libFLAC`) name no source either, so the image builds those four
from pinned Xiph.Org releases instead (`firmware/third_party/xiph/README.md`,
`sources.lock`: each one's version, URL and SHA-256). ALAC and libmad were
already built from source. `CONFIG_HIFI_PUBLISHABLE_CODECS` in the board's
`sdkconfig.extra` turns this on; every other board links upstream's archives.

The equalizer behind the app's sound settings and loudness is squeezelite's,
but upstream runs it on Espressif's ESP-ADF filters (`libesp_processing.a`),
which come with no source and may only ship on Espressif products. This image
runs it on a small biquad equalizer of our own instead
(`firmware/third_party/eq/README.md`), so the app's equalizer still works.

Wi-Fi onboarding uses the firmware's own **setup AP** (squeezelite-esp32's
wifi-manager), not Improv: with no Wi-Fi saved the board opens `hifi-lite-` plus the
last three bytes of its MAC in hex (`hifi-lite-8a0cbe`; AirPlay and Bluetooth
use the same name), password `squeezelite`. Join it and open `http://192.168.4.1` to pick a
network.

## Rebuild it

You need ESP-IDF v4.4.8 (see `firmware/README.md`), or the hive-fw tool:

```sh
eval "$(fw env firmware)"        # or IDF_PATH set by hand
HIFI_BOARD=lite-makers firmware/tools/build.sh ~/fw-build/makers
```

`build.sh` sees the `makers` file in the board folder and:

- refuses to start if `HIFI_HIVE_PUBKEY`, `HIFI_RELAY_*`, `HIFI_UPDATE_URL` or
  `HIFI_SIGN_KEY` is set, doesn't look up the Hive key itself, ignores
  any Spotify key in the environment or in `~/.config/h2/secrets`, and doesn't sign;
- builds, then runs `firmware/tools/makers_check.py`, which **fails the build**
  if any private item is in any image (below);
- runs `firmware/tools/makers_image.py`, which checks each image's offset
  against the partition table and writes `docs/install/firmware/`:
  `hifi-lite-makers.bin` (the merged image, flashed at 0),
  `manifest.json`, and `hifi-lite-makers.json` (each part's offset, size and
  SHA-256, the merged file's, and the source: this repo's commit, the
  squeezelite-esp32 commit and IDF from `firmware/upstream.lock`, and each
  codec source from `sources.lock`; the page
  shows them, for the GPL's source offer). It refuses a `firmware/` with
  uncommitted changes unless given `--dirty` (`HIFI_MAKERS_DIRTY=1` for build.sh), which marks the file dirty;
- scans the merged file too, and deletes the output if that scan fails.

`docs/install/firmware/` is in `.gitignore`: the image is built, never
committed. To try the page, serve `docs/install/` on localhost (Web Serial
needs https or localhost):

```sh
python3 -m http.server --bind 127.0.0.1 8000 -d docs/install
```

and open `http://127.0.0.1:8000` in Chrome or Edge.

**With Spotify, for yourself.** Make a Spotify developer app and build the usual
Lite config instead: `SPOTIFY_CLIENT_ID=... SPOTIFY_CLIENT_SECRET=... firmware/tools/build.sh`.
That image carries your key, so don't share it.

## The leak scan

`makers_check.py BUILD` reads every `.bin` in the build as bytes and looks
for each private item as hex in either case and as raw bytes: the Hive's
public key, the bench signing seed, the bench host and its Cloudflare Access
team, the Access token's id and secret, the Spotify key, the saved Wi-Fi
network and password, every other file in `~/.config/h2/secrets` (named by
file), and any `--ssid` given. It also checks that
`CONFIG_HIFI_LAB` and `CONFIG_CSPOT_SINK` are off and `CONFIG_HIFI_PUBLISHABLE_CODECS`
is on, that the link maps hold no lab object, no cspot library, no `libhelix`,
no `libresample16`, no `libesp_processing` and none of the four prebuilt Xiph archives, and that no image ends in a LAN signature.
It prints only each item's name, never its value; an item whose source isn't
on the machine shows as *not checked*, not passed. `--file F` scans another
file too (the merged image). `--strict` fails on any *not checked* item;
`build.sh` passes it on the lab's machine (where `~/modules/dash` is).

## The lab twin

A lab board never runs the makers' app itself. To test the build on one, the
lab builds its twin, the makers' config plus the Hive key and nothing else:

```sh
HIFI_BOARD=lite-makers HIFI_MAKERS_TWIN=1 firmware/tools/build.sh ~/fw-build/makers-twin
```

The twin's scan (`makers_check.py --twin`) needs the Hive key in the app
(the positive control that the scan sees it) and passes nothing else.
