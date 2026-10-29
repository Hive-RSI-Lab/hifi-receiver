"""Sine analysis in pure Python: frequency, level, THD and THD+N.

The board plays the tone from its own clock and the audio interface samples
on another, so the frequency is measured, not assumed: zero crossings give
it to a few ppm, a four-parameter sine fit (IEEE 1057) refines it, and the
record is cut to whole cycles so the harmonic projections are orthogonal.
"""
from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass
class Tone:
    freq_hz: float
    amp: float          # fundamental peak, in the record's units
    dc: float
    thd: float          # harmonics 2..10 over the fundamental, ratio
    thdn: float         # everything but fundamental and DC over the fundamental, ratio
    harmonics: list[float]  # peak of harmonics 2..10

    @property
    def rms(self) -> float:
        return self.amp / math.sqrt(2)


def _rising_crossings(x: list[float], fs: float) -> list[float]:
    mean = sum(x) / len(x)
    out = []
    # Hysteresis so noise at the crossing doesn't count twice.
    peak = max(abs(v - mean) for v in x) or 1.0
    h = 0.05 * peak
    armed = False
    for i in range(1, len(x)):
        a, b = x[i - 1] - mean, x[i] - mean
        if b < -h:
            armed = True
        if armed and a < 0 <= b:
            out.append((i - 1 + a / (a - b)) / fs)
            armed = False
    return out


def _fit(x, t, w):
    """Three-parameter least squares at angular frequency w: a*sin + b*cos + c."""
    ss = sc = cc = s1 = c1 = xs = xc = x1 = 0.0
    n = len(x)
    for v, ti in zip(x, t):
        s, c = math.sin(w * ti), math.cos(w * ti)
        ss += s * s; sc += s * c; cc += c * c; s1 += s; c1 += c
        xs += v * s; xc += v * c; x1 += v
    return _solve([[ss, sc, s1], [sc, cc, c1], [s1, c1, n]], [xs, xc, x1])


def _fit4(x, t, w, a, b):
    """One Gauss-Newton step of the four-parameter fit: returns a, b, c, w."""
    m = [[0.0] * 4 for _ in range(4)]
    r = [0.0] * 4
    for v, ti in zip(x, t):
        s, c = math.sin(w * ti), math.cos(w * ti)
        d = ti * (a * c - b * s)
        col = (s, c, 1.0, d)
        for i in range(4):
            r[i] += col[i] * v
            for j in range(i, 4):
                m[i][j] += col[i] * col[j]
    for i in range(4):
        for j in range(i):
            m[i][j] = m[j][i]
    a2, b2, c2, dw = _solve(m, r)
    return a2, b2, c2, w + dw


def _solve(m, r):
    n = len(r)
    m = [row[:] + [r[i]] for i, row in enumerate(m)]
    for i in range(n):
        p = max(range(i, n), key=lambda k: abs(m[k][i]))
        m[i], m[p] = m[p], m[i]
        if m[i][i] == 0:
            raise ValueError("singular fit (silent or clipped-flat record?)")
        for k in range(i + 1, n):
            f = m[k][i] / m[i][i]
            for j in range(i, n + 1):
                m[k][j] -= f * m[i][j]
    out = [0.0] * n
    for i in reversed(range(n)):
        out[i] = (m[i][n] - sum(m[i][j] * out[j] for j in range(i + 1, n))) / m[i][i]
    return out


def analyse(x: list[float], fs: float, nominal_hz: float | None = None, harmonics: int = 10) -> Tone:
    """Measure the one sine in x (sampled at fs)."""
    zc = _rising_crossings(x, fs)
    if len(zc) < 4:
        raise ValueError("no tone in the record (fewer than 4 cycles)")
    f = (len(zc) - 1) / (zc[-1] - zc[0])
    if nominal_hz and abs(f / nominal_hz - 1) > 0.05:
        raise ValueError(f"tone at {f:.1f} Hz, expected {nominal_hz:g} Hz")
    # Whole cycles only, from the first rising crossing to the last.
    i0, i1 = int(zc[0] * fs) + 1, int(zc[-1] * fs) + 1
    seg = x[i0:i1]
    t = [i / fs for i in range(len(seg))]
    w = 2 * math.pi * f
    a, b, c = _fit(seg, t, w)
    for _ in range(2):
        a, b, c, w = _fit4(seg, t, w, a, b)
    a, b, c = _fit(seg, t, w)
    amp = math.hypot(a, b)
    resid = [v - (a * math.sin(w * ti) + b * math.cos(w * ti) + c) for v, ti in zip(seg, t)]
    noise_rms = math.sqrt(sum(v * v for v in resid) / len(resid))
    hs = []
    for k in range(2, harmonics + 1):
        if k * w / (2 * math.pi) >= fs / 2:
            break
        ha, hb, _ = _fit(resid, t, k * w)
        hs.append(math.hypot(ha, hb))
    fund_rms = amp / math.sqrt(2) or 1e-30
    thd = math.sqrt(sum(h * h for h in hs) / 2) / fund_rms
    return Tone(w / (2 * math.pi), amp, c, thd, noise_rms / fund_rms, hs)


def rms(x: list[float]) -> float:
    m = sum(x) / len(x)
    return math.sqrt(sum((v - m) ** 2 for v in x) / len(x))
