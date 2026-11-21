# midicore

A **pure-synthesis MIDI engine for embedded game consoles**. No PCM samples, no
filesystem assumptions, no floating point in the audio path. A whole General MIDI
bank is **66 KB**, and a 32 kHz voice costs a few hundred cycles.

Designed for targets in the Game Boy Advance / SNES class: a microcontroller
that renders audio while the main loop runs a game, with a second chip doing
the drawing.

```
  SD card ──► smf.c ──► player (absolute frame events)
                          │
                          ▼
   bank.syn ──► synth.c ──► int16 stereo ──► I2S / DAC
   (66 KB)      (4-op wavetable+FM, 16 voices)
```

| | |
|---|---|
| Bank size (full GM, 128 melodic + 128 drums) | **66.5 KB** |
| Equivalent sample bank | 9.4 MB |
| Source size (all four .c files) | 31 KB |
| RAM per voice | 60 bytes |
| Floating point in render path | **none** |

## Why not samples

A half-second sample at 22 kHz is 22 KB. Sixteen of them and the flash is gone.
This engine stores **one cycle** of each instrument instead — 256 points, 512
bytes — and synthesises the rest: phase accumulation for pitch, ADSR for
duration, and a second operator for the spectral movement a static cycle can't
express. Programs are 64 bytes.

## Quick start (PC)

```bash
python3 tools/sf2_to_bank.py GeneralUser-GS.sf2 bank.syn   # 66 KB
gcc -O2 -Iinclude -Isrc src/*.c examples/host_render.c -o host_render -lm
./host_render bank.syn song.mid out.wav 30 32000 16
```

`host_render` calls exactly the same API a firmware does, so if it works here it
works there.

## Quick start (embed)

```c
#include "midicore.h"

mc_synth* s = mc_create(32000, 16);          /* 16 voices */
mc_load_bank(s, bank_bytes, bank_len);       /* bank stays mapped, not copied */
mc_song* g = mc_song_load(midi_bytes, midi_len, 32000);
mc_player_start(s, g);

int16_t buf[256 * 2];
for (;;) {
  int done;
  mc_player_render(s, buf, 256, &done);      /* sequencer + synth in one call */
  i2s_write(buf, 256);                       /* your HAL */
}
```

`mc_player_render()` dispatches MIDI events and renders audio in 64-frame
sub-blocks, so timing is accurate to ~1.5 ms without a scheduler.

## API at a glance

| Function | Purpose |
|---|---|
| `mc_create(rate, voices)` | Allocate. Nothing else allocates on the audio path. |
| `mc_load_bank(s, ptr, len)` | Validate + map a `.syn`. Zero copy. |
| `mc_song_load(data, len, rate)` | Parse SMF, resolve tempo map to frames. |
| `mc_player_start / _render / _stop` | Sequencer. |
| `mc_note_on / _off / _program_change / _control_change / _pitch_bend` | Direct MIDI input. |
| `mc_render(s, buf, frames)` | Synth only, if you sequence elsewhere. |
| `mc_get_stats(...)` | Voice count, peak, call count. |

## The instrument bank

`tools/sf2_to_bank.py` reads a SoundFont and **measures** it rather than
sampling it:

1. FFTs the steady state, extracts harmonics 1..96 **with phases**, and
   re-synthesises one cycle. The timbre of the sample survives; its bytes do not.
2. Fits an ADSR to the sample's own amplitude envelope.
3. Compares the spectral centroid of the attack against the sustain. If the
   attack is brighter, it emits a **second operator** with a fast-decaying
   modulation index — that is what makes brass bite and strings swell.

Identical waveforms are deduplicated, so 128 presets collapse into ~100 tables.

## What is in the box

```
include/    midicore.h, mc_bank.h        public API + file format
src/        synth.c smf.c api.c mc_tables.c
tools/      sf2_to_bank.py  bank_info.py
examples/   host_render.c   esp32_sd_player/
port/esp32/ I2S output glue
docs/       FORMAT.md ARCHITECTURE.md PORTING.md PERFORMANCE.md
```

## Validation

The engine ships with its own regression suite: eight generated test MIDIs and a
validator that asserts properties on the rendered audio.

```bash
python3 tools/mk_tests.py build/tests && python3 tools/validate.py
```

Current: **24 of 24 checks pass**, pitch accurate to **0.95 cents mean / 2.7
cents worst**. The suite found six real bugs in 1.0.0 — see
[docs/VALIDATION.md](docs/VALIDATION.md).

## Anti-aliasing

A stored cycle holds 96 harmonics; played high, the excess folds back. Measured
on isolated notes: **16.3 % mean aliased energy, 85 % on a bright instrument at
MIDI 96**. `mc_build_mips()` derives band-limited levels at init from the single
stored cycle — **0.0 % aliasing, zero flash cost**, ~150 KB RAM and ~40 % CPU.
Options and trade-offs in [docs/ANTIALIASING.md](docs/ANTIALIASING.md).

## Waveform fidelity

`tools/compare_bank.py` measures every synthesised program against the sample it
came from: **62 of 128** match the source spectrum closely (cosine >= 0.95), 50
are partial, 16 are poor, and **26 do not resolve to a preset at all**. Details
and causes in [docs/SAMPLE-COMPARISON.md](docs/SAMPLE-COMPARISON.md).

## Status, honestly

Verified on host: builds warning-free, renders 58 s of MIDI in 227 ms
(**255x realtime**, 16 voices), no clipping, no silent gaps, no stuck notes.

Not verified: **real-time behaviour on an actual ESP32-S3.** The CPU figure
above is x86. See `docs/PERFORMANCE.md` for the arithmetic and the two knobs
(`SFP_MAX_VOICES`, block size) if you underrun.

## License

Apache-2.0. See `LICENSE`.
