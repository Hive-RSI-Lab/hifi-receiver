"""vals.py: do the SI deck constants still match the current boards? Reads
boards/*/kicad/*.net and *.kicad_pcb (read-only), recomputes routed lengths (track graph,
vias join layers), widths, layers, series-R values and driver/receiver parts, compares with
the numbers hard-coded in the decks in this directory. Exit 1 on any mismatch (length
tolerance max(0.5 mm, 2 %)). Run: heavy --quick $HWDE_PY vals.py"""
import heapq, math, re, sys
from pathlib import Path
R = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(R / "docs/review-3/sims/layout"))
from pcbgeo import load_board

def netinfo(f):
    t = (R / f"boards/{f}/kicad/{f}.net").read_text()
    val = dict(re.findall(r'\(comp\s+\(ref "([^"]+)"\)\s+\(value "([^"]*)"\)', t))
    nets = {m.group(1): re.findall(r'\(ref "(\w+)"\)\s+\(pin "(\w+)"', m.group(2))
            for m in re.finditer(r'\(net\s+\(code "\d+"\)\s+\(name "([^"]+)"\)(.*?)\n\t\t\)', t, re.S)}
    return val, nets

def graph(b, net):
    key = lambda p: (round(p[0], 2), round(p[1], 2)); G = {}
    def edge(a, c, w): G.setdefault(a, []).append((c, w)); G.setdefault(c, []).append((a, w))
    pts = set()
    for t in b["tracks"]:
        if t["net"] == net:
            a, c = key(t["s"]), key(t["e"]); edge(a, c, math.dist(t["s"], t["e"])); pts |= {a, c}
    for v in b["vias"]:
        if v.get("net") == net: pts.add(key(v["xy"]))
    pads = {}
    for r, f in b["footprints"].items():
        for p in f["pads"]:
            if p["net"] == net:
                pk = ("pad", r, p["num"]); pads[(r, p["num"])] = pk
                for q in pts:
                    d = math.dist(q, (p["x"], p["y"]))
                    if d < 0.6: edge(pk, q, d)
    L = sorted(pts)
    for i, a in enumerate(L):
        for c in L[i + 1:]:
            if c[0] - a[0] > 0.05: break
            if math.dist(a, c) < 0.05: edge(a, c, 0)
    return G, pads

def plen(b, net, s, d):
    G, pads = graph(b, net); S = pads[s]; D = {S: 0}; h = [(0, S)]
    while h:
        x, u = heapq.heappop(h)
        if x > D.get(u, 1e9): continue
        for v, w in G.get(u, []):
            if x + w < D.get(v, 1e9): D[v] = x + w; heapq.heappush(h, (x + w, v))
    return D.get(pads[d], float("nan"))

def props(b, net):
    ts = [t for t in b["tracks"] if t["net"] == net]
    return {t.get("w", t.get("width")) for t in ts}, {t["layer"] for t in ts}

bad = []
def eq(what, got, deck, tol=None):
    tol = tol if tol is not None else max(0.5, 0.02 * abs(deck))
    ok = abs(got - deck) <= tol
    print(f"{'ok  ' if ok else 'FAIL'} {what}: board {got:.2f} deck {deck} (tol {tol:.2f})")
    if not ok: bad.append(what)
def same(what, got, deck):
    ok = got == deck; print(f"{'ok  ' if ok else 'FAIL'} {what}: board {got} deck {deck}")
    if not ok: bad.append(what)

# ---- core
cv, cn = netinfo("core"); cb = load_board(str(R / "boards/core/kicad/core.kicad_pcb"))
print("== core ETH_CLK (core_ethclk/gen.py: 19.0 / 2.6 / 36.5 / 56.6, R60 33R, R31 10k)")
eq("Y2.3 -> R60.1 (ETH_OSC)", plen(cb, "/ethernet/ETH_OSC", ("Y2", "3"), ("R60", "1")), 19.0)
eq("R60.2 -> U9.5 (=2.6+36.5)", plen(cb, "/ETH_CLK", ("R60", "2"), ("U9", "5")), 39.1)
eq("R60.2 -> U7.25 (=2.6+56.6)", plen(cb, "/ETH_CLK", ("R60", "2"), ("U7", "25")), 59.2)
for n in ("/ethernet/ETH_OSC", "/ETH_CLK"):
    w, l = props(cb, n); same(f"{n} width (+-0.01)", all(abs(x-0.2)<=0.01 for x in w), True); same(f"{n} layers outer only", l <= {"F.Cu", "B.Cu"}, True)
same("R60 value", cv["R60"], "33R"); same("R31 value", cv["R31"], "10k")
same("ETH_CLK driver Y2 / receivers", (cv["Y2"], cv["U9"], cv["U7"]), ("50MHz", "LAN8720A", "ESP32-WROVER-E-N8R8"))
print("== core I2S_BCK (core_i2sbck/gen.py: 61.9 / 1.5 stub / 27.7 / 4.9 / 8.9, no series R)")
# deck junction X1 splits: U7 -> X1 61.9; X1 -> U13 stub 1.5 ; X1 -> X2 27.7; X2 -> U12 4.9; X2 -> U10 8.9
eq("U7.24 -> U13.6 (61.9+1.5)", plen(cb, "/I2S_BCK", ("U7", "24"), ("U13", "6")), 63.4)
eq("U7.24 -> U12.13 (61.9+27.7+4.9)", plen(cb, "/I2S_BCK", ("U7", "24"), ("U12", "13")), 94.5)
eq("U7.24 -> U10.27 (61.9+27.7+8.9)", plen(cb, "/I2S_BCK", ("U7", "24"), ("U10", "27")), 98.5)
w, l = props(cb, "/I2S_BCK"); same("/I2S_BCK width (+-0.01; 0.205 seen)", all(abs(x-0.2)<=0.01 for x in w), True); same("/I2S_BCK no series R (only 4 pins)", sorted(r for r, p in cn["/I2S_BCK"]), ["U10", "U12", "U13", "U7"])
print("== core UF SCK (uf_sck/tb_sck_asbuilt.cir: 49.2 / 33R / 57.8)")
eq("U7.8 -> R402.1 /LCD_SCLK", plen(cb, "/LCD_SCLK", ("U7", "8"), ("R402", "1")), 49.2)
eq("R402.2 -> J3.5 /ui/FFC_SCK", plen(cb, "/ui/FFC_SCK", ("R402", "2"), ("J3", "5")), 57.8)
same("R402 value", cv["R402"], "33R")
# ---- ultra-front
uv, un = netinfo("ultra-front"); ub = load_board(str(R / "boards/ultra-front/kicad/ultra-front.kicad_pcb"))
print("== ultra-front LCD_SCK front side (deck 20.4 mm J1.5 -> J2.4)")
eq("J1.5 -> J2.4", plen(ub, "/LCD_SCK", ("J1", "5"), ("J2", "4")), 20.4)
same("front J1 part", uv["J1"], "FPC-05F-20PH20")
# ---- lite
lv, ln = netinfo("lite-main"); lb = load_board(str(R / "boards/lite-main/kicad/lite-main.kicad_pcb"))
print("== lite-main I2S_BCLK (lite_i2sbck/gen.py: 45.6 mm, 0.2 mm, no series R)")
eq("U5.12 -> U7.5", plen(lb, "/I2S_BCLK", ("U5", "12"), ("U7", "5")), 45.6)
w, l = props(lb, "/I2S_BCLK"); same("width (+-0.01)", all(abs(x-0.2)<=0.01 for x in w), True); same("pins on net (no series R)", sorted(r for r, p in ln["/I2S_BCLK"]), ["U5", "U7"])
same("parts", (lv["U5"], lv["U7"]), ("ESP32-WROVER-E-N8R8", "ES8388"))
print("== PVDD decoupling parts (core_pvdd / lite_pvdd decks)")
for r in ("C207", "C209", "C307", "C309"): print("core", r, cv.get(r))
for r in ("U8",): print("lite", r, lv.get(r))
print("RESULT:", "FAIL " + "; ".join(bad) if bad else "PASS (all deck constants match the boards)")
sys.exit(1 if bad else 0)
