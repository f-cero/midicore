# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 midicore contributors
#!/usr/bin/env python3
"""
mk_tests.py -- generate the midicore validation MIDI suite.

Each file isolates one axis of the engine so a failure points at a cause:
pitch, program coverage, velocity, release, polyphony, pan, drums, stress.

usage: mk_tests.py outdir
"""
import os, struct, sys

DIV = 480

def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))

def track(events):
    """events: list of (tick, bytes). Sorted, delta-encoded."""
    events = sorted(events, key=lambda e: e[0])
    body = bytearray()
    last = 0
    for tick, data in events:
        body += vlq(tick - last) + data
        last = tick
    body += vlq(0) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(body)) + bytes(body)

def write(path, events, div=DIV):
    hdr = b'MThd' + struct.pack('>IHHH', 6, 0, 1, div)
    open(path, 'wb').write(hdr + track(events))

def note(ch, tick, key, vel, dur):
    return [(tick, bytes([0x90 | ch, key, vel])),
            (tick + dur, bytes([0x80 | ch, key, 0]))]

def prog(ch, tick, p):
    return (tick, bytes([0xC0 | ch, p & 127]))

def cc(ch, tick, num, val):
    return (tick, bytes([0xB0 | ch, num, val & 127]))

def tempo(usq=500000):
    return (0, b'\xff\x51\x03' + struct.pack('>I', usq)[1:])

def title(name):
    b = name.encode('latin1')
    return (0, b'\xff\x03' + vlq(len(b)) + b)

# --------------------------------------------------------------- 01 pitch
def t_pitch(path):
    """Chromatic A0..C8 on program 0. Validates phase-increment maths."""
    ev = [title('pitch sweep'), tempo(), prog(0, 0, 0)]
    t = 0
    for key in range(21, 109):
        ev += note(0, t, key, 100, DIV * 4)   # 2 s, enough cycles at 30 Hz
        t += DIV * 5                          # 2.5 s spacing
    write(path, ev)

# ------------------------------------------------------------- 02 programs
def t_programs(path, first=0, last=128):
    """Every GM program plays the same phrase. Validates bank + program change."""
    ev = [title('program sweep'), tempo()]
    t = 0
    for p in range(first, last):
        ev.append(prog(0, t, p))
        for k, key in enumerate((48, 55, 60, 64)):   # C3 G3 C4 E4
            ev += note(0, t + k * DIV // 2, key, 96, DIV // 2 - 24)
        t += DIV * 3                     # 1.5 s per program
    write(path, ev)

# ------------------------------------------------------------- 03 velocity
def t_velocity(path):
    """One note at 16 velocities. Validates the velocity -> gain path."""
    ev = [title('velocity sweep'), tempo(), prog(0, 0, 0)]
    t = 0
    vels = [1, 4, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 112, 120, 127]
    for v in vels:
        ev += note(0, t, 60, v, DIV)
        t += DIV * 2
    write(path, ev)

# ---------------------------------------------------------------- 04 length
def t_length(path):
    """Note lengths 15 ms .. 2 s. Validates release and stuck notes."""
    ev = [title('length sweep'), tempo(), prog(0, 0, 0)]
    t = 0
    for ticks in (7, 15, 30, 60, 120, 240, 480, 960):
        ev += note(0, t, 60, 100, ticks)
        t += DIV * 6            # 3 s: room for a long release
    write(path, ev)

# ------------------------------------------------------------- 05 polyphony
def t_polyphony(path):
    """Chords of 1..16 notes. Validates allocation and headroom."""
    ev = [title('polyphony'), tempo(), prog(0, 0, 0)]
    t = 0
    for n in range(1, 17):
        for i in range(n):
            ev += note(0, t, 48 + i * 3, 90, DIV)
        t += DIV * 3
    write(path, ev)

# ------------------------------------------------------------------- 06 pan
def t_pan(path):
    """CC10 0..127 on a held note. Validates stereo placement."""
    ev = [title('pan sweep'), tempo(), prog(0, 0, 0)]
    t = 0
    for p in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        ev.append(cc(0, t, 10, p))
        ev += note(0, t, 60, 100, DIV)
        t += DIV * 2
    write(path, ev)

# ----------------------------------------------------------------- 07 drums
def t_drums(path):
    """Every drum key 35..81."""
    ev = [title('drum kit'), tempo(), cc(9, 0, 10, 64)]
    t = 0
    for key in range(35, 82):
        ev += note(9, t, key, 110, 60)
        t += DIV // 2
    write(path, ev)

# --------------------------------------------------------------- 08 torture
def t_torture(path):
    """Dense 32nds, 16 voices, program change every beat. Voice stealing."""
    ev = [title('torture'), tempo(), prog(0, 0, 0)]
    t = 0
    step = DIV // 8
    for i in range(1400):
        if i % 64 == 0:
            ev.append(prog(0, t, (i // 64 * 17) % 128))
        for k in range(16):
            ev += note(0, t, 40 + ((i * 7 + k * 5) % 60), 88, step)
        t += step
    write(path, ev)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'build/tests'
    os.makedirs(out, exist_ok=True)
    jobs = [
        ('01_pitch_sweep.mid',   t_pitch),
        ('02_program_sweep.mid', t_programs),
        ('03_velocity_sweep.mid', t_velocity),
        ('04_length_sweep.mid',  t_length),
        ('05_polyphony.mid',     t_polyphony),
        ('06_pan_sweep.mid',     t_pan),
        ('07_drums.mid',         t_drums),
        ('08_torture.mid',       t_torture),
    ]
    for name, fn in jobs:
        p = os.path.join(out, name)
        fn(p)
        print(f"  {name:24s} {os.path.getsize(p):7d} bytes")
    return 0

if __name__ == '__main__':
    sys.exit(main())
