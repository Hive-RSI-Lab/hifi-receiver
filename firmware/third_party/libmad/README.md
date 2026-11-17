# libmad, built from source with Debian's fixes

squeezelite-esp32 links its MP3 decoder from `components/codecs/lib/libmad.a`, a
prebuilt libmad 0.15.1b. That release overflows its Layer III `main_data`
buffer on crafted frames and reads past the end of its input
(CVE-2017-8372, CVE-2017-8373, CVE-2017-8374). Debian fixes these with
`md_size.diff` and `length-check.patch`. The shipped `.a` has asserts on, so
on the board a crafted MP3 (from LMS or an internet radio stream) trips the
`md_len` assert and reboots it. The `.a` comes with no source or build
recipe, so it can't be patched in place. This folder holds the source, and
patch 0029 builds it in place of the `.a`.

- `src/`: libmad-0.15.1b.tar.gz (sha256
  `bbfac3ed6bfbc2823d3775ebb931087371e142bb0e9bb1bee51a76a6e0078690`, the same
  file as Debian's `libmad_0.15.1b.orig.tar.gz`): the decoder's `.c`, `.h` and
  `.dat` files and its licence (GPL-2.0-or-later, as before). The patches below
  are applied to it, and `src/config.h` is added.
- `patches/`: what was changed in `src/`, in order. Commit 178e2c4 has the
  stock files, so `git diff 178e2c4 -- src` shows the same changes.
  - `01-inline-buffers.patch`: the board's port. The shipped `.a` keeps
    `main_data` inside `struct mad_stream`, and `overlap`, `xr` and `tmp` inside
    `struct mad_frame`. Stock libmad mallocs the first two and puts the others
    on the stack. The codecs are built against the shipped header
    (`components/codecs/inc/mad/mad.h`), so the source has to match it. No
    other change.
  - `02-md_size.patch`: Debian's `md_size.diff` (Kurt Roeckx, 0.15.1b-11).
    Checks that a frame's main data fits `main_data` before copying it.
  - `03-length-check.patch`: Debian's `length-check.patch` (the same author
    and version). Checks every `mad_bit_read` against the end of the buffer.
  - 02 and 03 are rebased onto 01, which moved some of the lines they touch.
- `src/config.h`: the settings the shipped `.a` was built with, as its header
  states them: `FPM_DEFAULT`, a 32-bit `long`.

`tools/fetch.sh` copies `src/` to `components/codecs/libmad` in the build tree.
Patch 0029 adds those files to the codecs component and drops `libmad.a`.

## Tests (firmware/test)

- `make mad`: `mad/test_mad.c` under AddressSanitizer, built twice: once on
  `src/` and once on a copy with 03 and 02 taken back out. The copy is stock
  0.15.1b with the board's buffers.
  - Crafted free-bitrate frames with `main_data_begin` 511 must overflow the
    copy (heap-buffer-overflow). The fixed build must refuse them, recoverably.
  - Both builds must decode `mad/sine.mp3`, a LAME file that uses the bit
    reservoir, to the same PCM hash, so the fixes change no samples.
  - With clang, `fuzz-mad` then runs the decoder under libFuzzer.
- `make upstream`: `mad/layout.c` prints every size and field offset of the
  shared structs, once as `mad.h` declares them and once as `src/` declares
  them. The two outputs must match. A mismatch would corrupt the heap on the
  board without warning.
- On a lab Lite, the `crafted` experiment (codec `mp3`) plays the frame from
  `test_mad frame 511`. Without the fix this reboots the board; with it, the
  frame is skipped.
