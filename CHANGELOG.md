# Changelog

## 1.1.0
* Added runtime band-limited mip levels (`mc_build_mips`, `mc_pick_table`).
  Aliasing on high notes measured **16.3 % -> 0.0 %**, at the cost of ~150 KB RAM
  and ~40 % CPU (cache pressure). No change to the bank format or its size.
* `tools/alias_test.py`: measures inharmonic energy on isolated notes.
* `docs/ANTIALIASING.md`: the options considered, with numbers.


## 1.0.2
* Fixed: melodic content on channel 10 was always rendered with the drum bank.
  A program change on channel 10 now switches that channel to melodic, which is
  how game rip MIDI files use it. Added `mc_set_drum_channel()`.
* Added `tools/compare_bank.py`: per-program spectral comparison against the
  source sample. Added `docs/SAMPLE-COMPARISON.md`.
* Known defect: 26 programs do not resolve to a preset and use a default patch.


## 1.0.1
Fixes found by the new validation suite (`tools/validate.py`).

* `note_inc` kept only whole Hz, putting low notes up to 49 cents flat.
* Velocity gain was computed then discarded; velocity had no effect.
* Velocity multiply overflowed int32 at low velocities.
* Channel pan (CC10) was stored but never applied to the mixer.
* FM phase offset overflowed uint32 on loud modulators.
* Bank packer wrote every field from byte 3 onward one byte late.
* Added: 8-test regression suite, `docs/VALIDATION.md`.


## 1.0.0
First working release.

* SYN2 bank format: 32-byte header, 512-byte wavetables, 64-byte patches.
* 4-operator voice: additive / 2-op FM / 4-op stack / twin pair.
* ADSR in Q16, one-pole exponential decay and release.
* Optional 2-pole state-variable filter, vibrato, tremolo, pitch envelope.
* SMF parser resolving tempo to absolute sample frames, formats 0/1/2.
* Sequencer dispatching in 64-frame sub-blocks.
* `sf2_to_bank.py`: harmonic re-synthesis with phases, ADSR fit, attack
  brightness detection.
* Host renderer sharing the exact target code path.
