# Phase 015 — Battery Power Management

## Objective

Make Onda reliable on battery-only power while preserving the existing
three-second PWR Wi-Fi reset gesture.

This phase adds explicit battery power latching, manual deep sleep, a safe
power-off action, and PWR wake-up. It does not add automatic idle sleep,
battery-charge measurement, or advanced power optimization.

## Hardware basis

The Waveshare ESP32-S3-ePaper-1.54G battery circuit uses:

- `GPIO17` (`BAT_Control`) to retain or release battery power.
- `GPIO18` (`BAT_KEY`) for the physical PWR button.

The implementation must follow Waveshare's ESP-IDF `08_BATT_PWR_Test` example:
assert `GPIO17` as soon as firmware begins running to retain power, and release
it for battery-only shutdown. `GPIO18` is an ESP32-S3 RTC-capable GPIO and may
be used as the low-level deep-sleep wake source.

## User interaction

| Input | State | Behaviour |
| --- | --- | --- |
| Hold PWR while off | Off, battery-only | The hardware supplies the board; firmware retains `GPIO17` during early boot. |
| PWR short press | `READY` | Enter manual deep sleep. |
| PWR short press | Deep sleep | Wake and run a clean boot. |
| PWR double press | `READY` | Release the battery latch and power off. If USB remains connected, enter deep sleep without retaining the battery latch. |
| Hold PWR for 3 seconds | `READY` | Preserve the existing Wi-Fi credential reset and BLE reprovisioning flow. |
| Any new PWR energy action | Recording, finalizing, saving, syncing, or terminal status | Ignore it; no recording or sync work may be interrupted. |

`BOOT` behaviour remains unchanged.

PWR double-press recognition must defer the PWR short action long enough to
distinguish a double press. A long PWR hold cancels any pending short press, so
holding PWR for three seconds cannot first enter sleep.

The PWR release that follows a battery boot or a PWR deep-sleep wake must be
ignored. It originates from the press used to establish or wake power, not an
intentional sleep request.

## Implementation

### Power component

Add a small `power` hardware component with a narrow interface:

- `power_init()` configures and asserts `GPIO17` before non-essential startup.
- `power_enter_sleep()` holds `GPIO17` high and enters deep sleep with a
  low-level `GPIO18` wake source.
- `power_release_battery_latch()` drops `GPIO17` for real battery power-off.
- `power_enter_off_sleep()` enters deep sleep while holding the latch low when
  USB prevents the immediate hardware cut-off.

Deep sleep must retain the configured `GPIO17` level through the ESP32-S3
GPIO deep-sleep hold APIs. Startup must set the output register high before
releasing any retained hold, avoiding a low pulse that could cut battery power.

### Button and application integration

- Extend the button component with a PWR-only double-press event and bounded
  300 ms double-press recognition.
- Suppress the initial PWR release if PWR was already held when button inputs
  initialize.
- Queue sleep and power-off commands through the existing application task;
  button callbacks must remain non-blocking.
- From `READY`, stop Wi-Fi/BLE provisioning activity before deep sleep. A
  cleanup failure is logged but cannot block the hardware power action.
- Preserve the current `PWR` three-second `wifi_request_reprovision()` mapping,
  including its existing `READY` state guard.
- Do not perform a display refresh solely for sleep or power-off: e-Paper
  retains its last image without power, and avoiding an extra refresh protects
  battery life and avoids delaying a hardware shutdown.

## Error handling and recovery

- Every GPIO, hold, wake-source, and Wi-Fi cleanup call returns `esp_err_t` and
  is logged with its component tag.
- A failed power-latch operation leaves the device running and enters the
  existing error state; it must not falsely report successful shutdown.
- Deep-sleep entry should not return. If it does, application code records an
  explicit error.
- Battery-only PWR double press may cut power before later code executes; this
  is normal. USB-powered PWR double press executes the off-sleep fallback.
- If a physical wake test fails, USB serial flashing remains the recovery path.

## Non-goals

- No automatic idle timeout or background sleep policy.
- No battery-voltage, charge-state, or low-battery UI.
- No power-off while recording, finalizing WAV data, or synchronizing.
- No change to BOOT recording/sync gestures or Wi-Fi reset semantics.
- No claim of measured current consumption until a physical measurement is
  performed.

## Validation

### Build and automated checks

- `source ~/.espressif/v5.5.5/esp-idf/export.sh && idf.py build`
- Verify the existing ESP-IDF Unity test apps still build where available.

### Required physical-device validation

1. Battery-only cold start: hold PWR, release it, and confirm Onda stays on and
   reaches Ready.
2. From Ready, short PWR: confirm deep sleep and that PWR wakes a clean boot.
3. From Ready, double PWR on battery: confirm actual loss of power.
4. From Ready, hold PWR three seconds: confirm Wi-Fi credentials are cleared
   and BLE provisioning appears; verify neither sleep nor power-off happened.
5. During recording, finalization, and sync: attempt short/double/three-second
   PWR inputs; confirm recording data and active work are not interrupted.
6. With USB connected: confirm double PWR enters the off-sleep fallback and PWR
   wakes it; disconnect USB afterward and verify battery does not remain latched
   on unintentionally.
7. Repeat at least one cold start and one deep-sleep wake after reflashing.

This hardware-dependent plan moves to `decisions/015-power-management.md` only
after the required physical-device validation is reported and recorded.
