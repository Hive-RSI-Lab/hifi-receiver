#!/bin/bash
# Builds the receiver firmware: fetches/patches upstream (fetch.sh), then
# idf.py build at low priority with at most 4 jobs.
#   firmware/tools/build.sh [DEST]
# Needs ESP-IDF v4.4.8: IDF_PATH set, or an install at $HOME/esp (see
# firmware/README.md), or hive-fw's 'fw' tool.
set -eu
FW=$(cd "$(dirname "$0")/.." && pwd)
DEST=$("$FW/tools/fetch.sh" "${1:-${HOME}/fw-build/squeezelite-esp32}" | tail -1)

if [ -z "${IDF_PATH:-}" ]; then
	export IDF_PATH="$HOME/esp/esp-idf-v4.4.8" IDF_TOOLS_PATH="$HOME/esp/tools"
	for env in "$IDF_TOOLS_PATH"/python_env/idf4.4_py3.*_env; do
		export IDF_PYTHON_ENV_PATH="$env"
	done
	export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
	set +u
	. "$IDF_PATH/export.sh" >/dev/null
	set -u
fi

cd "$DEST"
idf.py reconfigure >/dev/null
nice -n 19 ninja -C build -j "${JOBS:-4}"
ls -l build/squeezelite.bin build/recovery.bin
# squeezelite's build doesn't check the recovery image against its slot: do
# it here for both, since an image that outgrows its slot can't be flashed
python3 - partitions-hifi.csv build/recovery.bin build/squeezelite.bin <<'EOF'
import os, sys
parts = {}
for line in open(sys.argv[1]):
    f = [x.strip() for x in line.split("#")[0].split(",")]
    if len(f) >= 5 and f[1] == "app":
        parts[f[2]] = int(f[4], 0)
bad = 0
for img, slot in ((sys.argv[2], "factory"), (sys.argv[3], "ota_0")):
    n, cap = os.path.getsize(img), parts[slot]
    print(f"{os.path.basename(img)}: {n / 2**20:.2f} MB of {cap / 2**20:.2f} MB ({100 * n / cap:.0f}%)")
    bad |= n > cap
sys.exit(bad)
EOF
