# Changelog

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
