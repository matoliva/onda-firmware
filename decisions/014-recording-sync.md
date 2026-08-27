# Decision 014 — Offline Recording Sync

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-27

## Context and problem

Onda records meetings offline on the microSD card. Phase 013 gave each
recording a durable ID and metadata sidecar, but there was no safe way to send
those recordings to the BFF after connectivity returned. Synchronization must
be explicit, retry-safe, bounded in memory, and must never remove the local
backup.

## Accepted implementation

- A BOOT long press requests synchronization only from `READY`. The application
  enters `SYNCING`, ignores recording and conflicting sync inputs, refreshes
  the e-paper at sync start and at the final result, and returns to `READY`
  after the result is shown for three seconds.
- `components/recording_sync` discovers pending metadata and processes one
  recording at a time. Record-specific failures leave that record pending and
  the worker continues; unavailable Wi-Fi/BFF transport and 401/403 failures
  stop the run.
- The authenticated BFF protocol is exactly:

      POST /api/device/recordings/initiate
        → signed Blob PUT
        → POST /api/device/recordings/complete

  The device validates the response schemas and the server-generated RFC 3339
  `syncedAt`. An already-uploaded initiate response is success without a new
  Blob PUT. A recording becomes `synced` only after that response or a valid
  complete response.
- The device sends its bearer token only to the BFF. The Blob request uses only
  the headers and URL issued by initiate and streams raw WAV bytes from the SD
  card through a fixed buffer. The Blob client's transmit-request buffer is
  sized for the bounded signed URL.
- Phase 013 sidecars are upgraded on first sync by measuring the closed WAV and
  persisting `sizeBytes` atomically before initiate. Metadata updates for
  `sizeBytes`, `meetingId`, `syncedAt`, and `status` are published atomically.
  Malformed sidecars are rejected, and legacy WAV-only files remain excluded.
- Recording capture is capped at two hours of mono 16-bit 16 kHz PCM
  (`230,400,000` PCM bytes); the final write is clipped safely before WAV
  finalization, remaining below the 256 MiB upload limit.
- The production BFF reserves deterministic Blob pathnames (random suffixes
  disabled) so complete can inspect the exact object uploaded by the device.
  Its successful initiate/already-uploaded and complete responses include the
  authoritative `syncedAt` value. The companion fix was merged in Onda AI PR
  #25.

## Validation evidence

- `source ~/.espressif/v5.5.5/esp-idf/export.sh && idf.py build` completed
  successfully for the production-configured firmware.
- ESP-IDF Unity test firmware builds completed for `audio_recorder`,
  `device_ui`, `onda_api`, and `recording_metadata`.
- The firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G on
  `/dev/cu.usbmodem1101`. Physical boot logs verified SD mount/read checks,
  Wi-Fi connection, BFF identity authentication, and TLS certificate
  validation.
- The physical production sync was confirmed end to end: initiate returned
  HTTP 200, the signed Blob PUT returned HTTP 200, complete returned
  successfully, and the recording became visible in the application. The
  local WAV and sidecar were retained.
- The initial production failures were diagnosed without logging credentials or
  audio: the firmware transmit buffer was increased for long signed URLs and
  the BFF deterministic pathname fix was deployed and validated.

## Consequences and constraints

- Onda remains offline-first. Pending and synced WAV files stay on the SD card;
  this phase adds no cleanup, deletion, quota, or remote-delete policy.
- Retries reuse the same recording ID and restart at initiate. There is no
  resumable upload, background retry loop, or parallel upload.
- A successful Blob PUT alone is not synchronization success. BFF complete is
  the source of truth for creating the meeting and for the exact `syncedAt`.
- Display updates are intentionally sparse because the e-paper panel is slow
  and battery powered.
- Local development/production BFF URL and token selection remains a
  build-time choice in the ignored `onda_api_config_local.h`; no credential is
  exposed in firmware UI, logs, or tracked source.

## Follow-up work

- Exercise the remaining fault-injection matrix (lost complete response,
  interrupted Blob transfer, reboot recovery, and already-uploaded recovery)
  on hardware as additional confidence tests.
- Add explicit storage cleanup/retention policy only in a dedicated phase.
- Keep legacy WAV-only migration and resumable uploads out of scope unless a
  future plan adopts them.
