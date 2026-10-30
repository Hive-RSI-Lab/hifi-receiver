# fetch EasyEDA models IC-first for core (rate-limited), then re-run cpl_verify offline
D=/work/docs/review-2/sims/dfm; S=$HIVE_MODULES/hive-hwde/.claude/skills/hwde/scripts; P=/hive/home/.cache/hwde/venv/bin/python
CODES="C46061833 C2687116 C2058311 C97054 C45223 C2688377 C192421 C107671 C2929446 C11133 C475500 C465732 C91447 C717684 C25120 C25117 C25900 C25076 C23164 C25765 C25792 C25798 C25105 C17168"
for i in $(seq 1 40); do
  miss=""; for c in $CODES; do [ -f $D/easyeda-cache/$c.json ] || miss="$miss $c"; done
  [ -z "$miss" ] && break
  echo "round $i missing:$miss"
  $P -I $D/ee_dump.py $S/lib $miss > /dev/null 2>&1
  sleep 150
done
for v in plus ultra; do c=/work/boards/core/fab/CPL.csv; pj=/work/boards/core/parts/parts.json; [ $v = ultra ] && c=/work/boards/core/fab/ultra/CPL.csv && pj=/work/boards/core/parts/parts-ultra.json
 $P $S/cpl_verify.py --pcb /work/boards/core/kicad/core.kicad_pcb --cpl $c --parts $pj --cache-dir $D/easyeda-cache --out $D/cpl_$v.json --offline > /dev/null 2>&1; done
echo FINISHED
