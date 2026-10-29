"""Record the amp's output, and read the chip temperature.

The wiring (docs/bench.md): each output leg of the bridge-tied channel goes
through a divider to one input of a stereo USB audio interface, so the bench
records leg+ and leg- against ground and takes the difference. The
differential volts are the record times `volts_per_fs` (the divider ratio
and the interface's full scale, set once with `bench.py calibrate`).
"""
from __future__ import annotations

import shlex
import shutil
import struct
import subprocess
import wave


class Capture:
    rate = 48000

    def record(self, seconds: float) -> list[list[float]]:
        """Channels of samples scaled to +-1.0 full scale."""
        raise NotImplementedError


class Arecord(Capture):
    """ALSA's arecord (Linux). device is an ALSA name such as hw:1,0."""

    def __init__(self, device: str, rate: int = 48000, channels: int = 2):
        if not shutil.which("arecord"):
            raise RuntimeError("arecord not found (alsa-utils); use --capture sounddevice")
        self.device, self.rate, self.channels = device, rate, channels

    def record(self, seconds):
        n = int(seconds * self.rate)
        raw = subprocess.run(["arecord", "-q", "-D", self.device, "-t", "raw", "-f", "S32_LE",
                              "-r", str(self.rate), "-c", str(self.channels), "-s", str(n)],
                             check=True, capture_output=True).stdout
        return _deinterleave(raw, 4, self.channels)


class SoundDevice(Capture):
    """python-sounddevice (PortAudio): macOS, Windows and Linux."""

    def __init__(self, device: str | None, rate: int = 48000, channels: int = 2):
        import sounddevice  # type: ignore
        self.sd, self.device, self.rate, self.channels = sounddevice, device, rate, channels

    def record(self, seconds):
        data = self.sd.rec(int(seconds * self.rate), samplerate=self.rate, channels=self.channels,
                           dtype="float32", device=self.device, blocking=True)
        return [[float(r[c]) for r in data] for c in range(self.channels)]


class WavFile(Capture):
    """A recorded WAV, for analysing a capture made some other way."""

    def __init__(self, path: str):
        self.path = path

    def record(self, seconds=0):
        with wave.open(self.path) as w:
            self.rate = w.getframerate()
            return _deinterleave(w.readframes(w.getnframes()), w.getsampwidth(), w.getnchannels())


def _deinterleave(raw: bytes, width: int, channels: int) -> list[list[float]]:
    fmt = {2: "h", 4: "i"}[width]
    full = float(1 << (8 * width - 1))
    n = len(raw) // (width * channels)
    vals = struct.unpack(f"<{n * channels}{fmt}", raw[:n * width * channels])
    return [[v / full for v in vals[c::channels]] for c in range(channels)]


def differential(chans: list[list[float]], mode: str) -> list[float]:
    """diff: leg+ on input 1, leg- on input 2. single: input 1 alone."""
    if mode == "diff":
        return [a - b for a, b in zip(chans[0], chans[1])]
    return chans[0]


# ---- temperature ----------------------------------------------------------

class TempProbe:
    """Where the amp chip's temperature comes from. Lite has no sensor on
    the board, so it's an external probe on the chip's top: a command that
    prints degrees C (a thermocouple reader's CLI), or typed in by hand."""

    def __init__(self, cmd: str | None = None, manual: bool = False, fn=None):
        self.cmd, self.manual, self.fn = cmd, manual, fn

    @property
    def kind(self) -> str:
        return "cmd" if self.cmd else "manual" if self.manual else "fn" if self.fn else "device"

    def read(self, state: dict | None = None) -> float | None:
        if self.fn:
            return self.fn()
        if self.cmd:
            out = subprocess.run(shlex.split(self.cmd), capture_output=True, text=True, timeout=10)
            try:
                return float(out.stdout.split()[0])
            except (ValueError, IndexError):
                return None
        if self.manual:
            v = input("  chip temperature, deg C (blank to skip): ").strip()
            return float(v) if v else None
        return (state or {}).get("temp_c")
