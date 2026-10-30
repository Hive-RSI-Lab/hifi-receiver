#!/bin/sh
# refill.sh - copy each board to $R (default /tmp/ds3/layout/refill), refill zones there (never in boards/).
set -e
eval "$(/hive/tools/hwde/bin/hwde env)"
R=${R:-/tmp/ds3/layout/refill}; mkdir -p "$R"
for b in lite-main core ultra-front; do
  [ -f "$R/$b.kicad_pcb" ] && continue
  cp /work/boards/$b/kicad/$b.kicad_pcb /work/boards/$b/kicad/$b.kicad_pro "$R/"
  cp /work/boards/$b/kicad/$b.kicad_dru "$R/" 2>/dev/null || true
  "$HWDE_KICAD_CLI" pcb drc --refill-zones --save-board -o "$R/$b.rpt" "$R/$b.kicad_pcb" >/dev/null 2>&1 || true
done
