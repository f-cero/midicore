# The SYN2 bank format

A bank is a flat little-endian blob with **no pointers and no relocations**, so
it can be `mmap`'d from flash and used in place. Byte order is fixed; the target
endianness does not matter because the header is read field by field.

## Header (32 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `"SYN2"` |
| 4 | 2 | `version` | 1 |
| 6 | 2 | `headerBytes` | 32 |
| 8 | 2 | `numTables` | wavetables |
| 10 | 2 | `numPatches` | melodic + drum patches |
| 12 | 4 | `tableOff` | byte offset |
| 16 | 4 | `patchOff` | byte offset |
| 20 | 4 | `totalBytes` | must equal the file size |
| 24 | 4 | `crc32` | CRC-32 of everything after the header |
| 28 | 4 | `reserved` | 0 |

## Wavetable block

`numTables * 256 * int16` — one complete cycle per table, index 0 = phase 0.
Amplitude may reach ±32767. Tables are read with linear interpolation; a voice
whose patch sets `MC_F_NOINTERP` uses nearest-neighbour instead (cheaper, grittier).

## Patch block

`numPatches * 64` bytes. Patch `i` is:

* `0 .. 127` — GM melodic program `i`
* `128 + n` — drum key `n` (so index 256 is key 128)

### Patch (64 bytes)

| Off | Field | Range | Meaning |
|---|---|---|---|
| 0 | `opCount` | 1..4 | operators actually used |
| 1 | `algorithm` | 0..3 | see below |
| 2 | `flags` | | `1` drum, `2` one-shot, `4` no-interp |
| 3 | `velDepth` | 0..127 | velocity → level |
| 4 | `keyTrack` | 0..127 | level attenuation per octave |
| 5 | `filterMode` | 0..3 | 0 off, 1 LP, 2 HP, 3 BP |
| 6 | `filterRes` | 0..127 | |
| 7 | `filterCut` | 0..127 | log cutoff, 0 ≈ 20 Hz, 127 ≈ 20 kHz |
| 8 | `filterEnvAmt` | 0..127 | envelope sweep in cutoff units |
| 9 | `filterKeyTrk` | 0..127 | |
| 10 | `lfoMode` | 0..3 | 1 vibrato, 2 tremolo, 3 both |
| 11 | `lfoRate` | 0..127 | |
| 12 | `lfoDelay` | 0..255 | 4 ms units |
| 13 | `lfoVib` | 0..127 | depth, ~1/32 semitone per step |
| 14 | `lfoTrem` | 0..127 | |
| 15 | `gain` | 0..127 | patch output level |
| 16 | `pan` | -64..63 | 0 = centre |
| 17 | `pitchEnvAmt` | -24..24 | semitones at note-on |
| 18 | `pitchEnvRate` | 0..127 | how fast it settles |
| 19 | `pad0` | | 0 |
| 20 | `pad1` | u32 | 0 |
| 24 | `op[4]` | 4 × 10 bytes | |

### Operator (10 bytes, at `24 + 10*k`)

| Off | Field | Range | Meaning |
|---|---|---|---|
| 0 | `table` | 0..numTables-1 | wavetable index |
| 1 | `mul` | 0..15 | frequency multiple, 0 means 0.5x |
| 2 | `level` | 0..127 | FM index when modulating, amplitude when audible |
| 3 | `keyScale` | 0..127 | level attenuation per octave |
| 4 | `detune` | -8..7 | sixteenths of a semitone |
| 5 | `attack` | 0..127 | |
| 6 | `decay` | 0..127 | |
| 7 | `sustain` | 0..127 | |
| 8 | `release` | 0..127 | |
| 9 | `out` | 0..127 | output level; **0 means modulator only** |

### Algorithms

| Value | Name | Routing |
|---|---|---|
| 0 | ADD | all operators sum into the output |
| 1 | FM2 | `op1 → op0`; op0 audible, op2 (if any) additive |
| 2 | STACK | `op3 → op2 → op1 → op0`; only op0 audible |
| 3 | PAIR | `(op1 → op0) + (op3 → op2)` |

Phase modulation depth, in cycles: `(sample × level) / (32768 × 32)` — so
`level = 127` at full amplitude is about ±2 cycles.

### Envelope rates

`r` maps to a time constant of `0.002 × 2^((127-r)/9)` seconds. 127 ≈ 2 ms,
0 ≈ 36 s. Attack is a linear ramp over that time; decay and release are
one-pole exponentials. Because it is a single curve, one controller sweep feels
even across its whole range.

## Minimal writer

See `tools/sf2_to_bank.py` (`write_bank`) — 20 lines, CRC included. A writer in
another language only needs `zlib.crc32` and `struct.pack`.

## Versioning

Any layout change bumps `version`, and `mc_bank_open()` rejects the old one.
There is no negotiation and no migration path: regenerate the bank.
