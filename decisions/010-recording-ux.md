# Decision 010 — Recording UX and Reliability

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-24

## Context and problem

Phase 009 safely produced one deterministic `/sdcard/test.wav`, but it did not
provide meeting-oriented file organisation, user-visible finalisation, saved
feedback, or recovery from a missing SD card. Onda needs an offline recording
flow that never overwrites a completed recording and clearly communicates the
state of storage and Wi-Fi without treating the e-Paper display as a live
screen.

## Accepted implementation

- `recording_naming` is a small pure component that chooses recording paths.
  It uses UTC `YYYY-MM-DD/HH-MM-SS.wav` paths only when the system clock is
  plausible (2024–2100); otherwise it safely falls back to
  `/recordings/unknown-date/recording-NNNN.wav`. Timestamp collisions get a
  bounded `-NNNN` suffix.
- `storage` creates the required recording directories, uses exclusive staging
  creation, and publishes only to a destination that does not already exist.
  The recorder verifies the closed staging and final files contain exactly a
  WAV header plus nonzero PCM data before reporting success.
- `audio_recorder` accepts prepared staging/final paths and returns structured
  completion metadata: result category, error, PCM byte count, and final path.
  The fixed-size task and audio buffers from Decision 009 remain unchanged.
- The application state machine now has `FINALIZING` and `SAVED` between
  `RECORDING` and `READY`, alongside separate storage, audio, and generic
  error states. BOOT is ignored while finalizing; a static 20-second Saved
  timer returns to Ready without timer redraws. BOOT in a storage error
  retries mount and verification, then returns to Ready on success.
- The UI displays concise Saving, Saved, No SD Card, and Audio Error screens.
  Saved uses PCM duration and the final basename. The top bar orders SD,
  battery, and Wi-Fi indicators. SD is `SD` when available and crossed when
  unavailable/error; Wi-Fi and storage status changes refresh outside active
  recording, while updates remain suppressed during recording.

## Validation evidence

- `get_idf && idf.py build` completed successfully with ESP-IDF 5.5.5. The
  final application image is `0x13fc70` bytes and leaves `0xc0390` bytes (38%)
  free in the 2 MB application partition.
- Focused ESP-IDF Unity test firmware builds completed for `recording_naming`,
  `audio_recorder`, and `device_ui`, covering UTC/fallback paths, collision
  suffixes, duration formatting, WAV size validity, downmixing, UI mappings,
  and refresh policy.
- Firmware was flashed repeatedly to `/dev/cu.usbmodem1101`; esptool verified
  written image hashes. Serial monitoring confirmed SD mount and verification,
  ES8311 initialization, normal Ready rendering, and Wi-Fi connection with IP
  `192.168.0.42`.
- The user completed and confirmed manual validation of the recording flow,
  finalization/Saved feedback, unique file behaviour, playback, SD retry,
  timeout, and final status-bar layout.

## Implementation issues resolved during validation

- The Wi-Fi icon initially stayed crossed after the device connected because
  the accepted primary-state-only refresh policy retained the initial offline
  status. Status-only refreshes now occur outside `RECORDING`, so the icon
  reflects a successful connection without interrupting capture.
- The first SD-available marker used a literal `V`, which looked like a broken
  replacement for the battery icon. The accepted layout restores the battery
  indicator and uses only `SD` / crossed `SD` for storage.
- The first three-icon layout was visually unbalanced. The accepted geometry
  uses a smaller SD footprint and balanced SD-to-battery and battery-to-Wi-Fi
  gaps for the 200-pixel header.

## Consequences and constraints

- Recording remains fully offline and does not configure Wi-Fi, NTP, or a
  timezone. The device will normally use unknown-date sequential paths until a
  later feature supplies a trustworthy clock.
- Completed recordings are never replaced or deleted by this feature. Failed
  normal attempts remove only their incomplete `.part` file; power-loss
  recovery remains unimplemented.
- Battery remains `UNKNOWN` until the planned power-management phase provides
  a measured value. It is retained in the status bar for a stable layout.

## Follow-up work

- Add durable power-loss recovery and recording metadata only through future
  planned features.
- Add time synchronisation, upload/sync, authentication, and deletion policy
  only in their dedicated phases.
