# Development

Development guide and command reference for the Onda firmware.

## Requirements

- macOS
- ESP-IDF 5.5.x
- Waveshare ESP32-S3-ePaper-1.54G
- ESP32-S3 target
- USB data cable

## Activate ESP-IDF

ESP-IDF must be activated in every new terminal session.

```bash
get_idf
```

`get_idf` is a local shell alias for:

```bash
source ~/.espressif/v5.5.5/esp-idf/export.sh
```

Verify the environment:

```bash
idf.py --version
```

Expected:

```text
ESP-IDF v5.5.5
```

---

## Daily Development Flow

From the project root:

```bash
cd ~/projects/onda-firmware
get_idf
```

Build the firmware:

```bash
idf.py build
```

Put the device into BOOT mode if required, find its serial port, then flash and monitor:

```bash
ls /dev/cu.*
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Typical development loop:

```text
Edit code
   ↓
Build
   ↓
BOOT mode
   ↓
Flash
   ↓
Monitor logs
   ↓
Repeat
```

---

## Commands

### Activate ESP-IDF

```bash
get_idf
```

Loads the ESP-IDF environment, compiler, Python environment, and tools into the current terminal session.

Run this once whenever a new terminal is opened.

---

### Check ESP-IDF Version

```bash
idf.py --version
```

Confirms that ESP-IDF is available and shows the active version.

---

### Configure ESP32-S3 Target

```bash
idf.py set-target esp32s3
```

Configures the project for the ESP32-S3.

This normally only needs to be done when initially configuring the project or changing targets.

Note: `set-target` resets the build directory and regenerates `sdkconfig`.

---

### Build

```bash
idf.py build
```

Compiles the firmware and generates the binaries that will be written to the ESP32.

Builds are incremental, so subsequent builds are normally faster.

---

### Find the Device Serial Port

```bash
ls /dev/cu.*
```

The ESP32 usually appears as:

```text
/dev/cu.usbmodemXXXX
```

Example:

```text
/dev/cu.usbmodem1101
```

The exact number may change after reconnecting the device.

---

### Flash

```bash
idf.py -p /dev/cu.usbmodemXXXX flash
```

Writes the compiled firmware to the ESP32 flash memory.

Example:

```bash
idf.py -p /dev/cu.usbmodem1101 flash
```

---

### Monitor

```bash
idf.py -p /dev/cu.usbmodemXXXX monitor
```

Opens the serial console and displays firmware logs in real time.

Example output:

```text
I (769) ONDA: Starting Onda
I (800) ONDA: Initializing display
I (900) ONDA: Ready
```

Exit the monitor with:

```text
Ctrl + ]
```

---

### Flash + Monitor

```bash
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Builds if necessary, flashes the firmware, and immediately opens the serial monitor.

This will normally be the main command used during development.

---

## BOOT Mode

The Waveshare ESP32-S3-ePaper-1.54G may need to be manually placed into download mode before flashing.

Procedure:

1. Disconnect USB.
2. Hold the BOOT button (gear icon).
3. Connect USB while continuing to hold BOOT.
4. Wait approximately 2 seconds.
5. Release BOOT.
6. Verify the serial port:

```bash
ls /dev/cu.*
```

The `/dev/cu.usbmodemXXXX` device should remain available.

Then flash normally:

```bash
idf.py -p /dev/cu.usbmodemXXXX flash
```

---

## Project Configuration

The reproducible project baseline is committed in:

```text
sdkconfig.defaults
```

ESP-IDF generates `sdkconfig` for the local build. Both `sdkconfig` and
`sdkconfig.old` are ignored because local menu configuration must not become an
undocumented project dependency. Any setting required by Onda must be added to
`sdkconfig.defaults` and verified with a clean build.

The `build/` and `managed_components/` directories are generated locally and are
also ignored. Dependency manifests and lockfiles should be committed when managed
components are introduced.

To regenerate the local configuration from the committed defaults:

```bash
idf.py fullclean
mv sdkconfig /tmp/onda-sdkconfig.backup  # optional: preserve local settings
idf.py build
```

Review the resulting configuration before discarding any backup.

### Interactive configuration

```bash
idf.py menuconfig
```

Opens ESP-IDF's interactive project configuration.

This is used for settings such as:

* flash configuration
* component configuration
* logging
* Wi-Fi
* FreeRTOS
* power management
* compiler options

Do not change configuration without understanding whether it should live in `sdkconfig` or `sdkconfig.defaults`.

---

## Clean Build

```bash
idf.py clean
```

Removes generated build outputs while keeping the existing CMake configuration.

Use when a normal incremental build behaves unexpectedly.

---

## Full Clean

```bash
idf.py fullclean
```

Deletes the complete build directory and forces ESP-IDF to configure and compile everything again.

Use when:

* build configuration becomes inconsistent
* switching important SDK configuration
* debugging unusual CMake/build problems

Do not use this routinely.

---

## Useful Combined Command

During normal development:

```bash
get_idf
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Or, once the serial connection is reliable:

```bash
idf.py -p /dev/cu.usbmodemXXXX build flash monitor
```

---

## Hardware

Current development hardware:

| Component       | Value                           |
| --------------- | ------------------------------- |
| Board           | Waveshare ESP32-S3-ePaper-1.54G |
| MCU             | ESP32-S3-PICO-1                 |
| Flash           | 8 MB                            |
| PSRAM           | 8 MB                            |
| Display         | 1.54" 200×200 e-Paper           |
| Display colours | Black / White / Red / Yellow    |
| Audio codec     | ES8311                          |
| Storage         | microSD                         |
| Connectivity    | Wi-Fi + BLE                     |
| ESP-IDF target  | `esp32s3`                       |

---

## Important

Do not develop Onda directly inside the Waveshare example repository.

The Waveshare repository should be treated as a hardware reference for:

* pin mappings
* display driver
* ES8311 audio configuration
* microSD configuration
* power management
* peripheral initialization

Onda-specific firmware belongs in this repository.
