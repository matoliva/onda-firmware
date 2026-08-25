# Decision 013 — Recording Metadata Foundation

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-25

## Context and problem

Onda stores each meeting recording locally before any future synchronization.
Phase 010 added collision-safe WAV files but no durable identity or sidecar
metadata, so a later sync feature could not safely identify a recording across
reboots and retry attempts. This phase establishes that local foundation without
adding network behavior or changing the recording UX.

## Accepted implementation

- `components/recording_metadata` generates one immutable ID in the form
  `rec_` plus 32 lowercase hexadecimal characters from 128 bits of ESP32
  randomness before microphone capture begins.
- A small, synced `.json.seed` sidecar preserves the ID, WAV basename, and
  creation-time value before capture. Once the recorder has closed, verified,
  and published the WAV, the component atomically publishes the final sibling
  `.json` metadata and then removes its seed.
- Final metadata contains `id`, `file`, `createdAt`, `durationMs`,
  `status: "pending"`, `meetingId: null`, and `syncedAt: null`. A reliable NTP
  clock produces an RFC 3339 Pacific/Auckland timestamp; offline recordings
  explicitly store `createdAt: null`.
- On boot, metadata recovery scans only Phase 013 seed artifacts. It recreates
  metadata only for a valid finalized RIFF/WAVE file with a matching PCM payload,
  accepts a previously published valid JSON by removing its stale seed, and
  treats malformed metadata as a storage error. WAV-only recordings created
  before this phase remain untouched and are not future sync candidates.
- `storage` retains file ownership and now supplies bounded reads and regular
  recording-directory enumeration. The application stages metadata before
  `audio_recorder_start()` and enters Saved only after metadata publication;
  metadata failures use the existing storage-error state. No display, button,
  Wi-Fi, API, upload, or deletion behavior was added.

## Validation evidence

- `source ~/.espressif/v5.5.5/esp-idf/export.sh && idf.py build` completed
  successfully. The application image is `0x162580` bytes and leaves
  `0x9da80` bytes (31%) free in the 2 MB application partition.
- The focused ESP-IDF Unity Test App build for `recording_metadata` succeeded,
  compiling coverage for ID format, PCM-duration conversion, and reliable versus
  unreliable timestamp formatting.
- The firmware was flashed to `/dev/cu.usbmodem11101`; the Waveshare
  ESP32-S3-ePaper-1.54G mounted and verified its microSD card, initialized audio,
  buttons, display, Wi-Fi, time, and API components, and reached Ready.
- The user confirmed the manual physical-device validation passed, including the
  finalized recording metadata flow and recovery checks required by the plan.

## Consequences and constraints

- Recording remains fully offline-first. A valid WAV is never deleted by
  metadata creation, recovery, or failure handling.
- Metadata I/O runs outside active audio capture. A normal failed recording
  removes its seed; a sudden interruption leaves a recoverable seed until boot.
- Legacy WAV-only files intentionally have no generated retrofit ID or metadata.
- This phase deliberately creates no sync trigger, upload request, API contract,
  automatic retry, or storage cleanup policy.

## Follow-up work

- Define the BFF upload contract, idempotency response, and explicit success
  schema before implementing the later manual recording-sync feature.
- Add any recording retention or storage-pressure policy only in a dedicated
  phase.
