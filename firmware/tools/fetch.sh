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

rm -rf "$DEST/components/hifi"
cp -R "$FW/components/hifi" "$DEST/components/hifi"

# The web app (app/, built by app/build.py) is served at '/'; without it the
# firmware serves a placeholder page that links to squeezelite's own UI.
if [ -f "$REPO/app/build.py" ]; then
	python3 "$REPO/app/build.py" --out "$DEST/components/hifi/esp/webapp" >/dev/null
fi

# as upstream's CI does: the board's defaults become the starting sdkconfig
# (upstream commits an sdkconfig of its own, which the reset above restores)
cp "$FW/board/$BOARD/sdkconfig.defaults" "$DEST/sdkconfig.defaults"
cp "$FW/board/$BOARD/sdkconfig.defaults" "$DEST/sdkconfig"
cp "$FW/board/$BOARD/partitions.csv" "$DEST/partitions-hifi.csv"
echo "$DEST"
