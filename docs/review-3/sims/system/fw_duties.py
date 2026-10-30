#!/usr/bin/env python3
"""R3-SYSTEM: are review-2's firmware duties (docs/review-2/firmware-requirements.md)
that bear on field use present in firmware/?  A source check, not a proof: each
rule looks for the code a duty needs and fails when none is there.  Exits 1 on
any FAIL.  Run: python3 -I docs/review-3/sims/system/fw_duties.py [repo]"""
import re
import sys
from pathlib import Path

R = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[4])
H = R / "firmware/components/hifi"


def src(p):
    return (H / p).read_text()


def func(text, name):
    m = re.search(r"\n[^\n]*\b%s\s*\([^)]*\)\s*\{" % re.escape(name), text)
    if not m:
        return ""
    i, depth = m.end(), 1
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.start():i]


gov, main, core, lite, pwr = (src("core/hifi_gov.c"), src("esp/hifi_main.c"), src("esp/board_core.c"),
                              src("esp/board_lite.c"), src("core/hifi_power.c"))
cfg_lite = func(gov, "hifi_gov_cfg_lite")
route_core = func(core, "hifi_bsp_route")
otw_uses = [l for l in core.splitlines() if "EXP_AMP_OTW" in l and "selftest" not in l and "otw =" not in l]

checks = [
    ("L1 Lite thermal governor (THERMAL-3): hifi_gov_cfg_lite sets thermal_out_w > 0",
     bool(re.search(r"\.thermal_out_w\s*=\s*[1-9]", cfg_lite))),
    ("L4 amp waits for the codec (AUDIO-17): power sequencer or amp enable gated on codec/VMID ready",
     bool(re.search(r"codec_(ready|ok|up)|vmid", pwr + func(main, "power_task") + func(lite, "hifi_bsp_power_out"), re.I))),
    ("C5 DJ_DIRECT change holds both amps in reset (AUDIO-9)",
     "EXP_AMP_A_RST" in route_core or "resync" in route_core),
    ("C1 PHY_RST (P05) low before a software restart (DIGITAL-1)",
     "esp_register_shutdown_handler" in core or bool(re.search(r"EXP_PHY_RST\s*,\s*false", core))),
    ("C2 OTW_CLIP acted on outside the self-test (-3 dB every 2 s)", bool(otw_uses)),
    ("input (DJ direct) survives a power blip: saved to NVS", bool(re.search(r'"hifi_input"', main))),
]
bad = 0
for name, ok in checks:
    print(("PASS " if ok else "FAIL ") + name)
    bad += not ok
print(f"{bad} duty(ies) missing")
sys.exit(1 if bad else 0)
