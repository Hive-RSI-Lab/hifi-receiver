"""ESP32 strapping-pin check for lite-main and core (review-3 DIGITAL).

For each strap (GPIO0, GPIO2, MTDI/GPIO12, MTDO/GPIO15, GPIO5) it resolves,
from the board's .net:
  - external pull resistors to +3V3/GND, and the ESP32's own pull (datasheet
    v5.3 table 3-1: GPIO0 up, GPIO2 down, MTDI down, MTDO up, GPIO5 up,
    ~45 k);
  - every other pin on the net, checked against a list of parts known to be
    hi-Z (inputs, no pull) at reset;
  - any oscillator that can drive the net through a series resistor, and
    what holds that oscillator's OE.
It evaluates three events:
  por     power-on: every chip is reset, expander outputs are inputs
  en      EN pulled low (RC, auto-program, supervisor): the TCA9539, whose
          RESET is on ESP_EN, is reset with the ESP32
  sysrst  ESP32-internal system reset (brownout reset, RTC watchdog):
          ESP32 datasheet v5.3 section 3 'At system reset, the latches sample
          the strapping pins'; EN does not move, so an expander output that
          firmware set stays set.
Fails (exit 1) when a strap can read the wrong value in any event.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import netlib
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))

STRAPS = {  # module pin -> (gpio, internal pull, wanted value or None)
    '25': ('GPIO0', 'up', 1),     # 1 = SPI boot
    '24': ('GPIO2', 'down', 0),   # must be 0 or floating for download; any for SPI boot
    '14': ('MTDI/GPIO12', 'down', 0),  # 0 = VDD_SDIO 3.3 V: WROVER-E's flash/PSRAM are 3.3 V
    '23': ('MTDO/GPIO15', 'up', None),
    '29': ('GPIO5', 'up', None),
}
# parts whose pins on these nets are inputs without pulls at reset (datasheets)
HIZ = {'ES8388': 'MCLK/SCLK input', 'PCM5102A': 'DIN/BCK input', 'ES9018K2M': 'DATA input',
       'ES7243E': 'SCLK input', 'LAN8720A': 'CLKIN input', 'MMBT3904': 'collector, off at reset',
       'S8050': 'collector, off at reset', 'FFC 20P': 'connector (front LCD DC: input)'}

def analyse(board, net_path, esp_ref, expander_reset_net=None):
    comps, nets = netlib.load(net_path)
    pn = netlib.pin_net(nets)
    fails = []
    for pin, (gpio, ipull, want) in STRAPS.items():
        net = pn.get((esp_ref, pin))
        nodes = [n for n in nets.get(net, []) if n[0] != esp_ref]
        ups, downs, osc, unknown = [], [], [], []
        for ref, p, fn, ty in nodes:
            val = comps[ref]['value']
            if ref.startswith('R'):
                other = [pn[(ref, q)] for q in ('1', '2') if (ref, q) in pn and pn[(ref, q)] != net]
                o = netlib.short(other[0]) if other else None
                if o in ('+3V3',):
                    ups.append(f"{ref} {val}")
                elif o == 'GND':
                    downs.append(f"{ref} {val}")
                else:
                    # series resistor: look for an oscillator output behind it
                    for r2, p2, f2, t2 in nets.get(other[0], []) if other else []:
                        if r2.startswith('Y') and 'OUT' in f2.upper():
                            oe = pn.get((r2, '1'), '')  # pin 1 = OE / tri-state on 3225 oscillators
                            osc.append((r2, comps[r2]['value'], ref, netlib.short(oe)))
            elif not any(k in (val or '') for k in HIZ):
                unknown.append(f"{ref}.{p} {val}")
        # resolve per event
        for ev in ('por', 'en', 'sysrst'):
            clock = None
            for y, yv, rs, oe in osc:
                oe_nodes = nets.get([k for k in nets if netlib.short(k) == oe][0], [])
                exp = [r for r, p, f, t in oe_nodes if comps[r]['value'] in ('TCA9539', 'TCA9555', 'TCA9535')]
                if ev == 'sysrst' and exp:
                    clock = f"{y} {yv} via {rs}: OE={oe} is held by {exp[0]} (RESET on {expander_reset_net}, not moved by an internal reset)"
            if clock:
                lvl = 'clock'
            elif ups and not downs:
                lvl = 1
            elif downs and not ups:
                lvl = 0
            elif not ups and not downs:
                lvl = 1 if ipull == 'up' else 0
            else:
                lvl = 'fight'
            ok = want is None or lvl == want
            line = f"{board} {gpio:12s} {ev:6s} net={netlib.short(net)} up={ups} down={downs} -> {lvl}" + (f" [{clock}]" if clock else '')
            print(('PASS ' if ok else 'FAIL ') + line)
            if not ok:
                fails.append(line)
        if unknown:
            print(f"     {board} {gpio}: parts with unlisted reset state: {unknown}")
    return fails

fails = []
fails += analyse('lite-main', f"{ROOT}/boards/lite-main/kicad/lite-main.net", 'U5')
fails += analyse('core', f"{ROOT}/boards/core/kicad/core.net", 'U7', 'ESP_EN')
print(f"\n{len(fails)} strap failure(s)")
sys.exit(1 if fails else 0)
