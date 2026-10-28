# hifi-receiver-2

The WiiM-Ultra-like hi-fi streaming receiver/amp product in tiers (barebones, medium, full), designed with hwde on H0: boards, firmware, app, enclosure, manuals.

- [docs/product.md](docs/product.md): the three tiers (Lite, Plus, Ultra), every decision and why, power, cost and build order.
- [docs/plan.md](docs/plan.md): the later work in order, one worker session each.
- [docs/bom-estimate.csv](docs/bom-estimate.csv) + `python3 tools/bom_estimate.py`: per-tier BOM and JLC cost at 10 and 100 units (`--unit lite 100` prints one number).
- [docs/checks.md](docs/checks.md) + `python3 tools/hwcheck.py test|score`: the project's check (product.md complete, every board passes hwde's erc, drc_routed, verify and dfm gates) and score (the first tier's unit cost at 100 from its board's BOM, lower is better, only when its gates pass).
- [docs/app.md](docs/app.md) + `app/`: the web app (PWA) the firmware serves: what it does, the API it needs beyond docs/test-api.md, a mock receiver (`python3 app/mock/server.py`), the firmware pack (`python3 app/build.py`), tests (`python3 app/tests/test_app.py`) and screenshots.
