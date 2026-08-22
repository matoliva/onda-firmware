# Phase 1 — Board Bring-Up

**Status:** Complete — validated on the physical board on 2026-08-22.

## Objective

Establish the first working Onda firmware running on the physical Waveshare ESP32-S3-ePaper-1.54G.

This phase validates the project structure, ESP-IDF configuration, hardware target, logging, and basic firmware lifecycle.

The goal is not to implement product functionality yet.

## Hardware

Target board:

- Waveshare ESP32-S3-ePaper-1.54G
- SKU: 34586
- MCU: ESP32-S3-PICO-1
- Flash: 8 MB
- PSRAM: 8 MB
- ESP-IDF: 5.5.x
- Target: `esp32s3`

Use the official Waveshare repository and documentation as the source of truth for board-specific configuration.

Do not guess hardware settings.

## Requirements

- Configure the project for `esp32s3`.
- Configure the project for the board's actual 8 MB flash.
- Ensure PSRAM is correctly detected and available.
- Replace the inherited `hello_world` behaviour with a minimal Onda entry point.
- Add a clear startup log indicating that Onda firmware has booted.
- Log basic device information useful during development.
- Keep the implementation minimal and independent from future device features.
- Ensure the firmware builds cleanly using ESP-IDF 5.5.x.
- Flash and validate the firmware on the physical board.

## Expected Runtime Behaviour

On boot, the firmware should produce concise logs similar to:

    ONDA: Starting Onda firmware
    ONDA: ESP32-S3 detected
    ONDA: Flash: 8 MB
    ONDA: PSRAM: 8 MB
    ONDA: Board bring-up complete

Exact wording is not important.

The purpose is to confirm that the correct hardware is running the Onda firmware successfully.

## Project Cleanup

Remove unnecessary `hello_world` template behaviour and files where appropriate.

Keep only files that are useful to the Onda firmware project.

Do not introduce the display, audio, microSD, Wi-Fi, buttons, or power-management implementation in this phase.

## Configuration

Prefer committed configuration through `sdkconfig.defaults` where appropriate.

The project configuration must reflect:

- ESP32-S3 target
- 8 MB flash
- 8 MB PSRAM

Avoid relying on undocumented local `menuconfig` changes.

## Logging

Use ESP-IDF logging.

Prefer a clear application tag such as:

    ONDA

Do not use `printf` for application logging when ESP-IDF logging is appropriate.

## Non-goals

Do not implement:

- e-Paper display
- buttons
- microSD
- audio
- recording
- Wi-Fi
- BLE
- Onda API communication
- device authentication
- uploads
- power optimisation
- OTA
- AI functionality

## Validation

Automated:

    get_idf
    idf.py build

Physical-device validation:

1. Put the board into BOOT mode if necessary.
2. Flash the firmware.
3. Open the serial monitor.
4. Confirm Onda startup logs.
5. Confirm the firmware boots without crashes or reset loops.
6. Confirm the expected flash and PSRAM configuration.

### Validation evidence

The Phase 1 firmware was flashed and monitored on the physical Waveshare board
over its Espressif USB Serial/JTAG interface.

Observed hardware and runtime results:

- ESP32-S3-PICO-1, silicon revision v0.2
- embedded 8 MB flash detected
- embedded 8 MB octal PSRAM detected at 80 MHz
- ESP-IDF PSRAM memory test passed
- bootloader loaded the factory application successfully
- Onda reported `Board bring-up complete`
- two consecutive software reset and boot cycles completed without a crash or
  reset loop

The flash operation completed with image hash verification. No display, audio,
storage, networking, or other later-phase peripheral was initialized.

## Completion Criteria

This phase is complete when:

1. `onda-firmware` builds successfully.
2. The firmware flashes successfully to the physical board.
3. The board boots into Onda firmware.
4. Startup logs are visible in the serial monitor.
5. 8 MB flash configuration is correct.
6. 8 MB PSRAM is detected correctly.
7. No unnecessary `hello_world` behaviour remains.
8. No future hardware features have been introduced.
9. Documentation remains consistent with the implementation.

## Implementation Instructions

Before implementation:

1. Read `AGENTS.md`.
2. Read `docs/development.md`.
3. Inspect the current ESP-IDF project.
4. Inspect the official Waveshare configuration where hardware details are required.
5. Propose a short implementation plan before modifying code.

Keep this phase intentionally small.

Do not begin display integration as part of board bring-up.
