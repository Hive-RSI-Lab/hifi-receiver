# libogg, Tremor, opus and libFLAC, built from pinned sources

The makers' image (`board/lite-makers`, anyone may download and flash it) has
to ship with the source of everything it links. squeezelite-esp32 links its
Ogg, Vorbis, Opus and FLAC decoders from prebuilt archives in
`components/codecs/lib/` (`libogg.a`, `libvorbisidec.a`, `libopus.a`,
`libFLAC.a`). Upstream records no source, version or build recipe for any of
them. From the archives themselves:

- `libvorbisidec.a` (Tremor, 2019) calls `heap_caps_*`, so it was modified
  for the ESP32, and that change was never published.
- `libopus.a` (2023) is a fixed-point build with opus's default pseudostack
  (`NONTHREADSAFE_PSEUDOSTACK`, 120000 bytes). Its headers predate 1.5.
- `libogg.a` (2023) has libogg 1.3.x headers. `libFLAC.a` (2026) is said to
  be "stock 1.4", but its version string says 1.5.0.

The four libraries are all Xiph.Org's, under the BSD-3-Clause licence (each
source's `COPYING`), which fits with the GPL. Rather than ship archives whose
source can't be named, the makers' image builds each one from a pinned
release (`sources.lock`).

| name | version | from |
|------|---------|------|
| ogg | 1.3.6 | downloads.xiph.org release tarball |
| opus | 1.5.2 | downloads.xiph.org release tarball (sha256 as in xiph's SHA256SUMS) |
| flac | 1.5.0 | downloads.xiph.org release tarball (sha256 as in xiph's SHA256SUMS) |
| tremor | git 820fb3237ea81af44c9cc468c8b4e20128e3e5ad | gitlab.xiph.org archive of that commit (Tremor has no release) |

`tools/fetch.sh` does the work for a board whose `sdkconfig.extra` sets
`CONFIG_HIFI_PUBLISHABLE_CODECS=y`. It downloads each source into
`~/.cache/hifi-xiph` (or `HIFI_XIPH_CACHE`), and fails if the sha256 differs
from `sources.lock`. It then unpacks the sources to
`components/codecs/xiph/<name>`, gathers their public headers in
`components/codecs/xiph/include`, and deletes the prebuilt archives and
their `inc/` headers, so nothing can link them by mistake. Patch 0038 makes
`components/codecs` build the sources through `xiph.cmake`. Every other
board still links the prebuilt archives, as upstream does.

Build settings, kept to the prebuilt ones where they can be told:

- ogg: `src/bitwise.c` and `src/framing.c`. `include/ogg/config_types.h` is
  the one squeezelite-esp32 ships beside `libogg.a` (stdint types).
- Tremor: the decoder's 13 `.c` files, full accuracy (no `_LOW_ACCURACY_`).
  It allocates with plain `malloc`, not the prebuilt's `heap_caps_*`.
- opus: decoder and encoder lists from its own `opus_sources.mk`,
  `celt_sources.mk` and `silk_sources.mk` (`SILK_SOURCES_FIXED`), with
  `FIXED_POINT`, `DISABLE_FLOAT_API` and `NONTHREADSAFE_PSEUDOSTACK`, as in
  the prebuilt. There's no DRED, OSCE or deep PLC, and no SIMD.
- libFLAC: the decoder's files only, with Ogg FLAC (`FLAC__HAS_OGG=1`). It
  uses 32-bit words and no assembly or intrinsics. The settings are defines
  in `xiph.cmake`, not a `config.h`, because the codecs component already
  has libmad's `config.h` on its include path.

The image also leaves out two prebuilt archives that can't be published at
all (`components/hifi/Kconfig`, `HIFI_PUBLISHABLE_CODECS`):

- Helix AAC (`lib/libhelix-aac-sbr.a`) is under RealNetworks' RPSL/RCSL,
  which doesn't fit with the GPL, so the image has no AAC.
- libresample16 (`lib/libresample16.a`) has no recorded origin, so the
  image has no squeezelite `-R` resampling, which the Lite doesn't use.

`tools/makers_check.py` refuses a makers' build if either link map names
`libhelix`, `libresample16` or one of the four prebuilt archives.

ALAC and libmad were already built from source (patches 0021 and 0029,
`../libmad/README.md`). The equalizer is our own too, in place of Espressif's
`libesp_processing.a` (`../eq/README.md`).
