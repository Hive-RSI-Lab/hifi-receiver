"""Write a session: session.json (everything), sweep.csv, soak.csv,
thd.svg (THD+N against power per load) and report.md, the results page."""
from __future__ import annotations

import csv
import json
import math
from pathlib import Path

COLS = ["#1f6feb", "#d1242f", "#1a7f37", "#8250df", "#bf8700", "#57606a"]


def write(out: Path, s: dict) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    (out / "session.json").write_text(json.dumps(s, indent=1, default=str) + "\n")
    pts = [p for sw in s.get("sweeps", []) for p in sw["points"]]
    if pts:
        with open(out / "sweep.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(pts[0]))
            w.writeheader()
            w.writerows(pts)
        (out / "thd.svg").write_text(svg(s["sweeps"]))
    if s.get("soak"):
        rows = s["soak"]["rows"]
        with open(out / "soak.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]))
            w.writeheader()
            w.writerows(rows)
    (out / "report.md").write_text(markdown(s))
    return out / "report.md"


def _f(v, fmt="{:.1f}"):
    return "–" if v is None else fmt.format(v)


def markdown(s: dict) -> str:
    i = s.get("info", {})
    L = [f"# Bench results: {i.get('board', s.get('board', '?'))} {i.get('mac', '')}", "",
         f"{s.get('started', '')} · firmware {i.get('fw_version', '?')} · via {s.get('via', '?')}"
         + (" · **MOCK, not a board**" if s.get("mock") else ""), ""]
    if s.get("verdicts"):
        L += ["| Item | Result |", "|---|---|"]
        L += [f"| {k} | **{v}** |" for k, v in s["verdicts"].items()]
        L.append("")
    for st in s.get("selftests", []):
        L += [f"## Self-test on {st.get('stage')}: {'pass' if st['pass'] else 'FAIL'}", "",
              f"Source {st.get('source')}, contract {st.get('contract_v')} V.", "",
              "| Item | Result | Value | Limit |", "|---|---|---|---|"]
        for it in st["items"]:
            r = "pass" if it["pass"] else ("expected here" if it.get("expected") else "FAIL")
            L.append(f"| {it['name']} | {r} | {it.get('value', '')} | {it.get('limit', '')} |")
        L.append("")
    if s.get("sweeps"):
        L += ["## Power at 1 kHz", "",
              "| Load | Charger contract | Driven | Governor | P at 1% THD+N | P at 10% | Predicted (1%) | Model at held PVDD | PVDD min | THD+N near 1 W | Stopped | Verdict |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for sw in s["sweeps"]:
            L.append(f"| {sw['ohm']:g} Ω | {sw.get('contract', '?')} | {sw.get('driven', 'both')} | {'on' if sw['governor'] else 'off'} | {_f(sw['p1_w'])} W | {_f(sw['p10_w'])} W | "
                     f"{_f(sw['predicted_w'])} W | {_f(sw['model_at_pvdd_w'])} W | {_f(sw['pvdd_min_v'], '{:.2f}')} V | "
                     f"{_f(sw['thdn_1w_pct'], '{:.3f}')}% | {sw['stop']} | {sw['verdict'] or ('engaged' if sw['gov_engaged'] else '–')} |")
        L += ["", "![THD+N against power](thd.svg)", "",
              "Power is per channel, from the fundamental's RMS volts across the dummy load. "
              "Driven 'L' means one channel: the charger can't feed both at the predicted power. "
              "The verdict is on the governor-off sweep: at least "
              f"{int(s.get('pass_ratio', 0.85) * 100)}% of the predicted 1% power. With the governor on, "
              "the sweep shows where the charger budget stops it.", ""]
    if s.get("soak"):
        k = s["soak"]
        L += [f"## Chip temperature at 1/8 power ({k['target_w']} W into {k['ohm']:g} Ω, {k['level_dbfs']} dBFS)", "",
              f"Probe: {k['probe']}. Start {_f(k.get('temp_start_c'))} °C, end {_f(k['temp_end_c'])} °C, max {_f(k['temp_max_c'])} °C after "
              f"{k['rows'][-1]['t_s'] if k['rows'] else 0:g} s; stopped: {k['stop']}. **{k['verdict']}**.", ""]
    if s.get("checklist"):
        L += ["## By ear and hand", "", "| Item | Result |", "|---|---|"]
        L += [f"| {k} | {v} |" for k, v in s["checklist"].items()]
        L.append("")
    if s.get("events"):
        L += ["## Events", ""] + [f"- {e}" for e in s["events"]] + [""]
    return "\n".join(L)


def svg(sweeps: list[dict], w: int = 640, h: int = 400) -> str:
    """Log-log THD+N (%) against power (W), one line per load and governor."""
    pad = 50
    pts = [(p["power_w"], p["thdn_pct"]) for sw in sweeps for p in sw["points"]
           if p["power_w"] > 0 and p["thdn_pct"] == p["thdn_pct"] and p["thdn_pct"] > 0]
    if not pts:
        return '<svg xmlns="http://www.w3.org/2000/svg"/>'
    x0, x1 = math.floor(math.log10(min(p for p, _ in pts))), math.ceil(math.log10(max(p for p, _ in pts)))
    y0, y1 = math.floor(math.log10(min(t for _, t in pts))), max(1, math.ceil(math.log10(max(t for _, t in pts))))
    x1, y1 = max(x1, x0 + 1), max(y1, y0 + 1)

    def X(v):
        return pad + (math.log10(v) - x0) / (x1 - x0) * (w - 2 * pad)

    def Y(v):
        return h - pad - (math.log10(v) - y0) / (y1 - y0) * (h - 2 * pad)

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" font-family="sans-serif" font-size="11">',
         f'<rect width="{w}" height="{h}" fill="white"/>']
    for d in range(x0, x1 + 1):
        x = X(10 ** d)
        o.append(f'<line x1="{x:.1f}" y1="{pad}" x2="{x:.1f}" y2="{h - pad}" stroke="#ddd"/>'
                 f'<text x="{x:.1f}" y="{h - pad + 15}" text-anchor="middle">{10 ** d:g}</text>')
    for d in range(y0, y1 + 1):
        y = Y(10 ** d)
        o.append(f'<line x1="{pad}" y1="{y:.1f}" x2="{w - pad}" y2="{y:.1f}" stroke="#ddd"/>'
                 f'<text x="{pad - 5}" y="{y + 4:.1f}" text-anchor="end">{10 ** d:g}</text>')
    o.append(f'<text x="{w / 2}" y="{h - 12}" text-anchor="middle">power per channel (W)</text>'
             f'<text x="14" y="{h / 2}" transform="rotate(-90 14 {h / 2})" text-anchor="middle">THD+N (%)</text>')
    for k, sw in enumerate(sweeps):
        c = COLS[k % len(COLS)]
        line = [(X(p["power_w"]), Y(p["thdn_pct"])) for p in sw["points"]
                if p["power_w"] > 0 and p["thdn_pct"] == p["thdn_pct"] and p["thdn_pct"] > 0]
        if line:
            dash = ' stroke-dasharray="4 3"' if sw["governor"] else ""
            xy = " ".join(f"{a:.1f},{b:.1f}" for a, b in line)
            o.append(f'<polyline fill="none" stroke="{c}" stroke-width="1.5"{dash} points="{xy}"/>')
        o.append(f'<text x="{pad + 8}" y="{pad + 14 + 14 * k}" fill="{c}">{sw["ohm"]:g} Ω, governor '
                 f'{"on" if sw["governor"] else "off"}</text>')
    o.append("</svg>")
    return "\n".join(o) + "\n"
