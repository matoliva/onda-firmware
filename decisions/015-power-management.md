# Decision 015 — Battery Power Management

- **Status:** Accepted and manually validated
- **Completion date:** 2026-08-28

## Context and problem

Battery-only Onda operation needed an explicit way to retain battery power
after boot, enter manual deep sleep, turn off safely, and wake with the PWR
button without risking recordings or active synchronization.

## Accepted implementation

- The `power` component follows Waveshare's verified power circuit: it asserts
  `GPIO17` (`BAT_Control`) immediately at boot, uses `GPIO18` (`BAT_KEY`) as
  the deep-sleep wake source, and retains the required latch level through
  deep sleep.
- PWR short press from `READY` enters deep sleep while retaining the latch;
  PWR double press releases it for battery-only power-off and uses off-sleep
  when USB keeps the board powered.
- A three-second PWR hold preserves Wi-Fi credential reset and BLE
  reprovisioning. PWR-only double-press recognition is bounded to 300 ms, and
  the boot/wake PWR release is suppressed so it cannot become a later command.
- Power actions are queued through the application coordinator and ignored
  outside `READY`, protecting recording, finalization, saving, sync, and
  error work. Wi-Fi cleanup failures are logged but do not block the hardware
  power action.
- Phase 017 subsequently adds explicit e-Paper confirmation before the same
  accepted sleep/off actions; it does not change the hardware control or wake
  semantics established here.

## Validation evidence

- The integrated ESP-IDF firmware build completed successfully with ESP-IDF
  5.5.5.
- The user confirmed the Phase 015 physical-device scenarios passed: battery
  cold start, sleep/wake, battery power-off, PWR Wi-Fi reset, protection of
  active work, USB off-sleep fallback, and post-flash repeat starts.

## Consequences and constraints

- No automatic idle sleep, advanced power optimization, measured-current
  claim, or power-off during active work is introduced.
- A failed latch or deep-sleep operation remains an explicit error rather than
  falsely reporting a successful shutdown.
- USB serial flashing remains the recovery path if a physical wake issue is
  encountered.

## Follow-up work

- Add automatic power policy only after its interaction with recording
  reliability has a dedicated design.
- Keep battery voltage and charging/source feedback in their dedicated
  features.
