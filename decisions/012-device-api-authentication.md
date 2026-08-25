# Decision 012 — Device API Authentication

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-25

## Context and problem

Onda needs to verify that a Wi-Fi-connected device can authenticate with the
Onda BFF before later upload and synchronization phases. Backend reachability
and credential validity must remain independent from Wi-Fi connectivity,
recording, and the e-Paper UI.

## Accepted implementation

- `components/onda_api` owns an asynchronous worker that makes one authenticated
  `GET /api/device/me` request after the application observes its first
  `WIFI_STATE_CONNECTED` event of a boot. A per-boot latch prevents retries on
  later Wi-Fi reconnects.
- The component exposes `NOT_CHECKED`, `CHECKING`, `AUTHENTICATED`,
  `UNAUTHORIZED`, and `ERROR` independently of Wi-Fi state. The application
  does not display these API states or schedule e-Paper work for them.
- The API worker uses ESP-IDF's HTTP client with a five-second timeout,
  redirect suppression, disabled HTTP authorization retries, and a 512-byte
  response cap. It sends the device credential only through the `Authorization`
  Bearer header and never logs headers, bodies, URLs with credentials, or the
  token.
- Successful responses must contain non-empty string `deviceId`, `name`, and
  `serverTime` fields. Invalid, incomplete, malformed, or oversized `200`
  responses resolve to `ERROR`; `401` and `403` resolve to `UNAUTHORIZED`; and
  transport failures or unexpected statuses resolve to `ERROR`.
- Development configuration is isolated in the ignored
  `components/onda_api/private_include/onda_api_config_local.h` file. The
  tracked `.example` contains placeholders only. Persistent credential
  provisioning is intentionally deferred.

## Validation evidence

- `get_idf && idf.py build` completed successfully with ESP-IDF 5.5.5. The
  final application image is `0x161330` bytes and leaves `0x9ecd0` bytes (31%)
  free in the 2 MB application partition.
- A focused ESP-IDF Unity Test App build compiled the `onda_api` parsing and
  state-mapping suite, including valid identity, malformed/incomplete/oversized
  responses, HTTP authentication statuses, and transport failure mappings.
- Firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem11101`. With the valid local credential, it acquired Wi-Fi,
  called the BFF once, received HTTP `200`, and logged the parsed identity.
- With a temporary invalid local credential, the physical device received HTTP
  `401` and reported `Unauthorized`; ESP-IDF's internal HTTP client also emits
  its own error-level retry-limit message, but Onda correctly maps the status.
- With a temporary unused local BFF port, the device reported
  `ESP_ERR_HTTP_CONNECT` and `Device API check failed` without crashing or
  interrupting other startup behavior. The original valid configuration was
  restored and reflashed, then HTTP `200` was confirmed again.
- Serial output was inspected across all scenarios and contained no credential.
  A repository audit also confirmed no credential-like value in tracked or
  unignored files.

## Consequences and constraints

- API authentication is a one-shot boot diagnostic for this phase; it neither
  gates recording nor retries after reconnection.
- There is no permanent backend status UI, upload protocol, recording sync,
  token rotation, or NVS credential storage.
- HTTP remains limited to local development configuration. Production API
  configuration must use HTTPS.

## Follow-up work

- Add explicit device-token provisioning and secure persistent storage in its
  dedicated feature.
- Add authenticated recording upload and synchronization only in the later
  upload/sync phase.
