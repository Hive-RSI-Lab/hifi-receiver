#!/bin/sh
# Puts squeezelite-esp32 at the pinned commit (upstream.lock) into DEST,
# applies our patches and copies our component and board defaults in.
# Run it again any time: it resets DEST's sources to the pinned commit first,
# keeping DEST/build and DEST/build-qemu (fw build --qemu) so rebuilds stay
# incremental.
#   firmware/tools/fetch.sh [DEST]     default: $HOME/fw-build/squeezelite-esp32
set -eu
FW=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$FW/.." && pwd)
DEST=${1:-${HOME}/fw-build/squeezelite-esp32}
BOARD=${HIFI_BOARD:-lite-main}
eval "$(grep -v "^#" "$FW/upstream.lock")"

g() { git -C "$DEST" -c http.version=HTTP/1.1 -c advice.detachedHead=false "$@"; }

if [ ! -d "$DEST/.git" ]; then
	mkdir -p "$DEST"
	g init -q
	g remote add origin "$url"
fi
if [ "$(g rev-parse -q --verify HEAD 2>/dev/null || true)" != "$commit" ]; then
	g fetch -q --depth 1 origin "$commit"
	g checkout -q -f "$commit"
fi
g reset -q --hard "$commit"
g clean -q -fdx -e build -e build-qemu -e /sdkconfig.old
g submodule -q update --init --recursive --depth 1 --jobs 2

for p in "$FW"/patches/*.patch; do
	[ -e "$p" ] || continue
	g apply --whitespace=nowarn "$p"
done

# ESP-IDF 4.4.8 security backports (idf-patches/README.md): a patched copy of
# each IDF component goes into the tree, where it overrides the IDF's own
IDF=${IDF_PATH:-$HOME/esp/esp-idf-v4.4.8}
if [ -n "$(ls "$FW"/idf-patches/*.patch 2>/dev/null)" ]; then
	grep -q "IDF_VERSION_PATCH 8)" "$IDF/tools/cmake/version.cmake" 2>/dev/null &&
		grep -q "IDF_VERSION_MINOR 4)" "$IDF/tools/cmake/version.cmake" || {
		echo "fetch.sh: idf-patches need ESP-IDF v4.4.8 at IDF_PATH ($IDF)" >&2
		exit 1
	}
	for comp in $(grep -h '^+++ b/components/' "$FW"/idf-patches/*.patch | cut -d/ -f3 | sort -u); do
		rm -rf "$DEST/components/$comp"
		cp -R "$IDF/components/$comp" "$DEST/components/$comp"
		# submodule .git files point into the IDF's own .git; drop them
		find "$DEST/components/$comp" -name .git -exec rm -rf {} +
	done
	for p in "$FW"/idf-patches/*.patch; do
		g apply --whitespace=nowarn "$p"
	done
fi

rm -rf "$DEST/components/hifi"
cp -R "$FW/components/hifi" "$DEST/components/hifi"

# libmad from source with Debian's fixes (third_party/libmad/README.md),
# which components/codecs builds in place of lib/libmad.a (patch 0029)
rm -rf "$DEST/components/codecs/libmad"
cp -R "$FW/third_party/libmad/src" "$DEST/components/codecs/libmad"

# The web app (app/, built by app/build.py) is served at '/'; without it the
# firmware serves a placeholder page that links to squeezelite's own UI.
if [ -f "$REPO/app/build.py" ]; then
	python3 "$REPO/app/build.py" --out "$DEST/components/hifi/esp/webapp" >/dev/null
fi

# as upstream's CI does: the board's defaults become the starting sdkconfig
# (upstream commits an sdkconfig of its own, which the reset above restores)
# A board folder with a 'base' file (board/lite-lab) is a variant: the base
# board's defaults with its own sdkconfig.extra lines appended, and the
# base's partition table, so the two images share bootloader and partitions.
BASE=$BOARD
if [ -f "$FW/board/$BOARD/base" ]; then
	BASE=$(tr -d ' \r\n' < "$FW/board/$BOARD/base")
	[ -f "$FW/board/$BASE/sdkconfig.defaults" ] || { echo "fetch.sh: $BOARD's base '$BASE' has no defaults" >&2; exit 1; }
fi
cp "$FW/board/$BASE/sdkconfig.defaults" "$DEST/sdkconfig.defaults"
if [ "$BASE" != "$BOARD" ] && [ -f "$FW/board/$BOARD/sdkconfig.extra" ]; then
	cat "$FW/board/$BOARD/sdkconfig.extra" >> "$DEST/sdkconfig.defaults"
fi
cp "$DEST/sdkconfig.defaults" "$DEST/sdkconfig"
cp "$FW/board/$BASE/partitions.csv" "$DEST/partitions-hifi.csv"
echo "$DEST"
