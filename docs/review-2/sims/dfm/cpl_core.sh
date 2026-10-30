# re-runs hwde cpl_verify on core Plus and Ultra until EasyEDA stops rate-limiting
D=/work/docs/review-2/sims/dfm
S=$HIVE_MODULES/hive-hwde/.claude/skills/hwde/scripts; P=/hive/home/.cache/hwde/venv/bin/python
for i in $(seq 1 30); do
  $P $S/cpl_verify.py --pcb /work/boards/core/kicad/core.kicad_pcb --cpl /work/boards/core/fab/ultra/CPL.csv --parts /work/boards/core/parts/parts-ultra.json --cache-dir $D/easyeda-cache --out $D/cpl_ultra.json > $D/cpl.log 2>&1
  $P $S/cpl_verify.py --pcb /work/boards/core/kicad/core.kicad_pcb --cpl /work/boards/core/fab/CPL.csv --parts /work/boards/core/parts/parts.json --cache-dir $D/easyeda-cache --out $D/cpl_plus.json >> $D/cpl.log 2>&1
  $P $S/cpl_verify.py --pcb /work/boards/lite-main/kicad/lite-main.kicad_pcb --cpl /work/boards/lite-main/fab/CPL.csv --parts /work/boards/lite-main/parts/parts.json --cache-dir $D/easyeda-cache --out $D/cpl_lite.json >> $D/cpl.log 2>&1
  grep -q "rate limited" $D/cpl.log || { echo done $i; exit 0; }
  sleep 200
done
