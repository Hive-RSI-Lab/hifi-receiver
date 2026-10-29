#!/usr/bin/env python3
"""Set up a new receiver from your laptop: Wi-Fi, the relay, done. docs/remote.md.

    python3 bench/provision.py                 # the board on this laptop's USB-C
    python3 bench/provision.py --image firmware/build --wifi 'home:password'
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hifibench.provision import main  # noqa: E402

sys.exit(main())
