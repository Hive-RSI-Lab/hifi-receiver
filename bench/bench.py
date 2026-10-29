#!/usr/bin/env python3
"""Bring-up and test of the receiver boards. docs/bench.md is the guide.

    python3 bench/bench.py find
    python3 bench/bench.py --mock bringup --soak-s 120
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hifibench.cli import main  # noqa: E402

sys.exit(main())
