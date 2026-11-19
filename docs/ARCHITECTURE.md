# Architecture

## Layers

```
  midicore.h          public API
  ├─ api.c            lifecycle, channel state, sequencer
  ├─ smf.c            Standard MIDI File -> absolute sample frames
  ├─ synth.c          voice allocation, operator bank, mixing
  └─ mc_tables.c      rate/waveform tables, built once per sample rate
```

Only `mc_tables.c` contains floating point, and only inside `mc_tables_init()`,
which runs a handful of `pow`/`exp` calls at startup. After that everything is
integer, so results are bit-identical on a soft-float part and an FPU part.

## The voice

60 bytes:

```c
mc_opv op[4];        // 4 x 24: phase, inc, vibScale, env, sus, stage
const mc_patch* patch;
uint32_t age, lfoPhase, lfoInc;
uint16_t lfoDelay, vibActive;
int32_t  pitchEnv, pitchEnvStep, f1, f2;
uint8_t  active, released, ch, note, vel, opCount;
```

Allocation policy: free voice oldest-first, else the oldest released voice, else
steal the oldest sounding one. No voice stealing hysteresis — a game knows its
own polyphony better than a heuristic does.

## Per-sample work

For each active voice, for each sample:

1. Advance the LFO, apply vibrato to the phase increment.
2. For each operator: read the wavetable with linear interpolation, advance
   phase, step the envelope.
3. Route by algorithm (FM adds a phase offset to the carrier before the read).
4. Pan, accumulate into the stereo mix.

A table read is two loads, one multiply, one shift:

```c
idx = phase >> 24;  a = t[idx];  b = t[(idx+1) & 255];
fr  = (phase >> 12) & 0xFFF;
out = a + ((b - a) * fr >> 12);
```

4096 interpolation steps per cycle is far below the ear's threshold, and it
keeps the multiply inside `int32` — no 64-bit math in the hot loop.

## Envelope

`env` is Q16, 0..65536.

* attack: `env += attInc[rate]`
* decay/release: `env -= (env >> 8) * decMul[rate] >> 8` — a one-pole
  exponential done in two shifts so nothing overflows
* sustain: hold at `sustain * 65536 / 127`

A voice dies only when **every** operator has released to zero, so patches with
different release rates ring out naturally.

## Sequencer

`smf.c` converts every event to an absolute sample frame at load time, resolving
the tempo map and merging tracks. `mc_player_render()` then dispatches events in
64-frame sub-blocks — 2 ms at 32 kHz — which is finer than any MIDI file's own
resolution and needs no timer, no task and no mutex.

Looping a song is a pointer reset to zero, so a game can loop a battle theme
forever without re-parsing anything.

## Memory

| Item | Bytes |
|---|---|
| `mc_synth` (32 voices max) | ~2.2 KB |
| Mix buffers | 2 KB |
| Bank (mapped, not copied) | 66 KB |
| Song events | 8 bytes each, PSRAM-friendly |

The bank is read-only and never mutated: point `mc_load_bank()` straight at a
memory-mapped flash region and physical RAM cost is zero.

## Concurrency

None. The engine is single-threaded by construction. Call `mc_player_render()`
from one audio task; everything else (`mc_note_on`, `mc_set_master_gain`) must
come from that same task, or be queued by your own code. There are no locks
because there is no shared mutable state to protect.
