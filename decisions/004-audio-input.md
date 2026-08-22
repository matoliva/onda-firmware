# Decision 004 — Audio Input

- **Status:** Accepted and physically validated
- **Date:** 2026-08-22

Onda now exposes the Waveshare board microphone through a small `audio`
component. Audio remains an in-memory diagnostic: BOOT short press toggles
capture, and periodic peak levels are logged without creating or persisting a
recording.

## Context

Onda needs a reliable microphone boundary before it can add microSD-backed
recording. Waveshare's `07_Audio_Test` proves the ES8311 path but bundles
playback, user-interface, storage, and generic board configuration that Onda
does not need.

## Decision

| Area | Decision |
|------|----------|
| Public API | `audio_init()`, `audio_start()`, `audio_read(...)`, and `audio_stop()` return `esp_err_t` |
| Board control | Audio power is active-low GPIO42; ES8311 control uses I2C0 GPIO47/48 |
| I2S wiring | MCLK14, BCLK15, WS38, DOUT45, and DIN16 on I2S0 |
| Codec layer | Managed `espressif/esp_codec_dev` dependency pinned to 1.3.6; only ES8311 input support is used |
| Captured PCM | 16 kHz, 16-bit, stereo I2S PCM; physical capture showed microphone samples on one interleaved lane and zeroes on the unused lane |
| Buffering | The application uses a fixed 512-sample DMA-capable buffer and never accumulates a recording in RAM |
| Diagnostic UX | BOOT short press starts/stops temporary capture; PWR behaviour is unchanged; the e-Paper display is not refreshed for levels |
| Level metric | Peak magnitude is logged once per second while capturing |

## Implementation

- `components/audio` powers and initializes the ES8311, I2C, and standard I2S
  channels using only the board configuration confirmed by the vendor example.
- The component keeps codec/I2S details private, validates lifecycle calls, and
  returns initialization, start, read, and stop failures explicitly.
- The diagnostic task in `main/onda_main.c` receives BOOT notifications outside
  the button callback, reads bounded chunks, tolerates partial I2S reads, and
  emits `ONDA_AUDIO: Level …` logs.
- I2S channel lifecycle is tracked across repeated capture cycles so re-starts
  do not emit driver errors. Transient saturated samples at a capture boundary
  are excluded from the diagnostic peak metric.

## Validation evidence

The firmware was built with ESP-IDF 5.5.5, flashed to the physical
ESP32-S3-ePaper-1.54G through `/dev/cu.usbmodem11101`, and monitored.

Observed results:

- `idf.py build` completed successfully; the final image occupies `0x47280`
  bytes of the `0x100000` app partition.
- Flash writes and hashes completed successfully.
- Existing board checks, display initialization, button input, and `Onda ready`
  completed normally before audio initialization.
- The board reported `ONDA_AUDIO: Microphone ready (16 kHz, stereo, 16-bit PCM)`.
- Repeated physical BOOT short presses started and stopped capture cleanly.
- Spoken audio produced peak levels between approximately `6249` and `19104`.
- A quiet test produced materially lower steady levels, approximately `315` to
  `343` after the start transient.
- The final repeated start/stop test produced no I2S lifecycle errors; PWR was
  not assigned any audio behaviour.

## Consequences

- Future recording work can consume bounded PCM through `audio_read(...)`
  without importing codec or pin details.
- The current BOOT behavior is diagnostic-only and must be replaced deliberately
  when recording UX is designed.
- No local files, uploads, playback, compression, speech processing, or
  display-level visualization were introduced.

## Follow-up

Persistent microSD recording, final recording controls, and user-facing state
changes remain dedicated future features.
