# libmad, built from source with Debian's fixes

squeezelite-esp32 links its MP3 decoder from `components/codecs/lib/libmad.a`, a
prebuilt libmad 0.15.1b with asserts on. That release has two kinds of bug on
crafted input (CVE-2017-8372, CVE-2017-8373, CVE-2017-8374), which Debian fixes
with `md_size.diff` and `length-check.patch`:

- **Reads past the end of its input** (`length-check.patch`). Bit reads in
  the frame header, side info and Huffman data don't check the buffer's end.
  This is the risk on the playback path: an MP3 from LMS or an internet
  radio stream reaches it through squeezelite's `mad.c`.
- **A write past `main_data`** (`md_size.diff`). A free-bitrate frame whose
  main data plus the 511 bytes it may take from the frame before is more
  than the 2567-byte buffer is copied in unchecked, past the end of
  `struct mad_stream` on the heap, or it trips the `md_len` assert and
  reboots the board. squeezelite's `mad.c` hands libmad at most 2048 bytes
  at a time (`READBUF_SIZE`), so a frame that big never reaches the decoder
  in normal playback: it waits for more input. The lab's `crafted`
  experiment (`components/hifi/esp/lab_crafted.c`) decodes a whole buffer
  at once and does reach it, which is how the board test below shows the
  fix. A future change to `READBUF_SIZE` would make it reachable for real.

The `.a` comes with no source or build recipe, so it can't be patched in
place. This folder holds the source, and patch 0029 builds it in place of the
`.a`. (`components/codecs/component.mk` still names `lib/libmad.a`; it is the
old make build, unused under CMake, like its ALAC line.)

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
  - `02-md_size.patch`: Debian's `md_size.diff` (Kurt Roeckx, added in Debian 0.15.1b-9 and still shipped in -11).
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
- `make upstream`: `mad/layout.c` prints every size, field offset and field
  type of the shared structs, and `MAD_F_FRACBITS` and the fixed-point mode,
  once as `mad.h` declares them and once as `src/` declares them. The two
  outputs must match. A mismatch would corrupt the heap or the samples on the
  board without warning. The host is 64-bit and can't build 32-bit
  (no multilib), which is why the types are compared as well as the sizes:
  a `long` against a `long long` is the same size here and not on the
  board. The 32-bit offsets were checked by hand once against the old `.a`
  (`md_len` at 2616 in both).
- The fuzzer feeds each input as one buffer. It does not slide the buffer
  the way `mad.c` refills it, so bit-reservoir state across refills is only
  covered by `sine.mp3`.
- On a lab Lite, the `crafted` experiment (codec `mp3`) decodes the frame
  from `test_mad frame 511` (2871 bytes, sent in pieces with `more` and
  `offset`, the last with `repeat` 64). Without the fix this reboots the
  board (Lite 5, 2026-11-17: `crashed_in` "crafted"); with it, the frames
  are skipped (65 calls, 65 refused, heap intact, no restart). The fixed decoder reports it as `MAD_ERROR_LOSTSYNC`
  (recoverable); at the very end of a stream, `mad.c` treats that as the end
  of the track, so a refused last frame ends it quietly.
