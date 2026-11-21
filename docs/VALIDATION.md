# Validation

The engine ships with a generated test suite. `tools/mk_tests.py` writes eight
MIDI files, each isolating one axis; `tools/validate.py` renders them and asserts
properties **on the audio**, so a pass means a measured property held.

```bash
python3 tools/mk_tests.py build/tests
python3 tools/validate.py            # add --quick to skip the 128-program sweep
```

## The tests

| # | File | Isolates | Checks |
|---|---|---|---|
| 01 | `01_pitch_sweep.mid` | phase-increment maths | 88 notes A0..C8, f0 vs equal temperament, silence, clipping |
| 02 | `02_program_sweep.mid` | bank + program change | all 128 GM programs audible, level spread < 20 dB |
| 03 | `03_velocity_sweep.mid` | velocity → gain | monotonic, > 10 dB range, loudest ≥ 2x softest |
| 04 | `04_length_sweep.mid` | release / stuck notes | silent 1.5 s after note-off, zero voices at end |
| 05 | `05_polyphony.mid` | allocation + headroom | chords 1..16 notes, no clipping |
| 06 | `06_pan_sweep.mid` | CC10 stereo | balance tracks controller, centre is centred |
| 07 | `07_drums.mid` | drum bank | all 47 keys audible |
| 08 | `08_torture.mid` | voice stealing | 22 400 notes, program churn, no stall or blowup |

## Bugs the suite found

These were all live in 1.0.0 and were caught by the suite, not by inspection:

1. **`note_inc` truncated to whole Hz.** A 30.87 Hz note played at 30 Hz —
   **49 cents flat**. Fixed by keeping 10 fractional bits of Hz.
2. **Velocity was computed and discarded.** The gain value was assigned to a
   local and immediately `(void)`-ed, so every note played at full level.
3. **Velocity multiply overflowed `int32`.** `(65536 - vcurve) * depth` reaches
   3.25e9; the signed overflow produced garbage for low velocities.
4. **Channel pan was stored, never applied.** Only the patch's own pan reached
   the mixer, so CC10 did nothing.
5. **FM phase offset overflowed `uint32`.** `((m*level)>>5)<<16` reaches 8.5e9
   and wrapped, giving arbitrary modulation depth on every 2-op patch.
6. **The bank packer was one byte out of step** from field 3 onward, so
   `velDepth` read as 0 and every field after it was misaligned.

## Current result

```
midicore validation suite
  engine /home/user/workspace/midicore/build/hr
  bank   /home/user/workspace/midicore/build/gm.syn (68128 bytes)
  rate   32000

[01] pitch sweep -- phase increment maths
    PASS  every note produced audio   0 silent
    PASS  f0 within 15 cents of equal temperament   worst +2.7 cents at MIDI 103, mean 0.95
    PASS  f0 within 5 cents (median)   median 0.98 cents
    PASS  no clipping   0 samples

[02] program sweep -- 128 GM programs
    PASS  all 128 programs audible   silent: []
    PASS  no program wildly louder than the rest   max 0.043 mean 0.022
    PASS  level spread under 20 dB   19.5 dB
    PASS  no clipping   0 samples

[03] velocity sweep -- velocity -> gain
    PASS  response is monotonic non-decreasing   15/15 increasing steps
    PASS  loudest is at least 2x the softest   v1 0.0119 v127 0.0485 ratio 4.1
    PASS  velocity actually varies (>10 dB range)   12.2 dB

[04] length sweep -- release / stuck notes
    PASS  every note has decayed 1.2 s after note-off   worst tail rms 0.00000
    PASS  no stuck voices at end of song   0 active

[05] polyphony -- allocation and headroom
    PASS  all chord sizes audible   min 0.0285
    PASS  16-note chord louder than single note   1 note 0.0285 -> 16 notes 0.0989
    PASS  no clipping up to 16 voices   0 samples

[06] pan sweep -- CC10 stereo placement
    PASS  left pan puts energy left   CC0 balance +1.00
    PASS  right pan puts energy right   CC127 balance -1.00
    PASS  balance tracks the controller   swing +1.00 -> -1.00
    PASS  centre is centred   CC64 balance +0.00

[07] drum kit -- 47 keys
    PASS  all 47 drum keys audible   silent: []
    PASS  no clipping   0 samples

[08] torture -- 22k notes, 16 voices, program churn
    PASS  no silent gap in the middle   rms 0.1514
    PASS  no runaway clipping   0.00% samples
    PASS  engine survives (no NaN / DC blowup)   peak 0.736 dc -0.0004

==============================================================
RESULT: all checks passed
```

## What this does *not* prove

* **Real-time behaviour on hardware.** All numbers come from the x86 host build.
* **Subjective quality.** These are arithmetic checks — they prove the engine is
  correct, not that it sounds good. That is an ear call.
* **Aliasing at high notes.** The pitch test confirms the fundamental is right;
  it does not measure inharmonic energy above Nyquist. Expect brightness loss on
  notes above ~MIDI 100 at 32 kHz.
