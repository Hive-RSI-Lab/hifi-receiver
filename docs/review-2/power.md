# Power review (review-2, domain `power`) - Lite (lite-main) and core (Plus/Ultra)

Reviewer: independent power-electronics pass, 2026-10-28. Read-only on `boards/`.
Sources: netlists `boards/*/kicad/*.net` (parsed, not the generator comments), vendor datasheets
saved under `/tmp/ds/power/<part>/`, ngspice testbenches in `docs/review-2/sims/power/`
(run with `$HWDE_PY docs/review-2/sims/power/run.py <tb>.cir`; models in `models.inc`).

STATUS: work in progress - written block by block.

## Findings so far (details per block below)

| id | board | sev | one line |
|---|---|---|---|
| POWER-1 | lite-main | major | Q1 Miller cap C2 (gate-drain) turns Q1 ON on every VBUS rising edge with firmware off: PVDD charges to 2.8 V on attach and to 10-15.6 V on the 5->20 V PD step; inrush 20.6 A at the PD max slew (30 V/ms) |
| POWER-2 | core | major | TCA9555 internal ~100k pull-ups vs R40-R45 100k pull-downs: PVDD_EN, AMP_x_RST, DAC_EN, PHY_RST, DJ_DIRECT sit at ~1.3-2.1 V from power-up until firmware writes the expander; Q2 2N7002 (Vth 1.0-2.5 V) can turn PVDD on |

(table continues as blocks are written)
