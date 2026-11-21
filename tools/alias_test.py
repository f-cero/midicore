# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 midicore contributors
#!/usr/bin/env python3
"""
alias_test.py -- measure inharmonic (aliased) energy on isolated notes.

For a single note at f0, any energy that is NOT within a tolerance of a harmonic
k*f0 (k*f0 < Nyquist) is folded: it cannot come from the instrument, only from
aliasing. Reports that as a percentage of total energy.

usage: alias_test.py build/gm.syn [--rate 32000]
"""
import os, sys, struct, subprocess, tempfile, math
import numpy as np, wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIV = 480

def vlq(n):
    o = [n & 0x7F]; n >>= 7
    while n: o.append((n & 0x7F) | 0x80); n >>= 7
    return bytes(reversed(o))

def one_note(prog, key, path):
    ev = [(0, b'\xff\x51\x03\x07\xa1\x20'), (0, bytes([0xC0, prog])),
          (0, bytes([0x90, key, 100])), (DIV * 8, bytes([0x80, key, 0]))]
    body = bytearray(); last = 0
    for tick, d in ev:
        body += vlq(tick - last) + d; last = tick
    body += vlq(0) + b'\xff\x2f\x00'
    open(path, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, DIV) +
                           b'MTrk' + struct.pack('>I', len(body)) + bytes(body))

def render(bank, mid, wav, rate):
    subprocess.run([os.path.join(ROOT, 'build', 'hr'), bank, mid, wav,
                    '5.0', str(rate), '8'], check=True, capture_output=True)

def read(p):
    w = wave.open(p); sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    return x.reshape(-1, 2).mean(1), sr

def alias_ratio(x, sr, f0, tol=0.035):
    n = 32768
    if len(x) < n: x = np.pad(x, (0, n - len(x)))
    else: x = x[2048:2048 + n]
    S = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    nyq = sr / 2.0
    harm = np.zeros(len(f), dtype=bool)
    k = 1
    while k * f0 < nyq * 1.02:
        c = k * f0
        harm |= np.abs(f - c) <= max(tol * c, 2 * sr / len(x))
        k += 1
    # ignore the very low end (DC / window leakage below the fundamental)
    band = f > f0 * 0.55
    tot = S[band].sum() + 1e-30
    alias = S[band & ~harm].sum() / tot
    return float(alias)

CASES = [
    (99, 91), (99, 97), (99, 103), (99, 110), (99, 117),
    (82, 91), (81, 100), (74, 96), (48, 100), (4, 100),
]

def main():
    bank = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'gm.syn')
    rate = int(sys.argv[sys.argv.index('--rate') + 1]) if '--rate' in sys.argv else 32000
    tag = sys.argv[sys.argv.index('--tag') + 1] if '--tag' in sys.argv else 'run'
    tmp = tempfile.mkdtemp()
    print(f"{'prog':>4} {'key':>4} {'f0 Hz':>8} {'alias %':>8}")
    out = []
    for prog, key in CASES:
        mid = os.path.join(tmp, 'n.mid'); wav = os.path.join(tmp, 'n.wav')
        one_note(prog, key, mid)
        render(bank, mid, wav, rate)
        x, sr = read(wav)
        f0 = 440.0 * 2 ** ((key - 69) / 12.0)
        a = alias_ratio(x, sr, f0)
        out.append((prog, key, f0, a))
        print(f"{prog:4d} {key:4d} {f0:8.1f} {100*a:7.1f}%")
    mean = float(np.mean([o[3] for o in out]))
    print(f"\nmean aliasing: {100*mean:.1f}%   [{tag}]")
    return out

if __name__ == '__main__':
    main()
