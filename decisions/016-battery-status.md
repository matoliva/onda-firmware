# Decision 016 — Battery Status Indicator

- **Status:** Accepted and manually validated
- **Completion date:** 2026-08-28

## Context and problem

The status bar battery icon was a placeholder containing `?`. Onda needed
minimal, truthful feedback about battery voltage without treating its slow
e-Paper panel as a continuously updating display or claiming charging state
that the hardware cannot expose to firmware.

## Accepted implementation

- `components/battery` reads the Waveshare battery divider through calibrated
  ADC1 channel 3 / GPIO4, averages eight samples, and converts the ADC voltage
  with the board's 2× divider ratio.
- The application reports four coarse initial bands: HIGH at or above 3900 mV,
  MEDIUM at or above 3700 mV, LOW at or above 3500 mV, and CRITICAL below
  3500 mV. The first valid reading establishes the level; later transitions
  require two consecutive samples.
- Onda samples before the first UI render and every 60 seconds outside
  recording and finalization. A stable battery-only change refreshes e-Paper
  outside `RECORDING`; no battery sample or refresh runs in the recording or
  finalization path.
- The existing status-bar outline displays a black fill for high, medium, and
  low voltage, and a red fill for critical voltage. An unavailable reading
  hides the icon; `?` is no longer rendered.
- The icon represents battery voltage whether USB is connected or not. It does
  not identify the active supply or show charging. The board's charge LED
  remains the charge-state feedback.

## Validation evidence

- `idf.py build` succeeded with ESP-IDF 5.5.5.
- The `battery` and `device_ui` ESP-IDF Unity test apps built successfully.
- The normal firmware was flashed to the physical ESP32-S3-PICO-1 at
  `/dev/cu.usbmodem1101`; esptool verified all image hashes and reset the
  board.
- The user manually validated the Phase 016 behavior on the physical device.

## Consequences and constraints

- Thresholds are conservative initial values and may only be tuned using
  physical voltage measurements.
- The current board revision has no firmware-readable VBUS or charger-status
  signal. Firmware must not infer charging or active source from ADC voltage
  or USB serial.
- Battery UI updates remain intentionally sparse to protect recording
  reliability and e-Paper responsiveness.

## Follow-up work

- Revisit charging/source UI only if a future hardware revision exposes a
  reliable VBUS or charger `STAT` signal to the ESP32.
- Record measured voltage data before changing the four initial thresholds.
