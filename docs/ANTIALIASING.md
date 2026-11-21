# Anti-aliasing: options considered

A stored cycle carries up to 96 harmonics. Played high, the ones above Nyquist
fold back as inharmonic energy. Measured on isolated notes, **mean 16.3 % of
total energy was aliased**, with bright instruments far worse:

| program | note | f0 | aliased energy |
|---|---|---|---|
| 74 Recorder | 96 | 2093 Hz | **85.2 %** |
| 48 Strings 1 | 100 | 2637 Hz | **75.1 %** |
| 99 Atmosphere | 117 | 7040 Hz | 1.0 % |
| 4 EPiano 1 | 100 | 2637 Hz | 1.2 % |

Metric: energy not within 3.5 % of any harmonic k·f0 below Nyquist, as a share
of total. That cannot come from the instrument — only from folding.

## Options

| option | aliasing | flash | RAM | CPU | verdict |
|---|---|---|---|---|---|
| **A. Do nothing** | 16.3 % mean, 85 % worst | 0 | 0 | baseline | unacceptable |
| **B. Runtime-derived mips** | **0.0 %** | **0** | ~150 KB | +~40 % measured | **implemented** |
| C. Build-time mips in bank | 0 % | +45 KB | 0 | 0 | best CPU, costs flash |
| D. 2x oversample + decimate | 0 % | 0 | small | ~2x | most CPU |
| E. Fewer harmonics everywhere | 0 % but dull | 0 | 0 | 0 | rejected, kills timbre |

C and D are projections from the measured per-sample cost, not implementations —
only A and B were built and measured.

### B — what was implemented

At init, `mc_build_mips()` takes each stored 256-point cycle, runs a DFT, keeps
only the first N harmonics, and resynthesises. Four levels: 96 (stored), 32, 8,
2 harmonics, covering roughly MIDI 84 / 96 / 108 and above. At note-on the
engine picks the finest level whose harmonic ceiling still fits under Nyquist
(`mc_pick_table()`), so the choice costs one compare per note and **nothing per
sample**.

Measured effect on the same ten test notes: **16.3 % → 0.0 %**.

Brightness at low pitch is unchanged, because low notes keep the full table:

| program | note | centroid | × f0 |
|---|---|---|---|
| 74 Recorder | 60 | 4662 Hz | 17.8 |
| 74 Recorder | 96 | 4185 Hz | 2.0 (only ~7 harmonics fit) |
| 48 Strings | 60 | 3916 Hz | 15.0 |

### The cost, honestly

* **RAM.** 303 levels × 512 B = **151 KB**. On an ESP32-S3 that belongs in PSRAM,
  not SRAM. `mc_build_mips()` fills whatever buffer you give it and stops when
  full, so a 64 KB budget simply derives fewer levels.
* **CPU.** 58 s of Route 1: **227 ms → 324 ms (+43 %)** on the host. Allocation
  and selection are free, so this is cache pressure — 150 KB of tables instead
  of 50 KB. On a target with a small L1 this matters more than on x86, so treat
  40 % as optimistic.
* **Init time.** The DFT is naive (O(N²) per table); 101 tables × 3 levels takes
  well under a second on a 240 MHz core.

## Recommendation

Ship B for targets with PSRAM, and offer C as the build-time alternative for
flash-rich, RAM-poor boards. The code path is identical — C just points
`mip[t][L]` at tables baked into the bank instead of derived ones, so switching
is a converter flag away.

If the +40 % CPU is unacceptable, the cheapest real win is to derive level 1
only (32 harmonics, 51 KB, covers MIDI 84+ where most of the damage is) and
accept a small residual at the very top.
