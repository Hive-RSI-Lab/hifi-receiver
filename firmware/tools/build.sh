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
