"""R5-PA: the 5 A return from the amps (U15/U16 EP and GND pins, bulk caps'
GND) to J1's GND pins, all layers joined (power_path.py's method). Read-only;
run with $HWDE_PY. Prints the widest channel; exit 1 under 3.5 mm (IPC-2152
5 A at dT 10 C, 1 oz) with every layer counted."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'power_path.py')).read().split('\nhops = [')[0])
bad = 0
for name, a in (("U15 GND -> J1 GND", allcu("U15", "GND")), ("U16 GND -> J1 GND", allcu("U16", "GND")),
                ("C6 GND -> J1 GND", thru("C6", "GND"))):
    w = neck_layers("GND", (a, thru("J1", "GND")), CU)
    per = {l: neck_layers("GND", (a, thru("J1", "GND")), [l]) for l in CU}
    print(f"{name}: all layers {w:.2f} mm; single layers " + ", ".join(f"{l} {v:.2f}" for l, v in per.items()))
    if w < 3.5:
        bad = 1
sys.exit(bad)
