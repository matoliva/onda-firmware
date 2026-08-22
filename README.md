# Onda Firmware

Embedded firmware for the Onda meeting recorder.

## Hardware

- Waveshare ESP32-S3-ePaper-1.54G, SKU 34586
- ESP32-S3-PICO-1-N8R8
- 8 MB flash
- 8 MB octal PSRAM
- 1.54-inch 200x200 four-colour e-Paper display
- ES8311 audio codec
- microSD storage

Board-specific configuration must be verified against the
[official Waveshare repository](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54G).

## Requirements

- macOS
- ESP-IDF 5.5.x; the current development version is 5.5.5
- USB data cable
- Physical board for flash and hardware validation

## Build

```bash
get_idf
idf.py build
```

The committed `sdkconfig.defaults` is the reproducible configuration baseline.
The generated `sdkconfig` and `build/` directory are intentionally ignored.

## Flash and monitor

```bash
ls /dev/cu.*
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Exit the monitor with `Ctrl + ]`.

See [docs/development.md](docs/development.md) for the complete local workflow.

## Current phase

Development follows the ordered plans in `plans/`. The initial implementation is
[Phase 001 — Board Bring-Up](plans/001-board-bring-up.md). Later hardware and product
features must not be introduced implicitly during an earlier phase.

## Validation

Every change must build successfully with ESP-IDF 5.5.x. Hardware-dependent work
must also be verified on the physical device; a successful build alone is not proof
of correct hardware behaviour.
