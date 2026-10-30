#!/bin/sh
# R3-SYSTEM-1: the firmware governor against USB-C charger contracts.
# Exits non-zero when the firmware as it is lets the input pass the contract.
set -e
R=$(cd "$(dirname "$0")/../../../.." && pwd)
CORE=$R/firmware/components/hifi/core
T=$(mktemp -d)
cc -O1 -Wall -I"$CORE" "$R/docs/review-3/sims/system/gov_burst.c" "$CORE/hifi_gov.c" -lm -o "$T/asis"
# the proposed fix: the limiter works on the unclipped power (the clip cap
# only for reporting), with a 50 ms window
sed 's/return p < p_clip ? p : p_clip;/(void) p_clip; return p;/' "$CORE/hifi_gov.c" > "$T/hifi_gov_fix.c"
cc -O1 -DFIX -I"$CORE" "$R/docs/review-3/sims/system/gov_burst.c" "$T/hifi_gov_fix.c" -lm -o "$T/fix"
echo "--- proposed fix (informative)"; "$T/fix" || true
echo "--- firmware as it is"; "$T/asis"
