# midicore benchmark — Waveshare ESP32-P4-Pico

Native **ESP-IDF** project (Waveshare ships P4 support only for IDF; Arduino
adaptation for this chip is documented as incomplete). Auto-starts, prints a
report to the **USB-Serial-JTAG console** over the Type-C port, repeats every
12 s forever.

## Board assumptions

| | |
|---|---|
| Module | ESP32-P4NRW32X |
| CPU | dual-core RISC-V, 360 MHz (400 MHz capable) |
| SRAM | 768 KB |
| PSRAM | 32 MB stacked, hex mode |
| Flash | 32 MB NOR |

These are set in `sdkconfig.defaults`. If your board differs, change them there
rather than in `menuconfig`, so the file stays the source of truth.

## Build and flash

    . $IDF_PATH/export.sh          # ESP-IDF v5.5 or newer
    idf.py set-target esp32p4
    idf.py build
    idf.py -p /dev/ttyACM0 flash monitor

On Windows the port is usually `COM5` or similar. The console is
USB-Serial-JTAG, so baud rate is irrelevant — `idf.py monitor` handles it.

## Why this port differs from the ESP32-S3 build

* **Clock counter.** Xtensa `CCOUNT` does not exist on RISC-V; the code uses
  `esp_cpu_get_cycle_count()`, which reads the `mcycle` CSR.
* **No `%f`.** IDF's newlib nano formatting silently drops floats. All numbers
  are printed through integer formatters instead, so the log is trustworthy
  regardless of `CONFIG_NEWLIB_NANO_FORMAT`.
* **Watchdogs disabled.** The clean-cost pass masks interrupts for 250 ms at a
  time to keep the RTOS tick out of the measurement. Both watchdogs are off in
  `sdkconfig.defaults` for that reason. Do not copy this config into firmware.
* **Pinned to core 1**, so the numbers describe a core with nothing else on it.
* **`CONFIG_SPIRAM_IGNORE_NOTFOUND=y`** — if PSRAM fails to train, the board
  still boots and reports 0 KB instead of dying in the bootloader.

## Configs measured

| tag | isolates |
|---|---|
| A no-mips flash-bank | the shipped path before the anti-aliasing change |
| B no-mips ram-bank | how much of A is flash/cache, not synthesis |
| C mips-1 ram | 32-harmonic level only, ~51 KB |
| D mips-3 ram | **the proposed default**, ~151 KB |
| E mips-3 psram | D with tables in hex-mode PSRAM |
| F/G | D at 48 kHz and 44.1 kHz |
| H/I | 16 and 32 distinct voices held — worse than any real song |

## Reading the log

* `%core` — share of one 360 MHz core. On this chip expect low single digits.
* `xRT` — seconds of audio rendered per second of CPU.
* `dl%` — worst block as a share of the 4 ms DMA deadline at 32 kHz.
* `MIPS COST` — the A→D delta, i.e. what anti-aliasing costs you.
* `PSRAM PENALTY` — if large, keep the mip tables in SRAM; you have 768 KB.
* `INTEGRITY` — FNV-1a hash of 30 s of PCM. It must read `0xE9D05B19`, the
  x86 reference. A match means this build renders bit-identical audio.

Copy everything between the `====` rules; two runs is plenty.

## Console: where the log goes (read this if you see nothing)

The ESP32-P4-Pico has **two** USB connectors and they are not the same thing:

| Connector | What it really is |
|---|---|
| Type-C "UART flashing" port | Either a **CH343 USB-UART bridge on UART0** (as on the sibling P4-NANO) or the chip's **native USB-Serial-JTAG** — the wiki does not state which for this board |
| 4-pin USB 2.0 HS port | The P4's **USB OTG High-Speed** peripheral, GPIO24/25 — never a console |

Flashing works over either one, which is exactly why "it flashed but prints nothing" happens: the download path and the console path are independent.

**This build prints to both at once.** Primary console is UART0 at 115200 (ROM banner included), with USB-Serial-JTAG mirrored as a secondary channel. Whichever port your board wires to the Type-C, one of the two catches the log.

### If you still see nothing, find the port in 20 seconds

1. Unplug everything, reconnect **only** the Type-C.
2. Identify what enumerated:

   * Linux: `lsusb | grep -i -E "ch34|cp210|303a"`
     * `1a86:55d3 ... CH343` → the Type-C is a **UART bridge**. Your terminal must be at **115200 8N1**.
     * `303a:1001 ... USB JTAG/serial debug unit` → the Type-C is **native USB-Serial-JTAG**. Baud is irrelevant, and `idf.py monitor` auto-detects it.
   * Windows: Device Manager → Ports. `USB-Enhanced-SERIAL CH343` vs `USB JTAG/serial debug unit`.
   * macOS: `ls /dev/cu.*` → `cu.usbserial-*` (bridge) or `cu.usbmodem*` (native).

3. Press **RST** and watch. The ROM banner (`ESP-ROM:esp32p4-...`) is emitted by the chip's mask ROM before any of our code runs, and it follows the **primary** console = UART0. Banner but no table → the app is faulting before the report; no banner at all → you are on the wrong port or a charge-only cable.

### Note on the interrupt-masking pass

The clean-cost timing pass masks interrupts for 250 ms at a time. UART output survives that; the USB-Serial-JTAG secondary channel is **non-blocking**, so bytes queued during a masked window can be dropped. Every report line is flushed *before* the next timing pass begins, and the whole report is printed after the passes finish, so the numbers are never cut mid-table. If a line ever looks truncated, it is the transport, not the measurement — re-run and it repeats every 12 s.
