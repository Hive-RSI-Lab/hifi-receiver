"""Run every tb_*.cir in a directory through hwde sim_run.py and print each bench's
measures against its bounds. Exit 1 if any error-severity bound fails (warnings are
printed as WARN, exit 0). usage: run_benches.py DIR   (run under `heavy --quick`,
with `eval "$(~/modules/bin/hwde env)"` done so HWDE_PY / HWDE_SKILL are set)."""
import json, os, subprocess, sys, tempfile
d = sys.argv[1]
py, sk = os.environ["HWDE_PY"], os.environ["HWDE_SKILL"]
out = os.path.join(tempfile.mkdtemp(dir=os.environ.get("TMPDIR")), "r.json")
p = subprocess.run([py, f"{sk}/scripts/sim_run.py", "--dir", d, "--out", out], capture_output=True, text=True)
if not os.path.exists(out):
    print("sim_run failed:", p.stdout[-500:], p.stderr[-500:]); sys.exit(2)
r = json.load(open(out)); bad = 0
for tb in r["testbenches"]:
    bounds = json.load(open(os.path.join(d, tb["name"] + ".bounds.json")))
    if isinstance(bounds, dict): bounds = bounds["bounds"]
    print(f"== {tb['name']}")
    for b in bounds:
        v = tb["measures"].get(b["measure"])
        if v is None:
            print(f"  {b['measure']:12s} MISSING"); bad = 1; continue
        lo, hi = b.get("min"), b.get("max")
        ok = (lo is None or v >= lo) and (hi is None or v <= hi)
        lim = (f">= {lo}" if lo is not None else f"<= {hi}")
        mg = (v - lo) if lo is not None else (hi - v)
        tag = "ok  " if ok else ("FAIL" if b["severity"] == "error" else "WARN")
        if not ok and b["severity"] == "error": bad = 1
        print(f"  {tag} {b['measure']:12s} {v:9.4f} V  limit {lim:8s} margin {mg:+.3f} V  [{b['severity']}]")
print("RESULT:", "FAIL" if bad else "PASS (error bounds)")
sys.exit(bad)
