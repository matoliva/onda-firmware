# Decision 005 — Recording State

- **Status:** Accepted and physically validated
- **Date:** 2026-08-22

Onda now has an explicit application state machine for recording. A BOOT short
press transitions between `READY` and `RECORDING`; audio capture is active only
while recording. Failures settle in terminal `ERROR` state for this phase.

## Context

The accepted audio-input feature exposed a bounded microphone diagnostic, but
its BOOT-controlled task did not establish a product-level recording state or
coordinate display state. Onda needs explicit, recoverable state transitions
before it can add persistent recordings.

The board's four-colour e-Paper panel requires a slow global refresh. Initial
physical validation showed that keeping the recording coordinator blocked while
rendering made an immediate stop request appear unavailable.

## Decision

| Area | Decision |
|------|----------|
| Application states | `READY`, `RECORDING`, and terminal `ERROR` are owned by the application coordinator in `main` |
| Recording control | BOOT short press toggles `READY`/`RECORDING`; BOOT remains input-only and PWR/long presses have no product action |
| Audio lifecycle | Entering `RECORDING` calls `audio_start()`; returning to `READY` calls `audio_stop()`; bounded PCM is read and discarded |
| Display API | `display_show_ready()`, `display_show_recording()`, and `display_show_error()` each render once and return the panel to sleep |
| Display concurrency | A display worker serializes global refreshes from a one-item latest-state queue; it cannot block state transitions or microphone shutdown |
| Error behaviour | Audio, command, or display failures log explicitly, attempt audio cleanup when needed, and enter `ERROR` |
| Persistence | No microSD, WAV data, metadata, duration tracking, upload, or network logic is included |

## Implementation

- `main/onda_main.c` replaced the Phase 004 diagnostic task with an application
  command queue and coordinator task. The coordinator accepts every queued BOOT
  short-press command, owns the state model, and continuously reads a fixed
  DMA-capable PCM buffer only while recording.
- The separate display task receives only the latest requested state while a
  refresh is in progress. This permits a stop command to close the microphone
  immediately even though the panel must finish its current global waveform
  before rendering the final screen.
- `components/display` shares the existing full-refresh path across the three
  static screens. It performs no timers, animations, partial refresh attempts,
  or panel polling beyond the established bounded busy wait.
- The Waveshare 1.54-inch four-colour G panel does not support partial refresh;
  global-refresh flicker and roughly 20-second display latency are hardware
  constraints. Display state may therefore lag the authoritative application
  state, but the final queued screen reflects that state.

## Validation evidence

The firmware was built with ESP-IDF 5.5.5, flashed to the physical
ESP32-S3-ePaper-1.54G via `/dev/cu.usbmodem11101`, and monitored.

Observed results:

- `idf.py build` completed successfully. The final application image occupies
  `0x47a30` bytes of the `0x100000` app partition.
- Flash writes and hashes completed successfully. Boot again reported the
  expected ESP32-S3-PICO-1, 8 MB flash, 8 MB PSRAM, display initialization,
  microphone readiness, button initialization, and the `Ready` screen.
- A physical BOOT short press transitioned `READY → RECORDING`, opened the
  ES8311 capture path, and queued the `Recording` screen.
- A second physical BOOT short press during the still-flashing `Recording`
  refresh transitioned `RECORDING → READY` and stopped audio immediately
  (`ONDA_AUDIO: Capture stopped`) before the first global display refresh had
  completed.
- The display worker subsequently completed its in-flight `Recording` refresh,
  then rendered the queued `Ready` screen. No crash, duplicate lifecycle error,
  or microphone leak was observed.

## Consequences

- Users do not need to wait for e-Paper flashing before stopping capture; the
  state transition and audio shutdown are responsive to BOOT input.
- The e-Paper cannot be an immediate status indicator on this board. Its screen
  can temporarily show the previous state until a global refresh completes.
- A future recording UX may need a faster separate indicator if immediate
  visual confirmation is required.
- `ERROR` has no recovery action in this phase; restart/recovery UX remains a
  later dedicated feature.

## Follow-up

Persistent microSD recording, recording duration, final user-feedback design,
and an immediate recording indicator remain separate future work.
