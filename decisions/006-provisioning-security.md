# Decision 006 — Wi-Fi Provisioning and Security

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-22

## Context and problem

Onda must record independently of network availability, but users need a safe
way to provide Wi-Fi credentials and intentionally replace them. Application
code must not depend on ESP-IDF event IDs, credentials must never be logged,
and provisioning failures must leave the device recoverable.

## Accepted implementation

- `components/wifi` owns ESP-IDF Wi-Fi station mode, BLE Provisioning Manager,
  credential persistence, retry/backoff handling, and low-level events.
- The public Wi-Fi contract exposes only `UNCONFIGURED`, `PROVISIONING`,
  `CONNECTING`, `CONNECTED`, `OFFLINE`, and `ERROR` through an asynchronous
  callback. Recording remains independent of every network state.
- Provisioning uses ESP-IDF Security 1 over BLE. Each device creates one stable
  random eight-character proof of possession (PoP), displays it only during
  setup, and never writes it to serial logs.
- Existing credentials trigger automatic connection at boot. Failed connection
  attempts are bounded before entering Offline with periodic retry; credentials
  are never erased automatically.
- Holding PWR for three seconds while READY clears credentials and starts
  provisioning again. The action is ignored while recording or in application
  Error state.
- Normal development keeps flash encryption disabled to preserve ordinary
  download/BOOT workflows. `sdkconfig.secure.defaults` provides the controlled
  release configuration for flash encryption and encrypted NVS; do not run it
  on a routine development board because first secure boot changes eFuses.
- ESP-IDF Wi-Fi INFO logging is limited to avoid SSID exposure. Passwords,
  credentials, PoP values, and meeting content are not logged.

## Validation evidence

- `get_idf && idf.py build` completed successfully with ESP-IDF 5.5.5.
- Firmware was flashed and monitored on the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem11101`.
- BLE Security 1 provisioning was completed with Espressif's official mobile
  app using the displayed PoP. The device acquired an IP address and rendered
  its connected Ready state.
- Automatic reconnect after reboot, credential persistence, offline/retry
  behavior, intentional PWR re-provisioning, and recording availability across
  network states were manually verified successfully by the user.
- A controlled flash-encryption/eFuse procedure was deliberately not run on the
  development device; its release configuration remains a factory-only action.

## Consequences and constraints

- Future features consume `wifi_state_t` only; they do not use ESP-IDF Wi-Fi or
  provisioning event IDs directly.
- Wi-Fi failure cannot block, corrupt, or stop local recording.
- The displayed provisioning PoP is sensitive setup material and must not be
  copied into source, issue text, test fixtures, or logs.
- Backend authentication and upload/sync remain separate later features.

## Follow-up work

- Add Onda backend API integration, explicit device authentication, and upload
  sync in their dedicated phases.
- Use the secure build only through an approved factory/recoverable-device
  process.
