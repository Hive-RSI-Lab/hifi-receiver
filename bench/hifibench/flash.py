"""Flash a board over its USB-C port with esptool.

The CH340C's DTR/RTS drive EN and GPIO0 through the auto-program pair, so
esptool's default reset puts the WROVER-E in the ROM loader with no buttons.
The image is an ESP-IDF build directory (its `flash_args` names every part
and offset) or one merged binary written at 0x0.
"""
from __future__ import annotations

import shlex
import subprocess
import sys
from pathlib import Path


def command(port: str, image: str, baud: int = 921600, esptool: str | None = None) -> tuple[list[str], str | None]:
    """The esptool command line and the directory to run it in."""
    tool = shlex.split(esptool) if esptool else [sys.executable, "-m", "esptool"]
    base = tool + ["--chip", "esp32", "--port", port, "--baud", str(baud),
                   "--before", "default_reset", "--after", "hard_reset", "write_flash"]
    p = Path(image)
    if p.is_dir():
        if not (p / "flash_args").is_file():
            raise FileNotFoundError(f"{p}/flash_args: not an ESP-IDF build directory")
        return base + ["@flash_args"], str(p)
    if not p.is_file():
        raise FileNotFoundError(image)
    return base + ["0x0", str(p.resolve())], None


def flash(port: str, image: str, baud: int = 921600, esptool: str | None = None, dry_run: bool = False) -> int:
    cmd, cwd = command(port, image, baud, esptool)
    print("$", (f"(cd {cwd} && " if cwd else "") + shlex.join(cmd) + (")" if cwd else ""))
    if dry_run:
        return 0
    try:
        return subprocess.call(cmd, cwd=cwd)
    except FileNotFoundError:
        print("esptool not found: pip install esptool (or pass --esptool)", file=sys.stderr)
        return 127
