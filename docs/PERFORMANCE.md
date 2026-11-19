# Performance

## Measured (host, x86-64, `-O2`)

Rendering 58 s of a 13-track MIDI at 32 kHz with 16 voices:

```
render wall time   227 ms
realtime factor    255x
```

Peak 0.628, RMS 0.099, zero clipped samples, zero silent seconds.

This number is not a target figure — it is a reference point. It tells you the
DSP is not doing anything pathological, and gives a baseline for comparing a
port.

## Arithmetic for a real target

Per sample, per operator: table fetch (2 loads), interpolation (1 multiply,
2 shifts), phase add, envelope step (1 multiply, 2 shifts), accumulate.
Call it **~18 cycles** on a 32-bit core with a barrel shifter. Voice overhead
(LFO, pan, mix) is ~20 cycles.

| Configuration | cycles/sample | at 32 kHz | at 240 MHz |
|---|---|---|---|
| 8 voices × 1 op | 304 | 9.7 M/s | 4 % |
| 16 voices × 1 op | 608 | 19.5 M/s | 8 % |
| 16 voices × 2 ops | 1216 | 38.9 M/s | 16 % |
| 32 voices × 2 ops | 2432 | 77.8 M/s | 32 % |

Leave half the core for the game and the second-chip protocol. That puts the
comfortable ceiling around **16 voices with 2-op patches**, which is what the
converter emits for bright instruments.

## Where the time goes

1. **Interpolation** — unavoidable, and the cheapest correct option.
2. **Envelope** — 3 ops per operator; keeping it in registers is why voices are
   a flat array of 60-byte structs.
3. **Cache** — a 256-entry `int16` table is 512 bytes; 100 tables are 50 KB, so
   a working set of concurrent patches stays in L1/L2.

## Knobs, in order of effect

1. `opCount = 1` — halves operator cost; the converter already does this for
   instruments with a flat spectrum.
2. Fewer voices — linear saving.
3. Lower sample rate — 22 kHz is 0.69x the work of 32 kHz.
4. `MC_F_NOINTERP` — drops the interpolating multiply; audibly grittier.
5. Turn the filter off — the SVF is ~8 ops/sample when enabled.

## Memory

| | |
|---|---|
| Bank, full GM | 66.5 KB (101 tables, 256 patches) |
| `mc_synth`, 16 voices | ~1.3 KB |
| `mc_synth`, 32 voices | ~2.6 KB |
| Song, 3000 events | 24 KB |

The bank can be memory-mapped, in which case its RAM cost is zero.
