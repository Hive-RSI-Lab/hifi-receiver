#!/bin/sh
# R4-PAD mute benches (hwde sim_run). Exit 0 = no error-severity bound fails.
eval "$(/hive/tools/hwde/bin/hwde env)"
cd "$(dirname "$0")" && $HWDE_PY $HWDE_SKILL/scripts/sim_run.py --dir . --out ${1:-/tmp/pad_mute_report.json}
