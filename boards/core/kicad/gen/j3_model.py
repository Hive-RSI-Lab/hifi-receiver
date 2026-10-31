"""J3 (FPC-05F-20PH20, the FFC to ultra-front): its 3D model sits 0.45 mm off
the pads (hwde check_model_fit: 12.9 of 22 pads covered as placed, 19.3
slid 0.45 mm). The library footprint and the board instance get the model
offset hwde names.  KiCad's python: kicad/gen/j3_model.py kicad/core.kicad_pcb"""
import sys
from pathlib import Path


lib = Path(__file__).resolve().parents[2] / "lib" / "aiee.pretty" / "FPC-SMD_20P-P0.50_XUNPU_FPC-05F-20PH20.kicad_mod"
t = lib.read_text()
lib.write_text(t.replace("(offset (xyz 0.000 0.000 0.000))", "(offset (xyz 0.000 0.450 0.000))", 1))
# the board instance, as text (pcbnew's Models() hands back copies)
pcb = Path(sys.argv[1])
s = pcb.read_text()
key = 'FPC-SMD_20P-P0.50_XUNPU_FPC-05F-20PH20.wrl"'
i = s.index(key)
j = s.index("(xyz 0 0 0)", i)
assert j - i < 200
pcb.write_text(s[:j] + "(xyz 0 0.45 0)" + s[j + len("(xyz 0 0 0)"):])
print("J3 model offset 0.45")
