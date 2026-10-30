"""Run a netlist text through hwde's in-process ngspice (simlib); returns measures and vectors."""
import os, sys
from pathlib import Path
sys.path.insert(0, os.path.join(os.environ["HWDE_SKILL"], "scripts", "lib"))
import simlib  # noqa: E402
simlib.setup_engine_env(Path(os.environ["HWDE_NGSPICE_DLL"]))
def meas(text):
    r = simlib.run_circuit(simlib.prepare_circuit(text))
    if r["status"] != "ok":
        raise RuntimeError(r.get("error"))
    return r["measures"]
def vectors(text):
    r = simlib.run_vectors([simlib.prepare_circuit(text)])
    if r["status"] != "ok":
        raise RuntimeError(r.get("error"))
    return r["plots"][0]
