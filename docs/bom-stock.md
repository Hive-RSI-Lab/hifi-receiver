# BOM stock: every board against live JLC and LCSC stock

Run this before every JLC order:

    python3 tools/bom_stock.py                # every board, for 100 boards each
    python3 tools/bom_stock.py --boards 10 lite-main
    python3 tools/bom_stock.py --json out.json

It reads every `boards/*/fab/BOM.csv` (and `boards/core/fab/ultra/BOM.csv`) and
looks up each LCSC part at JLC's assembly parts search and at LCSC. It exits 1
if any line is short. It needs the network, so it isn't part of
`tools/hwcheck.py test`. Once hive-hwde's stock gate lands (hive-hwde task
261029-022243-a4fd3c0edf), this check belongs there, run on every board as a
gate. Until then, this script is the check.

## How to read it

- **JLC free** is what an assembly order is placed from. That is JLC's
  `canPresaleNumber`: its stock less what open orders have already claimed.
  JLC's raw `stockCount` overstates it. J1's USB4105-GF-A-120 read 2230 in
  stock with 372 more claimed than that, and JLC's BOM review showed it as 0.
- **LCSC** is what can be bought into a JLC parts inventory before ordering.
  Basic parts read 0 at LCSC because JLC stocks them itself, so 0 there is
  not a problem.
- A part JLC marks not buyable (`isBuyComponent=0`) counts as short
  whatever its stock. JLC would swap in its own pick at order time.
- **ok** means JLC has enough free. **thin** means JLC is short but LCSC has
  enough: buy the part into the inventory first. **short** means neither
  has enough. Use an alternate from
  [bom-alternates.csv](bom-alternates.csv), or fit the part by hand.

Alternates in `bom-alternates.csv` are checked by hand against the part's own
datasheet: its rating and its land pattern. The listing's attributes aren't
enough on their own: LCSC lists HRO TYPE-C-31-M-22 at 100 V and GCT USB4110
at 48 V, but both datasheets rate them 20 V. Each alternate is listed for the
boards it suits, and the script prints its live stock beside the short line.

## 2026-10-29 03:37Z, for 100 boards each

192 lines on lite-main, core (Plus) and core/ultra: 186 ok, 6 short: J1 and
the CH340C on each of the three boards.

**J1 USB4105-GF-A-120 (C5184243): short on every board.** JLC has 0 free
and LCSC has 0. The same part with other shell stakes is out too:
USB4105-GF-A (C3020560) and USB4105-GF-A-060 (C3025063).

- **Lite (`lite-main`), 20 V:** use **HRO TYPE-C-31-M-12, C165948**. It is a
  drop-in on the USB4105 pads, rated 20 V / 5 A, with 280k free. Lite only
  asks its charger for 20 V, so the connector's rating covers it.
- **Plus and Ultra (`core`), 28 V EPR:** no in-stock part fits the USB4105
  pads and is rated for 28 V. EPR needs a 48–50 V receptacle.
  [product.md](product.md) rules out TYPE-C-31-M-12 at 28 V. Checked and
  rejected:

  | Part | Why not |
  |---|---|
  | HRO TYPE-C-31-M-22, C530612 | rated 20 V (the 100 V listed is its withstand voltage); shell tabs placed differently |
  | GCT USB4110-GF-A, C5143397 | rated 20 V DC in its datasheet |
  | JAE DX07S016JA1R1500, C3197885 | different land pattern (0.27 mm pads, Ø0.6 pegs 6.0 mm apart, slots 3.1 mm apart); VBUS rated 1.25 A |
  | Amphenol GSB1C41110SSHR, C34260841 | SMD shell pads (10.24 mm span) where the boards have through-hole slots; no voltage rating given |
  | Hirose CX90BW-16P(001), Würth 632723130112 | 48 V, but 19 and 3 free |
  | HRO TYPE-C-31-M-30, Molex 2012670005 | 24-pin footprint, 30 V |

  So for now, J1 on core stays off the JLC order and is fitted by hand
  (USB4105 from DigiKey or Mouser, in `core_BOM_digikey.csv` /
  `core_BOM_mouser.csv`). That's how the first core order went. The in-stock
  part that is properly rated is **GCT USB4085-GF-A, C7095263**: 48 V, 5 A,
  240 W, 1350 free at JLC. It is through-hole, though, so J1's footprint
  would have to change. Do that if USB4105 stays out.

**U8 (core) / U6 (Lite) CH340C C84681: short on every board.** JLC has
109k in stock but marks it not buyable (`isBuyComponent=0`, "no longer
manufacture"), so it won't place it. It swaps in C7464026 instead, a CH340C
with 100 in stock and 14,300 claimed. On the 10-28 order, that swap is why
U8 came up as C7464026. Use **WCH CH340B, C81010** (4.8k free) as a
drop-in. It is the same SOP-16 with a built-in clock. It differs from the C
only on pins 7 (RST#, an input with a pull-up), 8 (NC) and 15 (TNOW, an
output), and those three pads are unconnected on both boards (per their
netlists). It uses the same 1A86:7523 driver, and DTR#/RTS# on 13/14 still
drive the ESP32 auto-reset. CH340G (C14267, 42k free) has the same pinout
but needs a 12 MHz crystal on 7/8, a board change.

**ultra-front**'s BOM (checked on worker 261028-145644's branch on 10-29,
before it landed) is 18 lines, all ok at 100 boards. The thinnest is SW4,
the EC11E encoder (C361165), with 3204 free.

**Watch:** parts that pass but have less than 10x what 100 boards need free:

| Part | Board | Need | JLC free |
|---|---|---|---|
| U19 PLR237/T8BK, C17223620 | core/ultra | 100 | 234 |
| U10 ES9018K2M, C2688377 | core, core/ultra | 100 each | 660 |
| L1–L4 10 µH 8 A, C48391614 | lite-main | 400 | 1598 |

The script checks each board on its own. A part shared by Plus and Ultra in
one order needs both boards' counts together.

**Short by design: the TPA3220 (C2879306), U15 and U16 on core.** It
replaced the TPA3221 (task 261029-075354) so that no heatsink is fitted by
hand. JLC and LCSC hold none, so bom_stock.py reports it short. Buy it into
the JLC parts inventory before ordering. Mouser had 6,566 and DigiKey 2,032
on 2026-10-29, and `boards/core/fab/prebuy.csv` lists the counts. 100 Plus +
100 Ultra boards need 300.
