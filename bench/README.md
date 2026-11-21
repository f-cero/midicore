# midicore benchmark (ESP32-S3)

Auto-starts on boot, prints a full report to **Serial @115200**, then repeats
every 12 s forever. Nothing to press, no monitor needed at reset — attach at
any time and you will catch a complete run within 12 s.

## Flash it

**PlatformIO (from this folder):**

    pio run -t upload --upload-port COM5      # or /dev/ttyACM0

**Raw image (no toolchain):**

    esptool write_flash 0x0 midicore-bench-merged.bin

Both assume the board is a **16 MB flash / 8 MB octal PSRAM** N16R8 module. On
a different module, rebuild with the matching `board_build.arduino.memory_type`.

## What it measures

Nine configurations, each rendering the real 58-second Route 1 MIDI from
flash with 16 voices. Time is read from the CPU cycle counter `CCOUNT`:

* **clean cost** — interrupts disabled, 8 x 250 ms of audio, total cycles
  divided by total frames. This is the engine's own cost with no noise.
* **jitter** — 256 blocks timed individually with interrupts live; the worst
  block is reported. This is what an I2S DMA callback actually sees.
* **stress** — the same synth with 16 and 32 distinct simultaneous notes held,
  which is worse than any real song.

| config | what it isolates |
|---|---|
| A no-mips flash-bank | the shipped path before this change |
| B no-mips ram-bank | how much of A's cost is flash/cache, not synthesis |
| C mips-1 ram | only 32-harmonic level derived, ~51 KB |
| D mips-3 ram | **the proposed default**, ~151 KB |
| E mips-3 psram | D with the tables in octal PSRAM |
| F / G | same at 44.1 kHz |
| H / I | 16 and 32 voices held, worst case |

## How to read it

* `%core` — share of one 240 MHz core. Under ~25 % leaves a game plenty.
* `xRT` — how many seconds of audio per second of CPU.
* `dl%` — worst block as a percentage of the 4 ms DMA deadline at 32 kHz.
  Anything approaching 100 % is a click.
* `MIPS COST` — the delta between A and D: this is the number you wanted.
* `PSRAM PENALTY` — if this is large, keep the mip tables in internal SRAM.
* `INTEGRITY` — a hash of 30 s of rendered PCM. It must equal the host
  reference `0xE9D05B19` printed on the same line; if it does, the on-chip
  engine produced bit-identical audio to the x86 build.

## Sending me the log

Copy everything between the `====` rules. Two runs are enough (the second
shows repeatability); more is fine.
