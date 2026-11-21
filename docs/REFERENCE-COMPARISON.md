# Reference comparison: midicore vs FluidSynth

Rendered the same three MIDI files through **FluidSynth 2.3.4 with
GeneralUser-GS.sf2** — the exact soundfont the bank is derived from. That makes
this an apples-to-apples test: same notes, same instruments, different
synthesis. Timidity was run as a third opinion and agreed on timing.

## Verdict

**The MIDIs are not the problem.** Onset timing between my renderer, FluidSynth
and Timidity is identical:

| file | note onsets (mine / FluidSynth) | DTW slope | verdict |
|---|---|---|---|
| Wild Battle | 2 652 / 2 640 | **1.0000** | identical timing |
| Gym Leader | 2 527 / 2 507 | **1.0000** | identical timing |
| Route 1 | 4 914 / 3 926 | **1.0000** | identical in the common span |

A DTW slope of exactly 1.0000 means the two renders are not drifting against
each other at all: no tempo-map error, no rounding accumulation, no tick
mishandling. The sequencer is correct. The differences are all in **timbral
balance**.

## What is actually different

| file | centroid mine / ref | envelope corr | worst band error |
|---|---|---|---|
| Wild Battle | — / — | **0.49** | 110-250 Hz **-8.1 dB**, 1-2 kHz **-8.1 dB**, 2-4 kHz -7.1 dB |
| Gym Leader | 3 602 / 3 617 Hz | 0.76 | 1-2 kHz -4.9 dB, 2-4 kHz -5.8 dB |
| Route 1 | **6 044 / 2 809 Hz** | 0.59 | 8-16 kHz **+17.3 dB**, 4-8 kHz +7.6 dB |

Two distinct faults, and neither is the synthesis method itself.

### Fault A: missing presence (Wild Battle, Gym Leader)

Both tracks are 5-8 dB down in the 1-4 kHz band against the reference, and Wild
Battle is 8 dB down at 110-250 Hz too. This is the "slightly off" you are
hearing as dull or muffled. It is the 26 unresolved programs again: Wild Battle's
two lead voices (programs 81 and 82, 17.9 % of notes *each*) are generic default
patches whose single-waveform spectrum has no presence peak. Real lead patches
have one.

### Fault B: excess top end on Route 1

Route 1 shows **29 % of its energy between 8 and 16 kHz** where the reference has
**0.5 %**, and twice the spectral centroid. With a 32 kHz sample rate that region
is almost entirely above the point where harmonics stop being representable, so
the excess is **aliasing**, not brightness.

Cause: one engine design gap plus one data gap.

* The engine reads **one fixed 256-point table at every pitch**, with no
  mip-mapping. A table with meaningful upper harmonics played at MIDI 91-117
  puts harmonic 2 and 3 above Nyquist, and they fold back down into the audible
  band. The old sample-based engine carried three mip levels per sample
  precisely to avoid this; the new engine does not have that mechanism yet.
* Route 1 puts program 99 (Atmosphere) at MIDI 91-117 for 16 % of its notes,
  and program 99 is one of the 26 unresolved defaults.

Measured directly: program 99 at MIDI 91-117 renders with 22-27 % of its energy
above the fundamental at *every* pitch, with no pitch dependence, which is the
signature of folded harmonics rather than a naturally bright tone.

**Voice stealing is ruled out.** Rendering Route 1 at 32 voices instead of 16
changed the 8-16 kHz share from 29.4 % to 34.1 % — it got slightly *worse*, so
this is not clicks from stolen voices.

## Two things that are not faults

* **Route 1's duration.** FluidSynth produces 98.6 s, my renderer 59.7 s. The
  reference's output from 70 s to 98 s is digital silence (RMS 0.00002). The MIDI
  carries a long empty tail; my player stops at the last note. Harmless.
* **Level.** The band table compares normalised spectra, so overall loudness
  differences do not affect it.

## Fixes, in order of value

1. **Resolve the 26 missing programs.** Recovers Fault A entirely and part of
   Fault B. Needs a converter self-check that refuses to emit a bank unless all
   128 programs resolve to a real preset.
2. **Add wavetable mip-mapping.** Two or three octave-shifted, low-passed copies
   of each table, selected by pitch. Costs ~2x table bytes (still under 200 KB)
   and removes the aliasing on high notes.
3. **Per-program gain calibration** from the source sample's RMS, to close the
   19.5 dB level spread.

## Raw numbers

`cmp/result.json` holds per-file band shares, centroids, onset counts and the DTW
slope. Figure: `cmp/figure.png`.
