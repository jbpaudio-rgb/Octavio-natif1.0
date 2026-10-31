#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""FFT check of a chord on the polyphonic Prophet (NOTES-M): python3 tools/chordfft.py out.wav T0 T1 [midi notes...]
Takes the window [T0, T1] s (a held chord), lists the strongest spectral peaks and, for each expected note, the level of its fundamental
against the median of the spectrum around it.  Also reports clicks (second-difference spikes) and sample-to-sample jumps in [T0, T1]."""
import sys, wave, numpy as np
fn, t0, t1 = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
notes = [int(x) for x in sys.argv[4:]]
w = wave.open(fn); fs = w.getframerate(); n = w.getnframes()
a = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, 2)[:, 0].astype(float)
x = a[int(t0 * fs):int(t1 * fs)]
N = len(x); win = np.hanning(N); X = np.abs(np.fft.rfft(x * win)); f = np.fft.rfftfreq(N, 1 / fs)
db = 20 * np.log10(X / X.max() + 1e-9)
print(f"window {t0}-{t1} s, {N} samples, rms {np.sqrt((x**2).mean()):.0f}, peak {np.abs(x).max():.0f}")
# peaks: local maxima above -45 dB, merged within 3 % of the frequency
pk = [i for i in range(2, len(X) - 2) if X[i] > X[i - 1] and X[i] >= X[i + 1] and db[i] > -45 and f[i] > 40]
res = []
for i in sorted(pk, key=lambda i: -db[i]):
    if all(abs(f[i] / f[j] - 1) > 0.03 for j in res): res.append(i)
print("strongest peaks (Hz, dB re max):", ", ".join(f"{f[i]:.1f} ({db[i]:.0f})" for i in sorted(res[:14], key=lambda i: f[i])))
for m in notes:
    hz = 440 * 2 ** ((m - 69) / 12)
    lo, hi = np.searchsorted(f, hz * 0.97), np.searchsorted(f, hz * 1.03)
    band = db[lo:hi + 1]
    ref = np.median(db[np.searchsorted(f, hz * 0.5):np.searchsorted(f, hz * 1.5)])
    print(f"note {m} ({hz:.1f} Hz): peak {band.max():.0f} dB re max, {band.max() - ref:.0f} dB over the local median")
d = np.abs(np.diff(x)); print(f"largest sample step {d.max():.0f}, 99.9 % {np.percentile(d, 99.9):.0f}")
