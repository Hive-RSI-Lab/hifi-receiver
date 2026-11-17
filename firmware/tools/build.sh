#!/bin/bash
# Builds the receiver firmware: fetches/patches upstream (fetch.sh), then
# idf.py build at low priority with at most 4 jobs.
#   firmware/tools/build.sh [DEST]
# Needs ESP-IDF v4.4.8: IDF_PATH set, or an install at $HOME/esp (see
# firmware/README.md), or hive-fw's 'fw' tool.
set -euo pipefail
FW=$(cd "$(dirname "$0")/.." && pwd)

# no IDF_PATH and no $HOME/esp: the fw tool's copy, pinned by firmware/fw.json
FWTOOL=$(command -v fw || echo "$HOME/modules/fw/bin/fw")
if [ -z "${IDF_PATH:-}" ] && [ ! -d "$HOME/esp/esp-idf-v4.4.8" ] && [ -x "$FWTOOL" ]; then
	eval "$(cd "$FW" && "$FWTOOL" env .)"
fi
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

# fetch.sh needs IDF_PATH too (idf-patches/)
DEST=$("$FW/tools/fetch.sh" "${1:-${HOME}/fw-build/squeezelite-esp32}" | tail -1)

# Spotify Connect needs a Spotify developer app's client id and secret built in
# (cspot fetches a Web API token with them; without one a track never starts):
# from SPOTIFY_CLIENT_ID / SPOTIFY_CLIENT_SECRET, else files of those names in
# HIFI_SECRETS_DIR (default ~/.config/h2/secrets). Never echoed.
SECRETS_DIR=${HIFI_SECRETS_DIR:-$HOME/.config/h2/secrets}
sp_id=${SPOTIFY_CLIENT_ID:-}
sp_secret=${SPOTIFY_CLIENT_SECRET:-}
[ -n "$sp_id" ] || { [ -r "$SECRETS_DIR/SPOTIFY_CLIENT_ID" ] && sp_id=$(tr -d ' \r\n' < "$SECRETS_DIR/SPOTIFY_CLIENT_ID"); } || true
[ -n "$sp_secret" ] || { [ -r "$SECRETS_DIR/SPOTIFY_CLIENT_SECRET" ] && sp_secret=$(tr -d ' \r\n' < "$SECRETS_DIR/SPOTIFY_CLIENT_SECRET"); } || true
if [ -n "$sp_id" ] && [ -n "$sp_secret" ]; then
	case "$sp_id$sp_secret" in *[!A-Za-z0-9]*) echo "build.sh: the Spotify client id/secret must be letters and digits" >&2; exit 1 ;; esac
	export SPOTIFY_SECRET="-DCLIENT_ID=\"$sp_id\" -DCLIENT_SECRET=\"$sp_secret\""
	echo "build.sh: Spotify Connect key: set"
else
	unset SPOTIFY_SECRET
	echo "build.sh: Spotify Connect key: missing, Spotify will connect but play nothing" >&2
fi
unset sp_id sp_secret

# The Hive's public key, which the board needs to take agent commands and
# updates over Wi-Fi (hifi_relay.c): from HIFI_HIVE_PUBKEY, else the bench
# endpoint's tool if this machine has one. A build without it can only be
# flashed by USB, and the bench refuses to send it over the air.
if [ -z "${HIFI_HIVE_PUBKEY:-}" ] && [ -r "$HOME/modules/dash/remote_bench.py" ]; then
	HIFI_HIVE_PUBKEY=$(python3 "$HOME/modules/dash/remote_bench.py" pubkey 2>/dev/null || true)
fi
if [ -n "${HIFI_HIVE_PUBKEY:-}" ]; then
	case "$HIFI_HIVE_PUBKEY" in *[!0-9a-fA-F,\ ]*) echo "build.sh: HIFI_HIVE_PUBKEY must be hex keys, comma-separated" >&2; exit 1 ;; esac
	export HIFI_HIVE_PUBKEY
	echo "build.sh: Hive key: set"
else
	unset HIFI_HIVE_PUBKEY
	echo "build.sh: Hive key: missing, the board will refuse agent commands and Wi-Fi updates" >&2
fi

# The build number (seconds since 1970, UTC) the image carries and
# tools/sign_fw.py signs: a board refuses a LAN update with a lower one
# (security.md L9). Set HIFI_FW_BUILD to rebuild one exactly.
HIFI_FW_BUILD=${HIFI_FW_BUILD:-$(date -u +%s)}
case "$HIFI_FW_BUILD" in '' | *[!0-9]* | 0) echo "build.sh: HIFI_FW_BUILD must be a positive number" >&2; exit 1 ;; esac
export HIFI_FW_BUILD
echo "build.sh: build number $HIFI_FW_BUILD"

cd "$DEST"
idf.py reconfigure >/dev/null
nice -n 19 ninja -C build -j "${JOBS:-4}"
# "cannot open the home network": no port-mapping client (UPnP IGD, NAT-PMP,
# PCP, SSDP) may be linked into either image (security.md)
if grep -i -E -l 'upnp|natpmp|nat_pmp|ssdp|pcp_client' build/squeezelite.map build/recovery.map; then
	echo "build.sh: a port-mapping client is linked in (the maps above); refused" >&2
	exit 1
fi
# boot the main app (ota_0) after a flash from scratch, not the recovery app
# that IDF's all-0xff otadata selects (tools/otadata.py says why)
python3 "$FW/tools/otadata.py" build/ota_data_initial.bin
# sign the app for updates from the LAN, which the board refuses unsigned
# (tools/sign_fw.py; security.md). The relay and USB don't need it.
python3 "$FW/tools/sign_fw.py" build/squeezelite.bin ${HIFI_SIGN_KEY:+--key "$HIFI_SIGN_KEY"}
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
