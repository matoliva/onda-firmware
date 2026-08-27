````md
# Phase 14 — Offline Recording Sync

## Objective

Synchronize locally stored Onda recordings with the BFF using the finalized three-step upload protocol:

    initiate
      ↓
    direct Blob PUT
      ↓
    complete

Onda is offline-first.

Recordings are always finalized and stored safely on the microSD card before synchronization is attempted.

Synchronization is triggered manually with a BOOT long press while the device is in `READY`.

This phase must preserve all local recordings after synchronization. Automatic cleanup or deletion is out of scope.

---

## Existing Behaviour

The device already supports:

- Wi-Fi connectivity
- NTP time synchronization
- authenticated communication with the Onda BFF
- microSD storage
- WAV recording
- safe WAV finalization
- date-based recording directories
- recording duration tracking
- BOOT short press to start recording
- BOOT short press to stop recording
- offline operation
- persistent recording metadata
- stable recording IDs

Device authentication already uses the existing Onda device Bearer token.

---

## User Interaction

Preserve the current BOOT behaviour:

    READY + short press
        ↓
    RECORDING

    RECORDING + short press
        ↓
    FINALIZING
        ↓
    SAVED

Add:

    READY + long press
        ↓
    SYNCING

Long press must only trigger synchronization from `READY`.

Synchronization must not start while:

- recording
- finalizing
- already syncing
- another exclusive operation is active

Recording must not start while synchronization is active.

---

## Recording Identity

Every recording must have a stable device-generated ID:

    rec_<32 lowercase hexadecimal characters>

Example:

    rec_00112233445566778899aabbccddeeff

The recording ID must remain unchanged across:

- reboots
- failed uploads
- Wi-Fi disconnections
- repeated synchronization attempts
- lost HTTP responses

Never generate a new recording ID because synchronization failed.

---

## Recording Constraints

The firmware and BFF share these limits:

### Audio format

    RIFF/WAVE
    PCM
    16 kHz
    mono
    16-bit

### Maximum duration

    7,200,000 ms
    2 hours

### Maximum file size

    268,435,456 bytes
    256 MiB

The firmware should prevent or safely stop recordings that would exceed these limits.

A recording approaching the limit must be finalized safely rather than allowed to become invalid or corrupt.

---

## Local Metadata

Each recording must retain persistent metadata on the SD card.

Conceptually:

```json
{
  "id": "rec_00112233445566778899aabbccddeeff",
  "file": "14-22-31.wav",
  "createdAt": "2026-08-25T14:22:31+12:00",
  "durationMs": 2542000,
  "sizeBytes": 81344044,
  "status": "pending",
  "meetingId": null,
  "syncedAt": null
}
````

If device time was not trustworthy when the recording was created:

```json
{
  "createdAt": null
}
```

Do not fabricate a recording timestamp.

After successful synchronization:

```json
{
  "status": "synced",
  "meetingId": "backend-meeting-id",
  "syncedAt": "2026-08-25T16:02:00+12:00"
}
```

Keep both the WAV and metadata file after synchronization.

---

# Synchronization Protocol

## Step 1 — Initiate Upload

Call:

```
POST <BFF_BASE_URL>/api/device/recordings/initiate
```

Headers:

```
Authorization: Bearer <DEVICE_TOKEN>
Content-Type: application/json
```

Body:

```json
{
  "recordingId": "rec_00112233445566778899aabbccddeeff",
  "createdAt": "2026-08-25T14:22:31+12:00",
  "durationMs": 2542000,
  "sizeBytes": 81344044
}
```

`createdAt` may be:

* a valid RFC 3339 timestamp with timezone
* `null` when device time was not reliable

Do not send:

* user ID
* device ID
* local filesystem paths
* device token in the body

Ownership is determined exclusively by device authentication.

---

## Initiate — Already Uploaded

The BFF may respond:

```json
{
  "recordingId": "rec_00112233445566778899aabbccddeeff",
  "meetingId": "uuid",
  "status": "uploaded",
  "created": false
}
```

This means the recording was already completed during a previous sync attempt.

In this case:

```
do not upload again
    ↓
persist meetingId
    ↓
mark local recording synced
```

This is a successful synchronization outcome.

---

## Initiate — Upload Required

The BFF may respond with an upload descriptor:

```json
{
  "recordingId": "rec_00112233445566778899aabbccddeeff",
  "upload": {
    "url": "https://...",
    "method": "PUT",
    "headers": {
      "x-vercel-blob-access": "private",
      "x-content-type": "audio/wav"
    },
    "expiresAt": "2026-08-25T..."
  }
}
```

The firmware must treat the returned upload information as authoritative.

Use:

* exactly the returned URL
* exactly the returned HTTP method
* exactly the returned upload headers

Do not construct Blob URLs manually.

---

# Step 2 — Upload WAV Directly to Blob

Perform:

```
PUT <upload.url>
```

using the headers returned by `initiate`.

Example:

```
x-vercel-blob-access: private
x-content-type: audio/wav
```

The request body must contain:

```
raw WAV bytes
```

Do NOT use:

* multipart/form-data
* JSON encoding
* base64
* BFF Bearer token

The device token must never be sent to Blob.

---

## Streaming Requirement

Recordings may be hundreds of MiB.

The firmware must stream the WAV from the SD card using bounded buffers.

Conceptually:

```
microSD
  ↓
read chunk
  ↓
HTTP PUT
  ↓
Blob
  ↓
repeat
```

Never load the complete recording into:

* RAM
* PSRAM

Use a fixed-size buffer appropriate for the ESP32 and HTTP stack.

The implementation should remain suitable for the full 256 MiB upload limit.

---

## Blob Upload Failure

If the PUT fails:

* keep the WAV unchanged
* keep metadata as `pending`
* keep the same recording ID
* do not call `complete`
* do not delete anything
* allow a later sync attempt

Possible failures include:

* connection lost
* timeout
* expired upload URL
* Blob HTTP error
* SD read error

A later attempt must restart from `initiate`.

Resumable uploads are not required in this phase.

---

# Step 3 — Complete Upload

After the direct Blob PUT succeeds, call:

```
POST <BFF_BASE_URL>/api/device/recordings/complete
```

Headers:

```
Authorization: Bearer <DEVICE_TOKEN>
Content-Type: application/json
```

Body:

```json
{
  "recordingId": "rec_00112233445566778899aabbccddeeff"
}
```

Do not send the Blob URL unless the BFF contract explicitly requires it.

---

## Complete Success

Expected response:

```json
{
  "recordingId": "rec_00112233445566778899aabbccddeeff",
  "meetingId": "uuid",
  "status": "uploaded",
  "created": true
}
```

A retry after a lost response may return:

```json
{
  "recordingId": "rec_00112233445566778899aabbccddeeff",
  "meetingId": "uuid",
  "status": "uploaded",
  "created": false
}
```

Both are synchronization success.

Only now may the firmware:

```
persist meetingId
    ↓
persist syncedAt
    ↓
status = synced
```

---

# Safe Synchronization Rule

A local recording may be marked `synced` only when:

1. `initiate` reports the recording is already `uploaded`, or
2. `complete` returns a valid HTTP 200 success response confirming the recording.

A successful Blob PUT by itself is NOT enough.

The final BFF confirmation is the source of truth for synchronization completion.

---

## Idempotency

The protocol must be fully retry-safe.

Example:

```
initiate
  ↓
Blob PUT succeeds
  ↓
complete succeeds
  ↓
network drops before ESP32 receives response
  ↓
local recording remains pending
  ↓
user triggers sync again
  ↓
initiate with same recordingId
  ↓
BFF reports already uploaded
  ↓
device marks synced
```

The same recording must never create duplicate meetings.

The stable `recordingId` is the firmware-side idempotency key.

---

# Manual Sync Flow

When BOOT is long-pressed from `READY`:

```
SYNCING
  ↓
scan recording metadata
  ↓
find pending recordings
  ↓
validate Wi-Fi
  ↓
process pending recordings sequentially
  ↓
initiate
  ↓
already uploaded?
  ├── yes → mark synced
  └── no
        ↓
      Blob PUT
        ↓
      complete
        ↓
      mark synced
  ↓
next recording
  ↓
show summary
  ↓
READY
```

Use sequential synchronization for V1.

Do not perform concurrent recording uploads.

---

## Failure Handling

Failures must never cause valid local audio to be lost.

Handle at least:

* Wi-Fi unavailable
* BFF unavailable
* authentication failure
* initiate timeout
* malformed initiate response
* expired upload URL
* Blob PUT failure
* connection loss during upload
* SD read failure
* complete timeout
* malformed complete response
* non-2xx response

On failure:

```
status remains pending
```

Do not modify the WAV.

Do not generate a new recording ID.

Do not aggressively retry automatically.

The user may trigger sync again later.

---

## Connectivity-Level Failures

If Wi-Fi or the BFF is clearly unavailable, stop the current sync run instead of trying every pending recording.

Example:

```
5 pending
  ↓
first initiate fails because network is down
  ↓
stop
  ↓
show:
    NO CONNECTION
    5 pending
```

This avoids unnecessary network attempts and battery usage.

---

## UI

When manual sync begins:

```
SYNCING

3 pending
```

Avoid refreshing the e-paper for every uploaded chunk or request.

Useful refresh points:

```
sync started
    ↓
sync completed / stopped
```

Successful result:

```
SYNCED

3 uploaded
```

Partial result:

```
SYNC COMPLETE

2 uploaded
1 pending
```

Nothing pending:

```
UP TO DATE

No pending
recordings
```

No Wi-Fi:

```
NO WIFI

3 pending
```

Authentication failure:

```
SYNC ERROR

Authentication
failed
```

Detailed technical errors belong in serial logs.

---

## Logging

Use concise but useful logs.

Example:

```
ONDA_SYNC: Manual sync requested
ONDA_SYNC: Found 3 pending recordings

ONDA_SYNC: Initiating rec_0011...
ONDA_SYNC: Upload required

ONDA_SYNC: Uploading 81344044 bytes
ONDA_SYNC: Blob upload complete

ONDA_SYNC: Completing rec_0011...
ONDA_SYNC: Meeting ID: ...
ONDA_SYNC: Recording marked synced

ONDA_SYNC: Sync complete: 1 uploaded, 2 pending
```

Never log:

* device token
* Authorization header
* full signed Blob URL if it contains sensitive credentials
* sensitive request headers

---

## Security

* Bearer token is used only against the BFF.
* Never send the device token to Blob.
* Never place credentials in URLs controlled by the firmware.
* Use exactly the signed URL supplied by the BFF.
* Development may use HTTP for the local BFF.
* Production BFF communication must use HTTPS.
* Blob upload must use the signed HTTPS URL returned by the BFF.
* Do not trust remote responses without validating expected fields and status.
* Do not expose credentials on the e-paper display.

Signed upload URLs should be treated as temporary secrets.

Avoid logging them.

---

## Architecture

Keep responsibilities separated.

Conceptually:

```
App state / BOOT input
        ↓
    Sync service
        ↓
Recording repository
   ↙           ↘
metadata       WAV
        ↓
   Onda API client
    ↙         ↘
   BFF        Blob
```

The sync service should orchestrate existing modules.

It should not directly implement:

* SD filesystem details
* Bearer token storage
* low-level Wi-Fi management
* e-paper drawing
* recording logic

---

## Concurrency

Recording and synchronization are mutually exclusive in this phase.

During:

```
SYNCING
```

the device must not start recording.

During:

```
RECORDING
FINALIZING
```

the device must not start synchronization.

State-machine checks must enforce this.

---

## Reboot Recovery

The synchronization design must survive reboots.

If the device reboots:

* `pending` remains pending
* `synced` remains synced
* no persistent `uploading` state should block retry

If reboot occurs after Blob PUT but before completion:

```
metadata remains pending
  ↓
next sync restarts at initiate
  ↓
BFF resolves correct state idempotently
```

---

## Retention

Do not delete recordings during this phase.

Rules:

```
pending → always retain
synced  → retain
```

The SD card acts as a local backup after synchronization.

Storage cleanup and retention policy will be implemented separately.

---

## Non-goals

Do not implement:

* automatic background sync
* sync immediately after recording
* parallel uploads
* resumable Blob uploads
* chunk-level resume
* automatic retry loops
* exponential backoff
* WAV deletion
* SD cleanup
* storage quotas
* remote deletion
* OTA updates
* BLE provisioning

These belong to future phases.

---

## Manual Validation

### Offline Recording

1. Disable Wi-Fi.
2. Record multiple meetings.
3. Confirm all WAV files are safely stored.
4. Confirm all new metadata remains `pending`.

### Successful Sync

1. Restore Wi-Fi.
2. Start the local BFF.
3. Long-press BOOT from READY.
4. Confirm `initiate` succeeds.
5. Confirm WAV is uploaded directly to Blob.
6. Confirm `complete` succeeds.
7. Confirm local metadata becomes `synced`.
8. Confirm `meetingId` is persisted.
9. Confirm WAV remains on the SD card.

### Already Uploaded

1. Force a recording to remain locally pending after backend completion.
2. Trigger sync again.
3. Confirm `initiate` reports it already uploaded.
4. Confirm the WAV is not uploaded again.
5. Confirm local metadata becomes synced.

### Lost Complete Response

1. Allow Blob upload and backend completion.
2. Interrupt connectivity before firmware receives final confirmation.
3. Confirm local metadata remains pending.
4. Restore connectivity.
5. Trigger sync again.
6. Confirm idempotency resolves the existing meeting.
7. Confirm no duplicate meeting is created.

### Blob Failure

1. Interrupt Wi-Fi during Blob PUT.
2. Confirm the WAV remains untouched.
3. Confirm metadata remains pending.
4. Trigger sync later and confirm successful recovery.

### Reboot

1. Leave pending recordings.
2. Reboot the device.
3. Confirm pending recordings are rediscovered.
4. Trigger sync.
5. Confirm they synchronize normally.

### Button Safety

Verify:

* short press READY → recording
* short press RECORDING → stop/finalize
* long press READY → sync
* recording cannot start while syncing
* sync cannot start while recording/finalizing

---

## Completion Criteria

This phase is complete when:

1. BOOT long press starts synchronization from READY.
2. Pending recordings are discovered persistently.
3. `initiate` follows the BFF contract exactly.
4. `createdAt: null` is supported.
5. Firmware enforces 2-hour / 256 MiB limits.
6. WAV files are streamed directly from SD to Blob.
7. Complete WAV files are never loaded fully into RAM or PSRAM.
8. Device credentials are never sent to Blob.
9. `complete` follows the BFF contract exactly.
10. Successful completion marks recordings synced.
11. Already-uploaded recordings become synced without re-upload.
12. Failed uploads remain pending.
13. Retries reuse the same recording ID.
14. Reboots do not lose pending synchronization state.
15. Recording and synchronization remain mutually exclusive.
16. WAV files remain on SD after successful sync.
17. Sensitive credentials and signed URLs do not appear in logs.
18. `idf.py build` succeeds.
19. The complete offline → initiate → Blob → complete flow works on the physical device.

---

## Implementation Instructions

Before coding:

1. Read `AGENTS.md`.
2. Inspect the existing recording state machine.
3. Inspect BOOT short/long press handling.
4. Inspect persistent recording metadata.
5. Inspect SD/storage abstractions.
6. Inspect the existing authenticated Onda API client.
7. Read the BFF device recording ingestion contract.
8. Confirm endpoint paths and response schemas match exactly.
9. Review ESP-IDF HTTP client streaming APIs.
10. Propose a short implementation plan before coding.

Prioritize:

```
data integrity
    ↓
idempotency
    ↓
bounded memory
    ↓
reliable recovery
    ↓
synchronization speed
```

Do not implement recording deletion or automatic background synchronization in this phase.
