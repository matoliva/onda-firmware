# Decision 017 — Power Feedback and Button Guide

- **Status:** Accepted and manually validated
- **Completion date:** 2026-08-28

## Context and problem

The e-Paper panel retained its prior `Ready` image during sleep and power-off,
which made a dormant device appear awake. The existing BOOT and PWR gestures
were also difficult to discover from the device itself.

## Accepted implementation

- `READY` permanently shows the complete English guide:
  `BOOT: rec / hold: sync`, `PWR: sleep / 2x: off`, and
  `PWR 3s: Wi-Fi reset`.
- The display contract now supports a third optional detail line for the Ready
  guide without changing existing screens.
- PWR short press first renders `Sleeping` / `Press PWR to wake`; PWR double
  press first renders `Powering off` / `Hold PWR to start`.
- The display worker sends a successful-render acknowledgement to the
  application coordinator. Only then does it stop Wi-Fi and enter deep sleep
  or release the battery latch, so the explicit state remains visible on the
  retained e-Paper display.
- Pending power transitions suppress competing Wi-Fi and battery redraws, and
  normal button handlers reject actions outside `READY`. A failed confirmation
  keeps the device awake and follows the existing error path.

## Validation evidence

- `idf.py build` completed successfully with ESP-IDF 5.5.5.
- The `device_ui` ESP-IDF Unity test firmware built successfully, covering the
  new screen mappings, Ready guide, and refresh suppression.
- The normal firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem1101`; esptool verified written image hashes and startup
  logs reached the Ready render.
- The user confirmed the Phase 017 manual-device validation scenarios passed,
  including Ready guidance, sleep/wake, power-off/off-sleep, active-work
  protection, and the three-second Wi-Fi reset gesture.

## Consequences and constraints

- Sleep and off are intentionally delayed until the 15–20 second e-Paper
  confirmation completes; this prioritizes unambiguous feedback over an
  immediate transition.
- No LED, audio, charging-state, active-source detection, automatic sleep, or
  new gesture was added.
- A retained `Sleeping` or `Powering off` image communicates the requested
  power state, but it cannot prove that the CPU is still active.

## Follow-up work

- Revisit faster feedback only if future hardware exposes a verified status
  LED or another immediate user-feedback channel.
