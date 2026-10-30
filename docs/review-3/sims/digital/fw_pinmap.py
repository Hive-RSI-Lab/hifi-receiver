"""Firmware pin map vs board netlist (review-3 DIGITAL).

Reads the GPIO numbers the firmware uses (firmware/components/hifi/esp/
hifi_board_{lite,core}.h and the Kconfig defaults in firmware/patches/*.patch:
DAC_CONFIG, SET_GPIO, AUDIO_CONTROLS, I2C_CONFIG, ETH_CONFIG), maps each GPIO
to its WROVER-E module pin, and checks the netlist puts the expected net
there. Also checks the expander bit map against U17's pins, the input-only
rule (GPIO34-39 never outputs), GPIO6-11/16/17 unused, ADC channels on ADC1,
and the ETH PHY address squeezelite hard-codes against the LAN8720A strap.
Exit 1 on any mismatch.
"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import netlib
ROOT = os.path.abspath(os.path.join(HERE, '../../../..'))
FW = f"{ROOT}/firmware"

MOD = {36: '4', 39: '5', 34: '6', 35: '7', 32: '8', 33: '9', 25: '10', 26: '11', 27: '12',
       14: '13', 12: '14', 13: '16', 15: '23', 2: '24', 0: '25', 4: '26', 16: '27', 17: '28',
       5: '29', 18: '30', 19: '31', 21: '33', 3: '34', 1: '35', 22: '36', 23: '37'}
ADC1 = {36, 37, 38, 39, 32, 33, 34, 35}
ADC1_CH = {0: 36, 3: 39, 4: 32, 5: 33, 6: 34, 7: 35}
fails = []

def chk(cond, msg):
    print(('PASS ' if cond else 'FAIL ') + msg)
    if not cond:
        fails.append(msg)

def defines(path):
    out = {}
    for m in re.finditer(r'^#define\s+(\w+)\s+([^\s/]+)', open(path).read(), re.M):
        out[m.group(1)] = m.group(2)
    return out

patch = open(f"{FW}/patches/0001-hifi-receiver-hooks.patch").read()

KIND = {'DAC_CONFIG': r'model=', 'ETH_CONFIG': r'model=LAN8720', 'SET_GPIO': r'\d+=ir',
        'I2C_CONFIG': r'sda='}

def kdefault(key, cond):
    """The patch's added Kconfig default for KEY under CONFIG cond (matched by the string's shape)."""
    for m in re.finditer(r'^\+\s*default "((?:[^"\\]|\\.)*)" if ([^\n]*)', patch, re.M):
        val, conds = m.group(1), m.group(2)
        if re.search(r'\b' + cond + r'\b', conds) and re.match(KIND[key], val) and not val.startswith('{'):
            return val.replace('\\"', '"')
    return ''

def board(name, net, esp, hdr, cond, outputs, inputs, extra):
    comps, nets = netlib.load(net)
    pn = netlib.pin_net(nets)
    def at(g):
        return netlib.short(pn.get((esp, MOD[g]), '?'))
    for g, want, dirn in outputs + inputs:
        got = at(g)
        chk(got == want, f"{name} GPIO{g} ({dirn}) -> net {got}, firmware expects {want}")
        if dirn == 'out':
            chk(g < 34, f"{name} GPIO{g} driven as an output is not input-only (34-39)")
    for g in list(MOD):
        if g in (16, 17):
            chk(at(g).startswith('unconnected'), f"{name} GPIO{g} (PSRAM on WROVER-E) left unconnected")
    extra(comps, nets, pn, at)

# ---------------------------------------------------------------- lite-main
L = defines(f"{FW}/components/hifi/esp/hifi_board_lite.h")
dac = kdefault('DAC_CONFIG', 'HIFI_LITE')
d = dict(kv.split('=') for kv in dac.split(',') if '=' in kv)
setg = kdefault('SET_GPIO', 'HIFI_LITE')
sg = dict(kv.split('=', 1) for kv in setg.split(','))
ctrls = re.search(r'default "(\[\{\\"gpio\\":13.*?\])" if HIFI_LITE', patch).group(1).replace('\\"', '"')
btn = {int(m.group(1)): m.group(2) for m in re.finditer(r'"gpio":(\d+).*?"pressed":"(ACTRLS_\w+)"', ctrls)}
lite_out = [(int(L['PIN_PVDD_EN']), 'PVDD_EN', 'out'), (int(L['PIN_AMP_SDZ']), 'AMP_SDZ_IO', 'out'),
            (int(L['I2C_SCL']), 'I2C_SCL', 'out'), (int(L['I2C_SDA']), 'I2C_SDA', 'out'),
            (int(d['bck']), 'I2S_BCLK', 'out'), (int(d['ws']), 'I2S_LRCK', 'out'),
            (int(d['do']), 'I2S_DOUT', 'out'), (int(d['mck']), 'I2S_MCLK', 'out'),
            ([int(k) for k, v in sg.items() if 'green' in v][0], 'LED_DRV', 'out')]
lite_in = [(int(L['PIN_PD_PG']), 'PD_PG', 'in'), (int(L['PIN_AMP_FAULTZ']), 'AMP_FAULTZ', 'in'),
           (int(d['di']), 'I2S_DIN', 'in'), ([int(k) for k, v in sg.items() if 'ir' in v][0], 'IR_IN', 'in'),
           (ADC1_CH[int(L['ADC_VBUS'].split('_')[-1])], 'VBUS_SENSE', 'in'),
           (ADC1_CH[int(L['ADC_PVDD'].split('_')[-1])], 'PVDD_SENSE', 'in')]
for g, a in btn.items():
    lite_in.append((g, {'ACTRLS_VOLUP': 'BTN_VOLUP', 'ACTRLS_VOLDOWN': 'BTN_VOLDN', 'ACTRLS_TOGGLE': 'BTN_PLAY'}[a], 'in'))

def lite_extra(comps, nets, pn, at):
    chk(int(d['i2c']) == int(L['ES8388_ADDR'], 16) == 0x10, f"lite ES8388 I2C address firmware {d['i2c']} = 0x10 (CE pin 26 on {netlib.short(pn[('U7','26')])})")
    chk(netlib.short(pn[('U7', '26')]) == 'GND', "lite ES8388 CE on GND -> 0x10")
    chk(int(d['mck']) in (0, 1, 3), "lite MCLK on GPIO0/1/3 (CLK_OUT capable)")
    # line-in divider the firmware assumes vs the board
    r30 = comps['R30']['value']; r31 = comps['R31']['value']
    ratio = (float(r30.rstrip('k')) + float(r31.rstrip('k'))) / float(r31.rstrip('k'))
    chk(abs(ratio - float(L['LINE_DIVIDER'].rstrip('f'))) < 0.05, f"lite LINE_DIVIDER {L['LINE_DIVIDER']} vs board R30/R31 {r30}/{r31} = {ratio:.2f}")
    has_taps = 'R66' in comps and netlib.short(pn[('R66', '1')]).startswith('SPK_')
    chk(not has_taps or L.get('HIFI_HAS_TAPS') == '1', f"lite board has speaker taps (R66-R69): firmware HIFI_HAS_TAPS={L.get('HIFI_HAS_TAPS')}")

board('lite', f"{ROOT}/boards/lite-main/kicad/lite-main.net", 'U5', L, 'HIFI_LITE', lite_out, lite_in, lite_extra)

# ---------------------------------------------------------------- core
C = defines(f"{FW}/components/hifi/esp/hifi_board_core.h")
dacc = dict(kv.split('=') for kv in kdefault('DAC_CONFIG', 'HIFI_PLUS').split(',') if '=' in kv)
eth = dict(kv.split('=') for kv in kdefault('ETH_CONFIG', 'HIFI_PLUS').split(',') if '=' in kv)
setc = dict(kv.split('=', 1) for kv in kdefault('SET_GPIO', 'HIFI_PLUS').split(','))
i2cc = dict(kv.split('=') for kv in kdefault('I2C_CONFIG', 'HIFI_PLUS').split(','))
core_out = [(int(C['I2C_SCL']), 'I2C_SCL', 'out'), (int(C['I2C_SDA']), 'I2C_SDA', 'out'),
            (int(i2cc['scl']), 'I2C_SCL', 'out'), (int(i2cc['sda']), 'I2C_SDA', 'out'),
            (int(C['PIN_I2S_BCK']), 'I2S_BCK', 'out'), (int(dacc['bck']), 'I2S_BCK', 'out'),
            (int(C['PIN_I2S_LRCK']), 'I2S_LRCK', 'out'), (int(dacc['ws']), 'I2S_LRCK', 'out'),
            (int(dacc['do']), 'I2S_DOUT_MAIN', 'out'), (int(C['PIN_I2S_DOUT_SUB']), 'I2S_DOUT_SUB', 'out'),
            (int(eth['mdc']), 'MDC', 'out'), (int(C['PIN_MDC']), 'MDC', 'out'), (int(eth['mdio']), 'MDIO', 'out'),
            (int(C['PIN_RMII_TXD0']), 'RMII_TXD0', 'out'), (int(C['PIN_RMII_TXD1']), 'RMII_TXD1', 'out'),
            (int(C['PIN_RMII_TX_EN']), 'RMII_TX_EN', 'out')]
core_in = [(int(dacc['di']), 'I2S_DIN', 'in'), (int(C['PIN_EXP_INT']), 'EXP_INT', 'in'),
           (ADC1_CH[int(C['ADC_ISENSE'].split('_')[-1])], 'ISENSE', 'in'),
           ([int(k) for k, v in setc.items() if 'ir' in v][0], 'IR_IN', 'in'),
           (int(C['PIN_ETH_CLK']), 'ETH_CLK', 'in'), (int(C['PIN_RMII_RXD0']), 'RMII_RXD0', 'in'),
           (int(C['PIN_RMII_RXD1']), 'RMII_RXD1', 'in'), (int(C['PIN_RMII_CRS_DV']), 'RMII_CRS_DV', 'in')]
sq_lan = os.environ.get('SQZ_LAN8720_C')   # optional: path to squeezelite's network_driver_LAN8720.c

def core_extra(comps, nets, pn, at):
    bits = {'EXP_AMP_A_RST': 'AMP_A_RST', 'EXP_AMP_B_RST': 'AMP_B_RST', 'EXP_DJ_DIRECT': 'DJ_DIRECT',
            'EXP_PVDD_EN': 'PVDD_EN', 'EXP_DAC_EN': 'DAC_EN', 'EXP_PHY_RST': 'PHY_RST',
            'EXP_LCD_BL': 'LCD_BL', 'EXP_LED': 'LED_STATUS', 'EXP_AMP_FAULT': 'AMP_FAULT',
            'EXP_AMP_OTW': 'AMP_OTW', 'EXP_ENC_A': 'ENC_A', 'EXP_ENC_B': 'ENC_B',
            'EXP_ENC_SW': 'ENC_SW', 'EXP_BTN_1': 'BTN_1', 'EXP_BTN_2': 'BTN_2', 'EXP_TAP_SEL': 'TAP_SEL_IO'}
    tca_pin = {b: str(4 + b) if b < 8 else str(13 + b - 8) for b in range(16)}  # P00..P07 = 4..11, P10..P17 = 13..20
    used = set()
    for k, net in bits.items():
        b = int(C[k]); used.add(b)
        got = netlib.short(pn[('U17', tca_pin[b])])
        chk(got == net, f"core expander bit {b} ({k}) -> U17 pin {tca_pin[b]} net {got}, firmware expects {net}")
    for b in range(16):
        if b not in used:
            got = netlib.short(pn[('U17', tca_pin[b])])
            cfg = int(C['EXP_CFG1'], 16) if b >= 8 else int(C['EXP_CFG0'], 16)
            is_in = bool(cfg >> (b % 8) & 1)
            chk(not got.endswith('_IO') or not is_in,
                f"core expander bit {b} (P1{b-8}) is net {got} on the board; firmware has no define for it and leaves it {'an input' if is_in else 'an output'}")
    chk(at(0) == 'ETH_CLK', 'core GPIO0 is the RMII clock input (CONFIG_ETH_RMII_CLK_IN_GPIO=0)')
    strap = netlib.short(pn[('U9', '10')])
    phyad = 1 if strap == '+3V3' else 0   # RXER/PHYAD0 has an internal pull-down (DS00002165C table 3-x, PD)
    want = None
    if sq_lan and os.path.exists(sq_lan):
        m = re.search(r'phy_addr\s*=\s*(-?\d+)', open(sq_lan).read())
        want = int(m.group(1)) if m else None
    else:
        want = 1   # squeezelite-esp32 ee1720c network_driver_LAN8720.c: phy_config.phy_addr = 1;
    # our patches to upstream win over upstream's own value
    import glob
    for pf in sorted(glob.glob(f"{ROOT}/firmware/patches/*.patch")):
        m = re.search(r'^\+\s*phy_config\.phy_addr\s*=\s*(-?\d+)', open(pf).read(), re.M)
        if m:
            want = int(m.group(1))
    chk(want in (-1, phyad), f"core LAN8720A PHYAD0 strap -> address {phyad} (pin 10 net {strap}); squeezelite queries address {want}")

# Ultra's LCD on the core's ESP32 (through J3 to the front board): squeezelite's SPI_CONFIG
ucfg = open(f"{FW}/board/ultra/sdkconfig.defaults").read()
spi = re.search(r'^CONFIG_SPI_CONFIG="([^"]*)"', ucfg, re.M).group(1)
spi = dict(kv.split('=') for kv in spi.split(',') if '=' in kv)
core_out += [(int(spi.get('data', spi.get('mosi', -1))), 'LCD_MOSI', 'out'), (int(spi.get('clk', -1)), 'LCD_SCLK', 'out'),
             (int(spi.get('dc', -1)), 'LCD_DC', 'out')]
chk(int(C['EXP_CFG1'], 16) >> 7 & 1 == 0, "core expander P17 (TAP_SEL) is an output (C6)")

board('core', f"{ROOT}/boards/core/kicad/core.net", 'U7', C, 'HIFI_PLUS', core_out, core_in, core_extra)
print(f"\n{len(fails)} mismatch(es)")
sys.exit(1 if fails else 0)
