# Decision 009 — Audio Recording

- **Status:** Accepted and physically validated
- **Completion date:** 2026-08-24

## Context and problem

Onda needs to persist microphone capture locally before it can upload or
transcribe meetings. The accepted audio boundary provides 16 kHz, 16-bit
stereo I2S capture, and the accepted storage boundary mounts a FAT32 microSD
card, but neither previously produced a durable, standard audio file.

The first persistent-recording baseline must remain offline-first, stream data
without accumulating a recording in RAM, and preserve a previous successful
test recording if a later attempt fails.

## Accepted implementation

- `components/audio_recorder` owns the microphone-to-file critical path. It
  uses a higher-priority static FreeRTOS task with fixed capture and mono
  buffers; `main` never calls `audio_read()` while a recording is active.
- BOOT short press starts a recording at `/sdcard/test.wav.part`. A second
  short press requests asynchronous stop and finalization. The application
  remains in `RECORDING` until the recorder callback confirms success, then
  transitions to `READY`; any recorder failure transitions to `ERROR`.
- Each 16-bit stereo I2S frame is downmixed with a 32-bit accumulator to
  16-bit mono PCM. The final file is a canonical 44-byte RIFF/WAVE header plus
  16 kHz, mono, 16-bit PCM data.
- The recorder writes a provisional header, streams bounded PCM chunks,
  flushes data, rewrites the header with final byte counts, flushes and closes
  the file, then publishes it as `/sdcard/test.wav`.
- `storage` retains ownership of FatFs/POSIX handles through opaque single-file
  APIs. It remains mounted after its existing boot diagnostic. A staged
  replacement uses a same-volume backup/rename sequence, restoring the prior
  completed test file on an ordinary replacement failure; failures remove only
  the incomplete `.part` file.
- No new recording UI, Wi-Fi dependency, upload, dynamic filename, metadata,
  compression, recovery, or transcription behavior was added. Existing UI
  rendering continues to show the established recording/ready states.

## Validation evidence

- `get_idf && idf.py build` completed successfully with ESP-IDF 5.5.5. The
  final application image is `0x13b0e0` bytes and leaves `0xc4f20` bytes (38%)
  free in the 2 MB application partition.
- The focused `audio_recorder` ESP-IDF Unity test firmware built successfully
  with the documented `EXTRA_COMPONENT_DIRS` command. It compiles coverage for
  WAV header byte layout, final data sizes, overflow-safe stereo downmixing,
  and incomplete/empty PCM handling.
- Firmware was flashed to `/dev/cu.usbmodem1101`; `esptool.py` verified all
  written image hashes. Captured serial boot logs showed a 29,820 MB SDHC card
  mounting at 20 MHz in one-bit mode, successful storage verification, ES8311
  initialization, buttons, Wi-Fi, and normal Ready UI startup.
- The user completed and confirmed the Phase 009 manual validation: BOOT
  start/stop recording, saved WAV inspection and playback, and the planned
  physical behavior checks passed successfully.

## Consequences and constraints

- `test.wav` is intentionally a deterministic diagnostic output; it is not a
  meeting filename or a sync queue entry.
- Recording depends on a mounted and usable microSD card, but Wi-Fi remains
  unnecessary for recording. Card, I2S, file-write, finalization, and
  replacement failures are logged explicitly and release active resources.
- The backup/rename sequence protects ordinary replacement errors, not sudden
  power loss. Power-loss recovery remains a dedicated future feature.

## Follow-up work

- Add meeting-specific recording UX, naming, metadata, durable recovery, and
  upload/sync only as separately planned features.
