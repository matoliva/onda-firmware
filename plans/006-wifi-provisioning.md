# Phase 6 — Wi-Fi Provisioning

## Objective

Implement the complete Wi-Fi experience for the Onda device.

This phase should allow Onda to:

* connect automatically to a previously configured Wi-Fi network
* enter setup mode when no credentials exist
* receive Wi-Fi credentials through BLE provisioning
* persist credentials securely
* expose connection state to the application
* communicate Wi-Fi state through the e-Paper display
* allow the user to reset and reconfigure Wi-Fi

Recording must remain available when the device is offline.

## High-Level Flow

### First boot

```text
No Wi-Fi credentials
        ↓
SETUP_REQUIRED
        ↓
BLE provisioning starts
        ↓
Phone provides SSID + password
        ↓
Credentials stored
        ↓
CONNECTING
        ↓
CONNECTED
        ↓
READY
```

### Subsequent boots

```text
Boot
  ↓
Load stored credentials
  ↓
CONNECTING
  ↓
CONNECTED
  ↓
READY
```

### Connection unavailable

```text
CONNECTING
    ↓
bounded retries
    ↓
OFFLINE
```

The device must remain usable while offline.

## Hardware

Target board:

* Waveshare ESP32-S3-ePaper-1.54G
* SKU: 34586
* MCU: ESP32-S3-PICO-1
* Wi-Fi + BLE
* ESP-IDF 5.5.x

Use official Waveshare and ESP-IDF documentation as the source of truth.

Prefer ESP-IDF's existing Wi-Fi provisioning facilities rather than creating a custom provisioning protocol.

## Requirements

### Wi-Fi Connectivity

* Use Wi-Fi Station mode.
* Connect automatically when valid stored credentials exist.
* Detect IP acquisition.
* Detect disconnections.
* Implement bounded retries and reasonable backoff.
* Expose connection state to application code.
* Do not block application startup indefinitely.

### Provisioning

When no Wi-Fi credentials exist:

* enter Wi-Fi setup mode
* start BLE provisioning
* allow a phone to provide SSID and password
* attempt to connect using the supplied credentials
* persist credentials only through an appropriate secure configuration mechanism
* stop provisioning after successful configuration

Use ESP-IDF Wi-Fi Provisioning Manager if appropriate.

Do not invent a custom BLE protocol unless the official provisioning solution cannot satisfy the requirement.

## Wi-Fi State

Model network state explicitly.

At minimum:

```text
UNCONFIGURED
PROVISIONING
CONNECTING
CONNECTED
OFFLINE
ERROR
```

Keep this state separate from the recording state machine.

Conceptually:

```text
Recording state       Network state

READY                 CONNECTED
RECORDING              OFFLINE
SAVING                 CONNECTING
```

Recording and networking must remain independent.

## Persistence

Wi-Fi configuration must survive reboot.

Use ESP-IDF-supported persistent storage, such as NVS, where appropriate.

Do not store credentials in source code.

Do not commit credentials to Git.

Do not expose credentials through application logs.

## Device UI

Use the existing display abstraction.

The e-Paper should communicate important Wi-Fi states without excessive refreshes.

### Setup Required

Conceptually:

```text
ONDA

Wi-Fi setup

Connect your phone
to configure Onda
```

### Provisioning

```text
ONDA

Setting up Wi-Fi...
```

### Connecting

```text
ONDA

Connecting...
```

### Connected

The normal ready screen may include a subtle connection indicator.

Conceptually:

```text
ONDA

Ready

Wi-Fi connected
```

### Offline

```text
ONDA

Offline

Recording available
```

Exact visual implementation should follow the existing Onda device UI.

Do not introduce animations or frequent display refreshes.

Refresh only when meaningful state changes occur.

## Reconfiguration

The user must be able to intentionally clear the current Wi-Fi configuration and enter provisioning mode again.

Use an existing physical button interaction where appropriate.

A possible interaction is:

```text
long press while not recording
        ↓
request Wi-Fi reset
        ↓
clear stored credentials
        ↓
enter provisioning mode
```

The exact button behaviour must be validated against the existing button architecture and board behaviour.

Do not allow Wi-Fi configuration reset while recording.

Avoid accidental credential deletion.

## Recording Independence

Wi-Fi availability must never determine whether recording is available.

The following must remain valid:

```text
CONNECTED  + READY      → recording allowed
OFFLINE    + READY      → recording allowed
CONNECTING + READY      → recording allowed
```

If Wi-Fi disconnects during recording:

* audio capture must continue
* the recording state must not change
* networking may reconnect independently

## Architecture

Keep responsibilities separated.

Conceptually:

```text
application
    │
    ├── recording state
    │
    └── network state
            ↓
      wifi component
            ↓
      provisioning
            ↓
    ESP-IDF Wi-Fi / BLE
```

Application code should not depend on low-level ESP-IDF event IDs.

Prefer a small network abstraction that exposes meaningful application-level state and operations.

## Security

* Never hardcode Wi-Fi credentials.
* Never log passwords.
* Do not expose Wi-Fi passwords through BLE after provisioning.
* Do not disable transport security for convenience.
* Do not introduce Onda backend credentials in this phase.
* Do not implement device authentication yet.
* Use official ESP-IDF provisioning security mechanisms where available.

Document the provisioning security mode selected and why.

## Error Handling

Handle at least:

* no stored credentials
* incorrect credentials
* Wi-Fi network unavailable
* connection timeout
* unexpected disconnect
* provisioning failure
* BLE provisioning failure
* credential persistence failure

Errors should leave the device in a recoverable state.

Do not crash or restart repeatedly because networking is unavailable.

## Logging

Use concise logs such as:

```text
ONDA_WIFI: Initializing
ONDA_WIFI: No credentials found
ONDA_WIFI: Provisioning started
ONDA_WIFI: Credentials received
ONDA_WIFI: Connecting
ONDA_WIFI: Connected
ONDA_WIFI: IP acquired
ONDA_WIFI: Disconnected
ONDA_WIFI: Entering offline mode
```

Never log passwords or sensitive provisioning payloads.

## Non-goals

Do not implement:

* Onda backend communication
* HTTP API calls
* device authentication
* recording upload
* cloud sync
* OTA updates
* captive portal
* custom mobile application
* Wi-Fi network management UI beyond provisioning
* automatic firmware updates
* Bluetooth features unrelated to provisioning

## Validation

### Build

```bash
get_idf
idf.py build
```

### Physical Device

Validate at least:

1. Boot with no stored Wi-Fi credentials.
2. Confirm provisioning mode starts.
3. Confirm the display shows setup state.
4. Provision Wi-Fi credentials through BLE.
5. Confirm the device connects successfully.
6. Confirm credentials survive reboot.
7. Confirm subsequent boot connects automatically.
8. Disable the Wi-Fi access point.
9. Confirm the device enters offline state gracefully.
10. Confirm recording remains available while offline.
11. Restore Wi-Fi and verify reconnect behaviour.
12. Trigger the Wi-Fi reset/reconfiguration flow.
13. Confirm credentials are cleared.
14. Provision a different network.
15. Confirm BOOT/download mode still works normally.

## Completion Criteria

This phase is complete when:

1. Onda connects automatically with stored credentials.
2. A device with no credentials enters provisioning mode.
3. Wi-Fi credentials can be provisioned through BLE.
4. Credentials persist across reboot.
5. Connection and provisioning states are represented explicitly.
6. Important network states are reflected in the e-Paper UI.
7. Connection retries are bounded.
8. Offline operation remains fully usable for recording.
9. Wi-Fi can be intentionally reset and reconfigured.
10. Credentials are never hardcoded, committed, or logged.
11. Existing display, button, audio, and recording functionality remains stable.
12. The firmware builds and runs successfully on the physical board.

## Implementation Instructions

Before implementation:

1. Read `AGENTS.md`.
2. Read `docs/development.md`.
3. Inspect the existing application, recording, display, and button architecture.
4. Inspect the official Waveshare Wi-Fi STA example.
5. Review the ESP-IDF 5.5 Wi-Fi Provisioning Manager and BLE provisioning APIs.
6. Verify how credentials are persisted by the selected ESP-IDF provisioning approach.
7. Determine the provisioning security mode.
8. Determine the safest existing button interaction for Wi-Fi reset.
9. Propose a short implementation plan before modifying code.

Keep networking independent from recording.

Do not implement Onda backend communication as part of this phase.
