# hifi-receiver-2

The WiiM-Ultra-like hi-fi streaming receiver/amp product in tiers (barebones, medium, full), designed with hwde on H0: boards, firmware, app, enclosure, manuals.

- [docs/product.md](docs/product.md): the three tiers (Lite, Plus, Ultra), every decision and why, power, cost and build order.
- [docs/plan.md](docs/plan.md): the later work in order, one worker session each.
- [docs/bom-estimate.csv](docs/bom-estimate.csv) + `python3 tools/bom_estimate.py`: per-tier BOM and JLC cost at 10 and 100 units (`--unit lite 100` prints one number).
- [docs/bom-stock.md](docs/bom-stock.md) + `python3 tools/bom_stock.py`: every board's BOM against live JLC and LCSC stock before an order, with checked alternates ([bom-alternates.csv](docs/bom-alternates.csv)) where a part is short.
- [docs/checks.md](docs/checks.md) + `python3 tools/hwcheck.py test|score`: the project's check (product.md complete, every board passes hwde's erc, drc_routed, verify and dfm gates) and score (the whole product, lower is better: every tier's unit cost at 100 from its boards' BOMs, counted only when they pass their gates and carry every function [`tools/product.json`](tools/product.json) names, plus 100 for each piece (firmware, app, bench, enclosures, manual) until its check in `tools/pieces.py`, which a stub fails, and its tests pass).
- [docs/order-readiness.md](docs/order-readiness.md): where each board stands before the order (gates, sims, reviews, firmware, bring-up plans) and what needs the person.
- [docs/manual/](docs/manual/user-manual.md): the user manual for all three tiers, the one-page [quick start](docs/manual/quick-start.md) for the box, and [sources.md](docs/manual/sources.md): where each fact comes from and what is still to be confirmed.
- [docs/app.md](docs/app.md) + `app/`: the web app (PWA) the firmware serves: what it does, the API it needs beyond docs/test-api.md, a mock receiver (`python3 app/mock/server.py`), the firmware pack (`python3 app/build.py`), tests (`python3 app/tests/test_app.py`) and screenshots.

## Licence

- **Hardware** (`boards/`, `enclosure/`): [CERN-OHL-P-2.0](LICENSES/CERN-OHL-P-2.0.txt).
- **Our own code and documents** (`app/`, `bench/`, `tools/`, `docs/`, and in `firmware/` the `components/hifi`, `board`, `test` and `tools` folders): [MIT](LICENSES/MIT.txt).
- **Firmware builds**, including the makers' image, are [GPL-3.0](LICENSES/GPL-3.0.txt) as a whole, because they link squeezelite. Each build names its exact source: this repo's commit, `firmware/upstream.lock`, the patches and the build steps.
- **Not ours:** `firmware/third_party/libmad` is GPL-2.0-or-later. The `boards/*/lib/aiee.*` footprints and 3D models come from EasyEDA parts through easyeda2kicad and keep their own terms. A change in `firmware/patches` or `firmware/idf-patches` takes the licence of the file it changes.

The full map is in [LICENSE](LICENSE), and the licence texts are in [LICENSES/](LICENSES/).
