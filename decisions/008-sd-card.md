# Decision 008 — SD Card Baseline

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-24

## Context and problem

Onda needs a reliable local-storage boundary before it can persist meeting
audio. The onboard microSD card must remain independent of Wi-Fi, product UI,
and the existing in-memory recording diagnostic. The baseline must prove that a
user-formatted FAT32 card can be mounted, written, read, and safely released
without ever formatting it automatically.

## Accepted implementation

- `components/storage` owns all SDMMC, FatFs/VFS, card-status, and POSIX file
  operations. Its public lifecycle is `storage_init()`, `storage_verify()`, and
  `storage_deinit()`.
- The onboard card uses the Waveshare native one-bit SDMMC connection: CLK
  GPIO39, CMD GPIO41, and D0 GPIO40. It mounts at `/sdcard` using the SDMMC
  default 20 MHz frequency.
- Mounting disables `format_if_mount_failed`, permits one open file, and enables
  card-status checks. No card-detect GPIO is available, so mount and status
  errors report a missing or unusable card.
- Verification overwrites only Onda-owned `/sdcard/onda-test.txt` with the
  fixed text `Onda SD card verification\n`, flushes and closes it, then reopens
  it and requires an exact byte-for-byte match with no trailing data.
- `main` runs mount → verify → unmount after board bring-up and before audio
  initialization. Any storage failure is logged and does not prevent existing
  audio, Wi-Fi, button, or UI startup.
- `sdkconfig.defaults` enables stack-based FatFs long-filename support because
  `onda-test.txt` exceeds the 8.3 filename limit. This is a committed setting,
  not a local menuconfig dependency.

## Validation evidence

- A clean `get_idf && idf.py build` completed with ESP-IDF 5.5.5. The resulting
  application image is `0x139cc0` bytes and leaves `0xc6340` bytes (39%) free
  in the 2 MB app partition.
- Firmware was flashed to the Waveshare ESP32-S3-ePaper-1.54G at
  `/dev/cu.usbmodem1101`; image hashes were verified by `esptool.py`.
- On the physical device, the installed 29,820 MB SDHC card mounted at 20 MHz
  in one-bit mode. Serial logs confirmed the write, read, exact verification,
  and clean unmount of `/sdcard/onda-test.txt`; normal audio and Wi-Fi startup
  then continued.
- The user manually verified the missing-card path is nonfatal and reports an
  actionable error, and confirmed the diagnostic file and fixed contents after
  inspecting the card on a computer.

## Consequences and constraints

- This is a one-shot boot diagnostic, not persistent recording storage. It
  neither keeps the card mounted nor writes audio, metadata, quotas, cleanup,
  encryption, or sync state.
- Future recording storage must keep the card mounted for the recording
  lifecycle, close and verify recordings before upload, and must not delete
  local audio merely after beginning an upload.
- The diagnostic file is intentionally the only card path modified by this
  phase. Invalid or non-FAT32 cards are logged and left untouched.

## Follow-up work

- Implement durable audio-file creation and safe recording writes as its own
  planned feature.
