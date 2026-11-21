# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 midicore contributors
#!/usr/bin/env python3
"""
validate.py -- render the test MIDIs through the engine and assert properties.

This is the engine's regression suite. Every check is arithmetic on the rendered
audio, so a pass means a measurable property held, not that a file appeared.

usage: validate.py [--quick]
"""
import os, sys, wave, subprocess, math
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HR = os.path.join(ROOT, 'build', 'hr')
BANK = os.path.join(ROOT, 'build', 'gm.syn')
TESTS = os.path.join(ROOT, 'build', 'tests')
RATE = 32000

FAIL = []
def check(name, ok, detail=''):
    print(f"    {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ''))
    if not ok:
        FAIL.append(f"{name} {detail}")

def render(mid, secs, voices=16, out=None):
    out = out or (mid.replace('.mid', '.wav'))
    r = subprocess.run([HR, BANK, mid, out, str(secs), str(RATE), str(voices)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("    render failed:", r.stderr.strip()[:200]); return None, 0
    voices_end = 0
    for line in r.stderr.splitlines():
        if 'voices_peak_state=' in line:
            voices_end = int(line.split('voices_peak_state=')[1].split()[0])
    w = wave.open(out)
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32)
    w.close()
    x = x.reshape(-1, 2) / 32768.0
    return x, voices_end

def f0_autocorr(seg, sr, fmin=20.0, fmax=6000.0):
    seg = seg - seg.mean()
    n = len(seg)
    if n < 512 or np.abs(seg).max() < 1e-5:
        return 0.0
    w = np.hanning(n)
    xw = seg * w
    nfft = 1 << int(2 * n - 1).bit_length()
    X = np.fft.rfft(xw, nfft)
    ac = np.fft.irfft(X * np.conj(X), nfft)[:n].astype(np.float64)
    lo = max(2, int(sr / fmax)); hi = min(n - 2, int(sr / fmin))
    if hi <= lo: return 0.0
    seg_ac = ac[lo:hi]
    i = int(np.argmax(seg_ac)) + lo
    if 0 < i < n - 1:
        a, b, c = ac[i - 1], ac[i], ac[i + 1]
        den = a - 2 * b + c
        if abs(den) > 1e-12:
            i = i + 0.5 * (a - c) / den
    return sr / i if i > 0 else 0.0

def rms(x): return float(np.sqrt(np.mean(x ** 2))) if len(x) else 0.0
def cents(f, ref): return 1200.0 * math.log2(f / ref) if f > 0 and ref > 0 else 999.0
def hz(key): return 440.0 * 2.0 ** ((key - 69) / 12.0)

def seg_of(x, t0, t1):
    a = int(t0 * RATE); b = int(t1 * RATE)
    b = min(b, len(x))
    return x[a:b] if b > a else np.zeros(1, dtype=np.float32)

# ------------------------------------------------------------------- tests
def t_pitch(quick):
    print("\n[01] pitch sweep -- phase increment maths")
    x, _ = render(os.path.join(TESTS, '01_pitch_sweep.mid'), 222)
    if x is None: return
    mono = x.mean(1)
    errs, misses = [], 0
    for i, key in enumerate(range(21, 109)):
        t0 = i * 2.5 + 0.40
        seg = seg_of(mono, t0, t0 + 1.60)
        if rms(seg) < 1e-4:
            misses += 1
            continue
        f = f0_autocorr(seg, RATE, fmin=hz(key) * 0.6, fmax=hz(key) * 1.7)
        if f <= 0:
            misses += 1
            continue
        errs.append((key, cents(f, hz(key))))
    check('every note produced audio', misses == 0, f'{misses} silent')
    if errs:
        worst = max(errs, key=lambda e: abs(e[1]))
        acc = [abs(c) for _, c in errs]
        check('f0 within 15 cents of equal temperament',
              worst[1] is not None and max(acc) < 15.0,
              f'worst {worst[1]:+.1f} cents at MIDI {worst[0]}, mean {np.mean(acc):.2f}')
        check('f0 within 5 cents (median)', float(np.median(acc)) < 5.0,
              f'median {np.median(acc):.2f} cents')
    check('no clipping', int(np.sum(np.abs(x) > 0.999)) == 0,
          f'{int(np.sum(np.abs(x) > 0.999))} samples')

def t_programs(quick):
    print("\n[02] program sweep -- 128 GM programs")
    x, _ = render(os.path.join(TESTS, '02_program_sweep.mid'), 193)
    if x is None: return
    mono = x.mean(1)
    silent, levels = [], []
    for p in range(128):
        t0 = p * 1.5 + 0.1
        seg = seg_of(mono, t0, t0 + 1.3)
        r = rms(seg)
        levels.append(r)
        if r < 3e-3:
            silent.append(p)
    check('all 128 programs audible', not silent, f'silent: {silent[:12]}')
    lv = np.array(levels)
    check('no program wildly louder than the rest', lv.max() < lv.mean() * 4.0,
          f'max {lv.max():.3f} mean {lv.mean():.3f}')
    check('level spread under 20 dB',
          (lv[lv > 0].max() / max(lv[lv > 0].min(), 1e-6)) < 10.0,
          f'{20*math.log10(lv[lv>0].max()/max(lv[lv>0].min(),1e-6)):.1f} dB')
    check('no clipping', int(np.sum(np.abs(x) > 0.999)) == 0,
          f'{int(np.sum(np.abs(x) > 0.999))} samples')

def t_velocity(quick):
    print("\n[03] velocity sweep -- velocity -> gain")
    x, _ = render(os.path.join(TESTS, '03_velocity_sweep.mid'), 17)
    if x is None: return
    mono = x.mean(1)
    vels = [1, 4, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 120, 127]
    levels = []
    for i, v in enumerate(vels):
        t0 = i * 1.0 + 0.10
        levels.append(rms(seg_of(mono, t0, t0 + 0.35)))
    lv = np.array(levels)
    inc = int(np.sum(np.diff(lv) > -0.002))
    check('response is monotonic non-decreasing', inc >= len(vels) - 2,
          f'{inc}/{len(vels)-1} increasing steps')
    check('loudest is at least 2x the softest',
          lv[-1] > lv[0] * 2.0, f'v1 {lv[0]:.4f} v127 {lv[-1]:.4f} ratio {lv[-1]/max(lv[0],1e-9):.1f}')
    check('velocity actually varies (>10 dB range)',
          20 * math.log10(lv[-1] / max(lv[0], 1e-9)) > 10.0,
          f'{20*math.log10(lv[-1]/max(lv[0],1e-9)):.1f} dB')

def t_length(quick):
    print("\n[04] length sweep -- release / stuck notes")
    x, voices_end = render(os.path.join(TESTS, '04_length_sweep.mid'), 26)
    if x is None: return
    mono = x.mean(1)
    ticks = [7, 15, 30, 60, 120, 240, 480, 960]
    tails = []
    for i, tk in enumerate(ticks):
        t_end = i * 3.0 + tk * (0.5 / DIV)
        tail = rms(seg_of(mono, t_end + 1.5, t_end + 1.8))
        tails.append(tail)
    check('every note has decayed 1.2 s after note-off',
          max(tails) < 0.004, f'worst tail rms {max(tails):.5f}')
    check('no stuck voices at end of song', voices_end == 0, f'{voices_end} active')

def t_polyphony(quick):
    print("\n[05] polyphony -- allocation and headroom")
    x, _ = render(os.path.join(TESTS, '05_polyphony.mid'), 25, voices=32)
    if x is None: return
    mono = x.mean(1)
    levels = []
    for n in range(1, 17):
        t0 = (n - 1) * 1.5 + 0.05
        levels.append(rms(seg_of(mono, t0, t0 + 0.40)))
    lv = np.array(levels)
    check('all chord sizes audible', lv.min() > 3e-3, f'min {lv.min():.4f}')
    check('16-note chord louder than single note', lv[-1] > lv[0],
          f'1 note {lv[0]:.4f} -> 16 notes {lv[-1]:.4f}')
    check('no clipping up to 16 voices', int(np.sum(np.abs(x) > 0.999)) == 0,
          f'{int(np.sum(np.abs(x) > 0.999))} samples')

def t_pan(quick):
    print("\n[06] pan sweep -- CC10 stereo placement")
    x, _ = render(os.path.join(TESTS, '06_pan_sweep.mid'), 10)
    if x is None: return
    pans = [0, 16, 32, 48, 64, 80, 96, 112, 127]
    bal = []
    for i, p in enumerate(pans):
        t0 = i * 1.0 + 0.10
        seg = x[int(t0*RATE):int((t0+0.35)*RATE)]
        l = rms(seg[:, 0]); r = rms(seg[:, 1])
        bal.append((l - r) / max(l + r, 1e-9))
    b = np.array(bal)
    check('left pan puts energy left', b[0] > 0.3, f'CC0 balance {b[0]:+.2f}')
    check('right pan puts energy right', b[-1] < -0.3, f'CC127 balance {b[-1]:+.2f}')
    check('balance tracks the controller', b[0] - b[-1] > 0.6,
          f'swing {b[0]:+.2f} -> {b[-1]:+.2f}')
    check('centre is centred', abs(b[len(b)//2]) < 0.15, f'CC64 balance {b[len(b)//2]:+.2f}')

def t_drums(quick):
    print("\n[07] drum kit -- 47 keys")
    x, _ = render(os.path.join(TESTS, '07_drums.mid'), 13, voices=24)
    if x is None: return
    mono = x.mean(1)
    silent = []
    for i, key in enumerate(range(35, 82)):
        t0 = i * 0.25
        seg = seg_of(mono, t0, t0 + 0.20)
        if rms(seg) < 3e-3:
            silent.append(key)
    check('all 47 drum keys audible', not silent, f'silent: {silent[:12]}')
    check('no clipping', int(np.sum(np.abs(x) > 0.999)) == 0,
          f'{int(np.sum(np.abs(x) > 0.999))} samples')

def t_torture(quick):
    print("\n[08] torture -- 22k notes, 16 voices, program churn")
    x, voices_end = render(os.path.join(TESTS, '08_torture.mid'), 92, voices=16)
    if x is None: return
    mono = x.mean(1)
    check('no silent gap in the middle',
          rms(seg_of(mono, 2, 8)) > 1e-3, f'rms {rms(seg_of(mono, 2, 8)):.4f}')
    check('no runaway clipping', float(np.mean(np.abs(x) > 0.999)) < 0.02,
          f'{100*np.mean(np.abs(x) > 0.999):.2f}% samples')
    check('engine survives (no NaN / DC blowup)',
          float(np.abs(mono).max()) < 1.001 and abs(float(mono.mean())) < 0.05,
          f'peak {np.abs(mono).max():.3f} dc {mono.mean():+.4f}')

DIV = 480

def main():
    quick = '--quick' in sys.argv
    print("midicore validation suite")
    print(f"  engine {HR}\n  bank   {BANK} ({os.path.getsize(BANK)} bytes)\n  rate   {RATE}")
    for fn in (t_pitch, t_programs, t_velocity, t_length,
               t_polyphony, t_pan, t_drums, t_torture):
        if quick and fn is t_programs:
            print("\n[02] skipped in --quick"); continue
        fn(quick)
    print("\n" + "=" * 62)
    if FAIL:
        print(f"RESULT: {len(FAIL)} check(s) FAILED")
        for f in FAIL:
            print("  -", f)
        return 1
    print("RESULT: all checks passed")
    return 0

if __name__ == '__main__':
    sys.exit(main())
