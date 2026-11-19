# Our own equalizer, in place of Espressif's

squeezelite-esp32's 10-band equalizer (`components/squeezelite/equalizer.c`,
the app's sound settings and loudness) calls Espressif's ESP-ADF equalizer,
which it links from `components/audio/lib/libesp_processing.a`. That archive
comes with no source, and its licence allows it only on Espressif products,
so an image anyone may download can't carry it (LICENSE: "No Espressif
equalizer in published images").

For a board whose `sdkconfig.extra` sets `CONFIG_HIFI_PUBLISHABLE_CODECS=y`
(the makers' image), `tools/fetch.sh` deletes the archive and Espressif's
header from `components/audio` and copies this folder's files there:

- `hifi_eq.c`: one peaking biquad per band (Robert Bristow-Johnson's Audio EQ
  Cookbook), an octave wide (Q = √2), at 31, 62, 125, 250, 500 Hz and 1, 2, 4,
  8, 16 kHz (the centres ESP-ADF uses), run in cascade in float on
  interleaved 16-bit samples in place, rounded and clipped once at the end.
  A band at 0 dB, or at or above 0.45 of the sample rate, is skipped, so a flat
  setting leaves the samples bit for bit as they were.
- `esp_equalizer.h`: the same six calls `equalizer.c` makes
  (`esp_equalizer_init`, `_uninit`, `_process`, `_set_band_value`,
  `_get_band_count`, `_get_band_value`), so `equalizer.c` and patch 0001 build
  unchanged.
- `CMakeLists.txt`: builds `hifi_eq.c` and links no prebuilt archive.

The app's equalizer and loudness work as on the lab's boards. The sound
differs a little from Espressif's filters, whose shape isn't published.

Host check: `cc -I. hifi_eq.c test.c -lm` with a sine in and the level out
(flat is bit-exact; +6 dB at 1 kHz gives 6.0 dB there and under 0.1 dB two
octaves off). `tools/makers_check.py` refuses a makers' build whose link map
names `libesp_processing`.

Every other board keeps Espressif's archive.

## Licence

`hifi_eq.c`, `esp_equalizer.h` and `CMakeLists.txt` here are our own work,
under the MIT licence:

Copyright (c) 2026 Hive-RSI-Lab

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
