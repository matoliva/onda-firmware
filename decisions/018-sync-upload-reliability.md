# Decision 018 — Sync Upload Reliability

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-28

## Context and problem

A roughly 40-minute recording remained pending after the device reported that
sync had finished. Live reproduction showed the 149,121,164-byte signed Blob
upload timing out while waiting for the upload response because it shared the
five-second timeout used by small BFF requests. The device also displayed
`SYNC COMPLETE` for the partial-sync state, obscuring the failure.

## Accepted implementation

- Small authenticated BFF requests (`me`, `initiate`, and `complete`) retain
  the five-second request timeout.
- Signed Vercel Blob PUT uploads use a dedicated 60-second HTTP timeout so a
  large upload can wait for its response without using the small-request
  budget.
- A failed upload or `complete` call leaves the local recording pending; it is
  only marked synced after the validated `complete` response.
- The partial result screen is titled `SYNC PARTIAL`, while a fully successful
  result remains `SYNCED`.
- No retries, multipart uploads, TLS changes, credentials, URLs, audio-format
  changes, or local-recording deletion policy were added.

## Validation evidence

- `idf.py build` completed successfully with ESP-IDF 5.5.5.
- The `device_ui` and `onda_api` ESP-IDF Unity test applications built
  successfully with the updated sources and tests.
- The normal firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem1101`; esptool verified the image hashes and startup logs
  showed the SD card mounted, Wi-Fi connected, and device authentication
  succeeded.
- The physical retry uploaded the 149,121,164-byte WAV successfully:
  `Signed Blob upload returned HTTP 200`, followed by
  `Recording complete returned HTTP 200`, and the device rendered `SYNCED`.
- Production Vercel logs for deployment `dpl_FqF9e2SWvXNa8E8MCnTgGRMrzuph`
  recorded HTTP 200 for both
  `/api/device/recordings/initiate` and
  `/api/device/recordings/complete`.

## Consequences and constraints

- A large sync can remain on the `SYNCING` screen for several minutes while
  the WAV transfers over Wi-Fi; the timeout now protects the response wait but
  does not provide progress reporting.
- The device still needs another sync attempt after a failed upload; automatic
  retry and resumable/multipart transfer remain out of scope.
- The Vercel Blob PUT is external to the BFF request logs, so its success is
  confirmed by device serial output rather than a Vercel function log.

## Follow-up work

- Consider progress reporting or resumable uploads only if large recordings
  continue to create unacceptable wait times.
