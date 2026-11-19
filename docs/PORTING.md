# Porting

## Requirements

* C99, `int16_t`/`int32_t`/`int64_t` of the usual widths
* `memcpy`, `memset`, `calloc`, `free`
* `pow`/`exp`/`sin` — **init only**, so a soft-float libm is fine

No OS, no HAL, no stdio in the library. The four `.c` files never include a
platform header.

## Two things your project must supply

1. **A bank.** Build it with `tools/sf2_to_bank.py`, ship it in flash.
2. **An output.** I2S, DAC, PWM — whatever; the engine hands you interleaved
   stereo `int16`.

## Wiring it into an existing project

Copy `include/` and `src/`, add the four `.c` files to your build, add
`include/` to the include path. That is the whole integration.

```c
mc_synth* s = mc_create(32000, 16);
if (mc_load_bank(s, bank, bank_len) != MC_OK) { /* log mc_last_error(s) */ }
mc_song* g = mc_song_load(midi, midi_len, 32000);
mc_player_start(s, g);
```

If your game already has a mixer, use `mc_render()` and mix the buffer yourself
instead of the player.

## Choosing a sample rate

| Rate | Bandwidth | Voice cost | Use |
|---|---|---|---|
| 22050 | 11 kHz | lowest | chiptune, GBA-ish |
| 32000 | 16 kHz | balanced | **default recommendation** |
| 44100 | 22 kHz | ~1.4x of 32k | SNES-ish, more CPU |

All wavetables hold 96 harmonics; above ~11 kHz those harmonics alias, so
22050 is honest for bright patches and 32000 keeps them clean. The engine does
not resample tables, so pick one rate per build.

## Polyphony

`mc_create(rate, voices)` fixes polyphony at startup; voices are a static array,
never grown. Budget roughly:

```
voice cost  ≈  ops × ~18 cycles + ~20 cycles of voice overhead
16 voices × 2 ops × 38  ≈ 1200 cycles per sample
at 32 kHz          ≈ 38 Mcycles/s
```

On a 240 MHz core that is ~16 % — leave the rest for the game. Drop to 8–12
voices and `opCount = 1` patches for a heavier frame.

## Flashing the bank

The bank is read-only, so map it rather than copying it:

```c
#include "esp_partition.h"
const esp_partition_t* part = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, 0x40, "bank");
esp_partition_mmap_handle_t h;
const void* ptr; size_t len;
esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &h);
mc_load_bank(s, ptr, len);       /* zero RAM */
```

If you would rather keep it on the SD card, read it into PSRAM once at boot —
66 KB is nothing, unlike the 9.4 MB a sample bank would need.

## Real-time discipline

* Call `mc_player_render()` from exactly one task.
* Do not `malloc` in that task; the library never does after `mc_create`.
* The song object lives in PSRAM (`smf.c` uses plain `calloc`; override
  `MC_ALLOC`/`MC_FREE` if you need a specific heap).
* Keep the block you render at least as large as your I2S DMA buffer, and
  double-buffer.

## Bring-up checklist

1. Host render a file — confirms bank + MIDI parse.
2. `mc_active_voices()` during playback — confirms events arrive.
3. `mc_get_stats()` peak — 0 means no audio reaching the mix.
4. I2S sine test — confirms the output path independently.
