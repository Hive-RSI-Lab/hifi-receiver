"""What each tier should do, from docs/product.md ("Power per channel").

The table there fits P = (PVDD * R / (R + Rs)) ** 2 / (2 R), with the
output stage's on-resistance and filter loss as Rs in series: 0.3 ohm for
the TPA3118D2 at 20 V, 0.51 ohm for the TPA3220 at 28 V (the TPA3221's
output stage, pad down). The bench reports the measurement against both
the table and that model at the PVDD the board actually held, so a sagging
charger isn't mistaken for a weak amp.
"""
from __future__ import annotations

# board -> nominal PVDD, then load ohm -> watts per channel at 1% THD.
TIERS = {
    "lite-main": {"pvdd": 20.0, "rs": 0.3, "p1": {4: 43.0, 6: 30.0, 8: 23.0}, "amp": "TPA3118D2",
                  # the BTL output's peak volts for a 0 dBFS sine (docs/firmware.md, the governor)
                  "fs_vpk": 17.98},
    "core": {"pvdd": 28.0, "rs": 0.51, "p1": {4: 77.0, 6: 55.0, 8: 43.0}, "amp": "TPA3220",
             "fs_vpk": None},  # not set until the core firmware fixes its gain
}
# A measurement under this share of the prediction is a fail.
PASS_RATIO = 0.85


def model_watts(pvdd: float, ohm: float, rs: float = 0.3) -> float:
    v = pvdd * ohm / (ohm + rs)
    return v * v / (2 * ohm)


def predicted(board: str, ohm: float) -> float | None:
    t = TIERS.get(board)
    if not t:
        return None
    return t["p1"].get(int(ohm)) or model_watts(t["pvdd"], ohm, t["rs"])


def at_pvdd(board: str, pvdd: float, ohm: float) -> float | None:
    """The model at the PVDD the board actually held."""
    t = TIERS.get(board)
    return model_watts(pvdd, ohm, t["rs"]) if t else None
