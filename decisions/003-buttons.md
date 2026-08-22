# Decision 003 — Button Input

- **Status:** Accepted and physically validated
- **Date:** 2026-08-22

Onda exposes BOOT and PWR as active-low physical inputs through a small
`buttons` component. The component emits short- and long-press events to the
application, which currently logs them only.

## Context

The validated board and display baseline had no product-facing input boundary.
The Waveshare examples demonstrate the board's button GPIOs but combine them
with event groups, polling, double-click actions, and power control that do not
belong in Onda's input foundation.

## Decision

| Area | Decision |
|------|----------|
| Supported inputs | BOOT on GPIO0 and PWR on GPIO18 |
| Electrical behaviour | Active-low GPIO inputs with pull-ups |
| Public API | `buttons_init(handler, context)` delivers `button_event_t` values |
| Event types | One short-press or one long-press event per confirmed interaction |
| Timing | At least 15 ms debounce and a 1 s long-press threshold |
| Execution boundary | A GPIO-edge ISR wakes a worker task; callbacks never run in ISR context |
| Product behaviour | Application logs `ONDA_BUTTON` events only; it does not record, refresh the display, or change power state |
| BOOT safety | GPIO0 is configured only as an input and is never driven by application firmware |

## Implementation

- `components/buttons` contains the GPIO configuration, interrupt handling,
  debounce, press-duration tracking, and event dispatching.
- The worker task coalesces GPIO edge notifications, confirms a stable input,
  reports a long press after the threshold while held, and otherwise reports a
  short press on release.
- `main/onda_main.c` registers a logging callback after the existing display
  ready screen completes.
- Initialization errors are returned to `main` and logged instead of being
  silently ignored or crashing the firmware.

## Validation evidence

The firmware was built with ESP-IDF 5.5.5, flashed to the physical
ESP32-S3-ePaper-1.54G through `/dev/cu.usbmodem11101`, and monitored after the
Phase 003 integration.

Observed results:

- `idf.py build` completed successfully; the application image occupies
  `0x36b90` bytes of the `0x100000` app partition.
- Flash writes and hashes completed successfully after BOOT GPIO integration.
- Startup completed normally, including the existing display sequence, button
  initialization, and `Onda ready`.
- Physical BOOT and PWR short presses produced `ONDA_BUTTON` short-press logs.
- Physical BOOT and PWR holds produced `ONDA_BUTTON: BOOT long press` and
  `ONDA_BUTTON: PWR long press` logs.
- No power-state or recording behaviour was triggered by PWR or BOOT input.

## Consequences

- Future application states can consume explicit button events without knowing
  board GPIO details.
- PWR remains an input event only. Battery-rail control belongs to the later
  dedicated power-management phase.
- BOOT must remain input-only in later changes to preserve firmware recovery
  and download behaviour.

## Follow-up

Phase 004 will cover microSD storage. Recording controls and display state
changes remain out of scope until their dedicated phases.
