# bench

Bring-up and test of the receiver boards from a laptop: find, flash,
self-test, power sweep into dummy loads, 1/8-power thermal soak, and a
results page. The guide is [docs/bench.md](../docs/bench.md).

```sh
python3 bench/bench.py --help
python3 bench/bench.py --mock bringup --soak-s 300     # no board needed
python3 -m unittest discover -s bench/tests            # also run by tools/hwcheck.py test
```
