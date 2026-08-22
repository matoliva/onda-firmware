# Decision 001 — Board Bring-Up Baseline

- **Status:** Accepted and physically validated
- **Date:** 2026-08-22

Onda uses a minimal, reproducible ESP-IDF baseline for the Waveshare
ESP32-S3-ePaper-1.54G. The baseline proves the MCU and installed memory before
later hardware features are introduced.

## Context

The repository began as the ESP-IDF `hello_world` example. Its generated local
configuration targeted 2 MB flash, did not enable PSRAM, and restarted
continuously. That was not a reliable base for the actual ESP32-S3-PICO-1-N8R8
hardware.

## Decision

| Area | Decision |
|------|----------|
| Framework | ESP-IDF 5.5.x, validated with 5.5.5 |
| Target | `esp32s3` |
| Flash | 8 MB |
| PSRAM | 8 MB octal PSRAM at 80 MHz |
| Configuration | Commit required settings in `sdkconfig.defaults`; ignore generated `sdkconfig` |
| Application | Keep `app_main` minimal and use the `ONDA` ESP-IDF log tag |
| Runtime checks | Verify ESP32-S3, flash size, PSRAM initialization, and PSRAM size before reporting success |
| Failure behaviour | Log explicit errors and do not report board bring-up as complete |

## Implementation

- The CMake project and application binary are named `onda_firmware`.
- `main/onda_main.c` logs the target, core count, silicon revision, flash size,
  and PSRAM size.
- The inherited countdown, restart loop, `printf` logging, template test, and
  generic README were removed.
- No display, buttons, storage, audio, networking, or power feature was added.

## Validation evidence

The firmware was built with ESP-IDF 5.5.5, flashed through the board's Espressif
USB Serial/JTAG interface, and monitored on the physical board.

Observed results:

- ESP32-S3-PICO-1, silicon revision v0.2
- embedded GD 8 MB flash
- embedded AP 3.3 V 8 MB octal PSRAM at 80 MHz
- ESP-IDF PSRAM memory test passed
- flash writes completed with image hash verification
- two consecutive software reset and boot cycles completed without a crash or
  reset loop
- Onda logged `Flash: 8 MB`, `PSRAM: 8 MB`, and
  `Board bring-up complete`

## Consequences

- Future phases can rely on the verified MCU and memory baseline.
- Hardware-specific configuration must remain reproducible through committed
  defaults rather than undocumented local `menuconfig` state.
- Changing the MCU, flash, PSRAM mode, or memory size requires a new decision or
  an explicit update to this record with new physical validation evidence.

## Follow-up

The next feature is Phase 002: e-Paper display integration. It requires its own
plan under `plans/` before implementation begins.
