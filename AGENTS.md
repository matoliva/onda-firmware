# AGENTS.md

## Project

Onda Firmware is the embedded firmware for the Onda meeting recorder.

The firmware runs on:

- Board: Waveshare ESP32-S3-ePaper-1.54G
- SKU: 34586
- MCU: ESP32-S3-PICO-1
- Flash: 8 MB
- PSRAM: 8 MB
- Display: 1.54" 200×200 four-colour e-Paper
- Audio codec: ES8311
- Storage: microSD
- Connectivity: Wi-Fi + BLE
- Framework: ESP-IDF 5.5.x
- Target: `esp32s3`

The companion web application is a separate project.

---

## Product Goal

Onda is a dedicated meeting recording device.

The intended high-level flow is:

    User starts recording
        ↓
    Device captures audio
        ↓
    Audio is written safely to microSD
        ↓
    Recording finishes
        ↓
    Device connects to Onda over Wi-Fi
        ↓
    Audio is uploaded
        ↓
    Server handles transcription and meeting intelligence

The device should remain simple, reliable, and focused.

---

## Development Principles

Prefer:

- simple implementations
- explicit state
- small components
- predictable behaviour
- recoverable operations
- offline-first recording
- defensive error handling

Avoid:

- unnecessary abstractions
- premature optimisation
- large dependency trees
- dynamic allocation in critical recording paths when avoidable
- hidden global state
- blocking operations that could interfere with audio recording
- coupling hardware drivers directly to product logic

Reliability is more important than cleverness.

---

## Hardware Source of Truth

Never guess hardware details.

For board-specific information, use the official Waveshare repository and documentation as the source of truth.

Reference repository:

    waveshareteam/ESP32-S3-ePaper-1.54G

Relevant Waveshare ESP-IDF examples include:

    04_SD_Card
    05_WIFI_AP
    06_WIFI_STA
    07_Audio_Test
    08_BATT_PWR_Test
    09_E_Paper_Test

Use these examples to verify:

- GPIO assignments
- SPI configuration
- I2C configuration
- I2S/audio configuration
- ES8311 initialization
- microSD configuration
- display initialization
- power management

Do not invent pin numbers or peripheral configuration.

When adapting vendor code, bring only the minimum required implementation into Onda.

Do not copy entire example applications.

---

## Architecture

Keep product logic separate from hardware-specific code.

Prefer boundaries such as:

    main/
        application orchestration

    components/
        display/
        audio/
        storage/
        buttons/
        wifi/
        power/

Hardware components should expose small interfaces to application code.

For example:

    audio_start_recording()
    audio_stop_recording()

rather than exposing low-level I2S operations throughout the application.

Do not introduce components before they are needed.

---

## Application State

Device behaviour should eventually be modelled explicitly.

Expected states may include:

    BOOTING
    READY
    RECORDING
    SAVING
    SYNCING
    ERROR

Avoid distributing device state across unrelated booleans.

State transitions should be explicit and testable where practical.

---

## Audio

Audio recording is a critical path.

Priorities:

1. Do not lose recordings.
2. Write recording data safely to microSD.
3. Do not depend on network availability while recording.
4. Avoid unnecessary processing on the device.
5. Keep audio formats compatible with the Onda backend.

The server is responsible for:

- transcription
- summarisation
- meeting intelligence
- AI processing

Do not move AI workloads onto the ESP32 unless explicitly requested.

---

## Storage

microSD is the primary persistent storage for recordings.

Recording should work without Wi-Fi.

Prefer:

    record
        ↓
    persist locally
        ↓
    verify file
        ↓
    upload later
        ↓
    mark as synced

Do not delete a local recording merely because an upload request was initiated.

Deletion policies must be explicit and safe.

---

## Networking

Wi-Fi must not be required to record a meeting.

Network failures should not corrupt or interrupt locally stored recordings.

Keep network and upload logic separate from audio capture.

Use secure HTTPS communication with the Onda backend.

Do not hardcode:

- Wi-Fi credentials
- API credentials
- tokens
- production URLs
- secrets

---

## Display

The e-Paper display has slow refresh characteristics.

Avoid treating it like an LCD/OLED.

Minimise unnecessary refreshes.

The UI should communicate only important device state such as:

    Ready
    Recording
    Saved
    Syncing
    Synced
    Error

Do not continuously refresh the display for timers or animations unless specifically justified.

---

## Power

The device is battery powered.

Avoid unnecessary:

- display refreshes
- Wi-Fi activity
- polling
- CPU work

Power optimisation should not compromise recording reliability.

Implement advanced power management only after the core recording flow is reliable.

---

## Logging

Use ESP-IDF logging:

    ESP_LOGI
    ESP_LOGW
    ESP_LOGE

Prefer meaningful component tags.

Example:

    ESP_LOGI("ONDA_AUDIO", "Recording started");
    ESP_LOGE("ONDA_STORAGE", "Failed to open recording file");

Never log:

- credentials
- authentication tokens
- sensitive meeting content
- raw audio data

Logs should help diagnose hardware and state transitions.

---

## Error Handling

Hardware operations can fail.

Handle failures explicitly, particularly for:

- microSD initialization
- file writes
- audio initialization
- Wi-Fi connection
- upload requests
- memory allocation

Do not silently ignore ESP-IDF error codes.

Prefer `esp_err_t` where appropriate.

Use `ESP_ERROR_CHECK` only when crashing/restarting is genuinely the correct behaviour.

Recover gracefully when practical.

---

## Configuration

Prefer committed defaults through:

    sdkconfig.defaults

Avoid relying on undocumented local `menuconfig` changes.

The board has 8 MB flash and 8 MB PSRAM.

Project configuration should reflect the actual hardware.

Do not commit machine-specific configuration or credentials.

---

## Development Workflow

Read:

    docs/development.md

before changing build, flash, ESP-IDF, or hardware configuration.

Typical validation:

    get_idf
    idf.py build

Hardware changes should also be validated on the physical device when possible.

Do not claim hardware behaviour has been verified unless it was actually tested on the board.

---

## Feature Documentation Lifecycle

Use numbered feature documents to separate intended work from accepted outcomes.

Before implementation:

    plans/NNN-feature-name.md

Plans contain the objective, scope, requirements, non-goals, validation steps, and completion criteria for proposed or active work.

After implementation and all required validation:

    decisions/NNN-feature-name.md

Replace the completed plan with a decision record in the same change. Preserve the feature number and slug, and remove the corresponding file from `plans/`.

A decision record must state:

- status and completion date
- context and problem
- accepted implementation or behaviour
- validation evidence
- consequences and constraints for later work
- follow-up work that remains out of scope

Do not migrate a hardware-dependent plan until it has been tested on the physical device. A successful build alone is insufficient.

Decision records are durable project history. Do not rewrite them as future plans or reuse their feature numbers. Update them only to correct evidence or record a deliberate change to the accepted outcome.

The next feature must have a plan before implementation begins.

---

## Testing

Separate what can be tested without hardware from what requires the device.

Prefer unit tests for:

- state transitions
- filename generation
- metadata handling
- protocol parsing
- pure business logic

Hardware integration requires physical-device validation.

Do not create mocks merely to achieve meaningless coverage.

---

## Security

Treat recordings as sensitive data.

Never:

- expose recordings publicly
- log recording contents
- embed production credentials in firmware
- trust server responses blindly
- disable TLS verification in production

Device authentication will be implemented explicitly as a dedicated feature.

Do not invent an authentication mechanism before that feature is designed.

---

## Scope Discipline

Implement only the requested feature.

Do not opportunistically add:

- Bluetooth features
- OTA updates
- cloud provisioning
- speaker identification
- local AI
- complex power management
- custom protocols
- additional dependencies

unless required by the current plan.

When hardware behaviour is uncertain, inspect the official Waveshare implementation before coding.

---

## Current Development Order

The expected implementation sequence is:

    001 board bring-up
    002 display
    003 buttons
    004 microSD
    005 audio recording
    006 recording UX
    007 Wi-Fi
    008 Onda API integration
    009 device authentication
    010 upload and sync
    011 power management

Do not implement later phases implicitly while working on an earlier phase.

---

## Definition of Done

For each feature:

1. The implementation follows existing architecture.
2. ESP-IDF build succeeds.
3. Relevant automated tests pass.
4. Errors are handled explicitly.
5. No credentials or machine-specific values are committed.
6. Documentation is updated when behaviour or setup changes.
7. Hardware-dependent behaviour is tested on the physical device when applicable.
8. No unrelated functionality is introduced.
9. The completed plan is replaced by a decision record with validation evidence.
