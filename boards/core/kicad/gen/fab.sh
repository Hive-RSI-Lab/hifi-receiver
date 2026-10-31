#!/bin/sh
# core's routed checks and fab files, Plus and Ultra (README "Rebuild and re-check" step 5).
# From boards/core:  sh kicad/gen/fab.sh
set -e
cd "$(dirname "$0")/../.."
H="sh ${HWDE:-$HIVE_MODULES/hive-hwde/bin/hwde}"
$H gate --gate drc_routed kicad/core.kicad_pcb --workspace "$PWD" --out reports/drc_routed.json
$H fab_export --pcb kicad/core.kicad_pcb --out-dir fab --out reports/fab_export.json >/dev/null
$H bom_cpl --pcb kicad/core.kicad_pcb --out-dir fab --parts parts/parts.json --easyeda-cache parts/easyeda --out reports/bom_cpl.json >/dev/null || true
$H bom_cpl --pcb kicad/core.kicad_pcb --out-dir fab/ultra --parts parts/parts-ultra.json --easyeda-cache parts/easyeda --out reports/bom_cpl_ultra.json >/dev/null || true
$H gate --gate dfm kicad/core.kicad_pcb --workspace "$PWD" --out reports/dfm.json
