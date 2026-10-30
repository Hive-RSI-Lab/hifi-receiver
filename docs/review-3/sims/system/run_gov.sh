#!/bin/sh
# R3-SYSTEM-1: the firmware governor (firmware/components/hifi/core/hifi_gov.c as
# it is) against USB-C charger contracts. Exits non-zero on a FAIL.
set -e
R=$(cd "$(dirname "$0")/../../../.." && pwd)
CORE=$R/firmware/components/hifi/core
T=$(mktemp -d)
cc -O1 -Wall -I"$CORE" "$R/docs/review-3/sims/system/gov_burst.c" "$CORE/hifi_gov.c" -lm -o "$T/gov"
"$T/gov"
