# Decision 011 — NTP Time Sync

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-24

## Context and problem

Onda's offline recording flow used collision-safe unknown-date paths until a
plausible system clock happened to exist. The device needs correct local dates
and times for recording organisation after Wi-Fi is available, without making
recording depend on a network connection or causing extra e-Paper refreshes.

## Accepted implementation

- `components/onda_time` owns ESP-IDF's managed SNTP client and the explicit
  `UNSYNCED`, `SYNCING`, `SYNCED`, and `FAILED` time states. It configures one
  `pool.ntp.org` server, uses an immediate update, and trusts a clock only after
  this boot receives a plausible SNTP callback.
- The time service configures the POSIX timezone rule
  `NZST-12NZDT,M9.5.0,M4.1.0/3`, so `localtime_r` applies Pacific/Auckland
  daylight saving automatically. `recording_naming` now uses that local time
  for timestamped directories and filenames.
- The application initializes the time service at boot and starts or refreshes
  synchronization only after it receives the existing high-level
  `WIFI_STATE_CONNECTED` notification. The component does not access Wi-Fi
  event IDs or alter Wi-Fi connection/provisioning behavior.
- Initial synchronization has a static 30-second timeout. A timeout stops
  SNTP, records `FAILED`, and waits for a later Wi-Fi-connected event to retry.
  After the first successful sync, the trusted state survives Wi-Fi loss and
  SNTP retains its one-hour resynchronization interval.
- Recording path selection supplies a real clock only while `onda_time` reports
  `SYNCED`; otherwise it preserves the collision-safe
  `/recordings/unknown-date/recording-NNNN.wav` fallback.
- NTP transitions are logging-only. They do not enqueue application or display
  work, add UI state, or refresh the e-Paper display. Existing Wi-Fi status UI
  behavior is unchanged.

## Validation evidence

- `get_idf && idf.py build` completed successfully with ESP-IDF 5.5.5. The
  application image is `0x142ef0` bytes and leaves `0xbd110` bytes (37%) free
  in the 2 MB application partition.
- A focused ESP-IDF Unity test firmware build compiled the `recording_naming`
  test suite, including Auckland local-time, daylight-saving transition, and
  unknown-date fallback cases.
- Firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem1101`. Serial monitoring confirmed Wi-Fi connection, SNTP
  synchronization, `UTC 2026-08-24T07:44:25Z`, and
  `Local 2026-08-24 19:44:25 NZST`.
- The user confirmed successful physical validation of the synchronized local
  naming flow, offline fallback, reconnect behavior, and absence of
  time-driven e-Paper refreshes.

## Consequences and constraints

- NTP improves local recording organisation only; no recording is blocked,
  stopped, or deleted because time synchronization or Wi-Fi fails.
- `pool.ntp.org` time is not an authorization, identity, or security signal.
- There is no time/date UI, timezone selection UI, manual clock setting, or
  backend/API work in this feature.

## Follow-up work

- Add recording metadata, upload/sync, authentication, and deletion policy only
  in their dedicated features.
- Add user-configurable timezones only if a future product feature requires it.
