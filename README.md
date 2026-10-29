# hifi-receiver-2

The WiiM-Ultra-like hi-fi streaming receiver/amp product in tiers (barebones, medium, full), designed with hwde on H0: boards, firmware, app, enclosure, manuals.

- [docs/product.md](docs/product.md): the three tiers (Lite, Plus, Ultra), every decision and why, power, cost and build order.
- [docs/plan.md](docs/plan.md): the later work in order, one worker session each.
- [docs/bom-estimate.csv](docs/bom-estimate.csv) + `python3 tools/bom_estimate.py`: per-tier BOM and JLC cost at 10 and 100 units (`--unit lite 100` prints one number).
- [docs/bom-stock.md](docs/bom-stock.md) + `python3 tools/bom_stock.py`: every board's BOM against live JLC and LCSC stock before an order, with checked alternates ([bom-alternates.csv](docs/bom-alternates.csv)) where a part is short.
- [docs/checks.md](docs/checks.md) + `python3 tools/hwcheck.py test|score`: the project's check (product.md complete, every board passes hwde's erc, drc_routed, verify and dfm gates) and score (the first tier's unit cost at 100 from its board's BOM, lower is better, only when its gates pass).
- [docs/manual/](docs/manual/user-manual.md): the user manual for all three tiers, the one-page [quick start](docs/manual/quick-start.md) for the box, and [sources.md](docs/manual/sources.md): where each fact comes from and what is still to be confirmed.
- [docs/app.md](docs/app.md) + `app/`: the web app (PWA) the firmware serves: what it does, the API it needs beyond docs/test-api.md, a mock receiver (`python3 app/mock/server.py`), the firmware pack (`python3 app/build.py`), tests (`python3 app/tests/test_app.py`) and screenshots.
