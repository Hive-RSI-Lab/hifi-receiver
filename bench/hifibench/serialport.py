"""Open a serial port: pyserial when it's installed, else termios (POSIX)."""
from __future__ import annotations

import os


def open_port(path: str, baud: int = 115200, timeout: float = 0.5):
    try:
        import serial  # type: ignore
        return serial.Serial(path, baud, timeout=timeout)
    except ImportError:
        return _Termios(path, baud, timeout)


class _Termios:
    def __init__(self, path, baud, timeout):
        import termios
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        speed = getattr(termios, f"B{baud}")
        a[0] = 0                                   # iflag: raw
        a[1] = 0                                   # oflag
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[3] = 0                                   # lflag: no echo, no canonical
        a[4] = a[5] = speed
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = max(1, int(timeout * 10))
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.buf = b""

    def write(self, data: bytes):
        os.write(self.fd, data)

    def readline(self) -> bytes:
        while b"\n" not in self.buf:
            chunk = os.read(self.fd, 256)
            if not chunk:
                out, self.buf = self.buf, b""
                return out
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line + b"\n"

    def close(self):
        os.close(self.fd)
