# Decision 002 — e-Paper Display Integration

- **Status:** Accepted and physically validated
- **Date:** 2026-08-22

Onda initialises the Waveshare ESP32-S3-ePaper-1.54G display after the board
bring-up checks have succeeded, renders one static ready screen, and then puts
the panel into deep sleep.

## Context

The firmware had a validated MCU and memory baseline but did not own the
board's display pipeline. The Waveshare `09_E_Paper_Test` example proves the
panel hardware but mixes its low-level transport with demo images, repeated
refreshes, long delays, and abort-on-error paths that are unsuitable for Onda.

## Decision

| Area | Decision |
|------|----------|
| Application API | `display_init()` and `display_show_ready()` return `esp_err_t` |
| Component boundary | All EPD power, SPI, GPIO, BUSY, framebuffer, and rendering details remain in `components/display` |
| Initial screen | A white 200×200 screen with black `ONDA` and `Hardware ready` text |
| Refresh behaviour | Render and perform one full refresh during startup; no demo loop, animation, or timer refresh |
| Idle behaviour | Send the panel to deep sleep and switch off its active-low power rail after a successful refresh |
| Errors | Log with `ONDA_DISPLAY`, return errors to `main`, and avoid a reset or crash |

## Implementation

- The component uses the Waveshare board's EPD power, SPI, data/command,
  reset, and BUSY configuration from the official ESP-IDF example.
- A static 10,000-byte framebuffer represents 200×200 pixels at four pixels
  per byte. Only white and black are used in this first Onda screen.
- The display driver waits for BUSY release with a bounded timeout rather than
  the vendor example's unbounded wait.
- The display transport and its Font12 asset retain the relevant third-party
  attribution and licensing information.
- `main/onda_main.c` runs the new display flow only after all existing board
  bring-up validation has passed.

## Validation evidence

The firmware was built with ESP-IDF 5.5.5, flashed to the physical
ESP32-S3-ePaper-1.54G through `/dev/cu.usbmodem11101`, and monitored after a
USB Serial/JTAG reset.

Observed results:

- Flash writes completed with image hash verification.
- Board bring-up logged the expected ESP32-S3 target, 8 MB flash, and 8 MB
  PSRAM before display startup.
- The physical device logged `Display initialised`, `Rendering ready screen`,
  `Display ready`, and `Onda ready`; the complete display path took about 21
  seconds from boot.
- The user visually confirmed that the e-Paper screen displayed correctly.
- No demo image cycle, crash, or reset was observed.

## Consequences

- Product code uses a small display abstraction instead of direct EPD calls.
- Future device states must explicitly reinitialise the panel before rendering
  because the Phase 002 ready screen leaves it asleep and unpowered.
- This phase intentionally adds no buttons, interactive UI, recording status,
  networking, or partial-refresh behaviour.

## Follow-up

Phase 003 will address buttons. Future display states belong to the dedicated
recording UX phase rather than this display bring-up feature.
