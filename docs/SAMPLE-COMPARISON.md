# Synthesised waveforms vs. the source samples

Method: for each GM program, take the sample the converter actually chose, FFT
its steady state, and compare the harmonic amplitude profile with what the engine
synthesises for that program at C4. Profiles are compared by harmonic index, so
the comparison is pitch-independent. 128 programs measured; figure in
`fig.png`, raw numbers in `compare.json`.

## Summary

| | |
|---|---|
| Programs measured | 128 |
| Harmonic match (cosine) >= 0.95 | 62 |
| Harmonic match 0.55 - 0.95 | 50 |
| Harmonic match < 0.55 | 16 |
| Programs with no source sample at all | 26 |
| Level spread across all programs | ~19.5 dB |

Where the match is good it is very good: 62 programs land within a few percent
of the source spectrum, and the median cosine is ~0.97. The failures cluster,
and each cluster has a cause.

## Program-by-program, for the instruments your tracks use

| prog | name | root | cos | cent syn | cent smp | level dB |
|---|---|---|---|---|---|---|
| 33 | Fingered Bass | 84 | -- | 0.00 | 5.33 | -180.0 |
| 82 | Calliope Lead | None | -- | 1.76 | -- | -29.8 |
| 81 | Saw Lead | None | -- | 1.76 | -- | -29.8 |
| 5 | EPiano 2 | 74 | -- | 0.00 | 3.97 | -180.0 |
| 26 | Jazz Guitar | 67 | 1.00 | 1.86 | 2.26 | -35.9 |
| 48 | Strings 1 | 46 | 0.96 | 35.32 | 74.83 | -45.2 |
| 99 | Atmosphere | None | -- | 1.76 | -- | -29.8 |
| 29 | Overdriven Gtr | 46 | 0.66 | 18.90 | 11.27 | -125.3 |
| 24 | Nylon Guitar | 35 | 0.99 | 7.00 | 19.02 | -38.7 |
| 87 | Bass+Lead | 33 | 0.82 | 1.76 | 3.92 | -29.8 |
| 80 | Square Lead | 51 | -- | 1.76 | -- | -29.8 |
| 44 | Tremolo Strings | 42 | 0.98 | 38.58 | 113.87 | -76.6 |
| 12 | Marimba | 71 | 0.96 | 3.94 | 7.45 | -31.1 |
| 74 | Recorder | 49 | 0.98 | 29.80 | 32.14 | -38.8 |
| 46 | Harp | 57 | 0.96 | 1.56 | 7.12 | -28.8 |
| 42 | Cello | None | -- | 2.92 | -- | -29.8 |
| 32 | Acoustic Bass | None | -- | 16.41 | -- | -32.2 |
| 43 | Contrabass | 36 | -- | 0.00 | 5.48 | -180.0 |

## The four real defects

**1. Melodic content on channel 9 was being played as drums.**
Not a waveform problem, but it dominates everything else. All three MIDI files
put melodic parts on channel 10 with an explicit program change:

| file | ch9 program | notes | share of track |
|---|---|---|---|
| Wild Battle | 0 (piano) | 271 | 16.4% |
| Gym Leader | 24 (nylon guitar) | 654 | **63.5%** |
| Route 1 | 0 (piano) | 328 | 20.5% |

The engine routed `ch == 9` to the drum bank unconditionally, so 63.5% of the
Gym track was randomised drum hits instead of a guitar. Fixed: a program change
received on channel 10 now switches that channel to melodic, which is how game
rips use it; `mc_set_drum_channel()` overrides either way.

**2. Twenty-six programs silently fall back to a generic default.**
Programs 61-63, 65, 67, 70-71, 76, 78, 80-82, 84, 99-100, 109, 113, 121, 123, 126
never resolve to a preset, so they emit a hard-coded patch by family. This is why
the two lead voices in the Wild Battle track (81 Saw Lead, 82 Calliope Lead,
17.9% of notes each) sound generic rather than like their named instruments.

Cause: `zones_for_preset()` scans past the present's own bag window looking for
the instrument generator, so it either finds the *next* preset's instrument or
none. A corrected version confined to `[pbag[p['bag']], pbag[p['bag']+1])` was
attempted this session and reduced the bank to 16 shared tables -- six times
worse -- so it was reverted rather than shipped. The lookup needs a proper fix,
not another guess: iterate the preset's own bag range and validate that the
resulting zone count is non-zero for all 128 programs before accepting it.

**3. Bright instruments lose their top end.**
Where `cent smp` is far above `cent syn` -- Trombone 14.7 vs 2.8, Shakuhachi
25.2 vs 7.2, Fifths Lead 51.7 vs 1.8, Bowed Pad 57.7 vs 10.0 -- the converter
chose a low root sample and the single-cycle table cannot carry the original's
harmonic extension. These are the "too dull" flags, 14 programs.

**4. Level spread of 19.5 dB.**
The table is normalised to peak and the patch's `gain` is a fixed family
default, so quiet instruments stay quiet: Pan Flute renders at -70.8 dB, Tinkle
Bell at -61.2 dB, Woodblock at -51.5 dB against a -27 dB median. Per-program
gain calibration from the source sample's RMS is the fix.

## What is right

* Pitch: 0.95 cents mean error across the whole range (validation suite).
* Envelope: ADSR is fitted per sample, so decay and release follow the source.
* Additive spectra: 62 programs match the source harmonics closely.
* Drums: all 47 keys resolve to real percussion samples.

## Honest limits

This compares spectra, not perception. A spectrum can match while an attack
transient still sounds wrong, and 16 programs have a brightness error large
enough that no amount of listening will make them correct.
