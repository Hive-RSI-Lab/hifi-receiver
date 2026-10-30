"""Run one testbench through hwde's simlib and print its measures (review-2 power)."""
import json, os, sys
from pathlib import Path
sys.path.insert(0, os.environ["HWDE_SKILL"] + "/scripts"); sys.path.insert(0, os.environ["HWDE_SKILL"] + "/scripts/lib")
import simlib
simlib.setup_engine_env(Path(os.environ["HWDE_NGSPICE_DLL"]))
cir = Path(sys.argv[1])
txt = cir.read_text()
os.chdir(cir.parent)
r = simlib.run_circuit(simlib.prepare_circuit(txt))
if r["status"] != "ok":
    print(r); sys.exit(1)
for k, v in r["measures"].items():
    print(f"{k:24s} {v: .5g}")
if r["failed_measures"]:
    print("FAILED:", r["failed_measures"])
