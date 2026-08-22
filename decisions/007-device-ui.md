# Decision 007 — Device UI System

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-22

## Context and problem

The earlier display integration exposed screen-specific APIs directly to the
application. Wi-Fi and recording state changes could therefore cause whole
slow e-Paper refreshes without a shared product layout or explicit policy for
suppressing non-meaningful updates.

## Accepted implementation

- `components/device_ui` owns the declarative UI contract: primary state
  (`READY`, `RECORDING`, `WIFI_SETUP`, `ERROR`), coarse Wi-Fi status, coarse
  battery status, and the provisioning PoP.
- `main` retains authoritative application and Wi-Fi state in its existing
  command/one-item display queues. The UI policy refreshes only on an initial
  visible state or a primary-state transition; Wi-Fi and battery-only changes
  are retained for the next meaningful render.
- `components/display` renders a generic fixed layout: `ONDA` at upper left,
  Wi-Fi and battery indicators at upper right, centred primary content, and
  optional contextual lines. It uses the board's 2-bit framebuffer for black,
  white, yellow, and red.
- Recording has a red semantic accent and does not update while capture is
  active. Wi-Fi setup remains actionable and displays the PoP. Connecting,
  retries, and offline status have no dedicated flashing screens.
- Battery remains `UNKNOWN` in production until Phase 011 supplies a measured
  coarse level. `SAVED`, Booting, storage, timers, and a debug-preview mode
  remain out of scope.
- Table-driven ESP-IDF Unity tests cover UI mapping, battery-level mapping,
  and refresh suppression. `docs/development.md` documents the Unit Test App
  build workflow.

## Validation evidence

- `get_idf && idf.py build` completed successfully with ESP-IDF 5.5.5.
- The `device_ui` Unity test firmware built successfully through ESP-IDF's Unit
  Test App using `IDF_EXTRA_COMPONENT_DIRS`.
- Firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem11101`; the device connected and completed its Ready render.
- The user manually verified the status-bar layout, revised mobile-style Wi-Fi
  icon, and the remaining Phase 007 real-device scenarios successfully.

## Consequences and constraints

- Application code must not compose display geometry or call the e-Paper
  transport directly; new product states extend `device_ui` first.
- Status-only changes must not bypass the refresh policy.
- Future storage may add `SAVED` only after successful file verification.
- Phase 011 is responsible for providing real battery levels, not for changing
  the UI contract or display transport.

## Follow-up work

- Implement verified local recording persistence before introducing `SAVED`.
- Implement battery measurement and map it to the accepted coarse battery
  states in Phase 011.
