# Wi-Fi provisioning security

Phase 006 uses ESP-IDF Wi-Fi Provisioning Manager over BLE with Security 1.
The firmware creates one random eight-character proof of possession (PoP) per
device and displays it only on the e-Paper setup screen. It is never emitted in
serial logs. The PoP stays stable across Wi-Fi reconfiguration.

The normal `sdkconfig.defaults` is intentionally a development configuration:
it includes the required NVS key partition but does **not** enable flash
encryption. This preserves the ordinary BOOT/download workflow.

## Controlled secure-device procedure

Only use a dedicated, recoverable validation or factory device after reviewing
Espressif's flash-encryption procedure. The secure build uses:

```sh
get_idf
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure.defaults" idf.py build
```

`sdkconfig.secure.defaults` enables release flash encryption and encrypted NVS.
Its first secure boot changes eFuses and must not be performed during routine
development. The `nvs_keys` partition is encrypted and supports ESP-IDF's
flash-encryption-backed NVS key protection; Wi-Fi credentials and the PoP then
remain encrypted at rest.

Do not put a Wi-Fi password, PoP, image-encryption key, or any production secret
in source control or terminal logs.

## Everyday Wi-Fi setup

Use Espressif's official **ESP BLE Provisioning** app to configure a new
Onda device:

1. Start the device and wait for the **Wi-Fi setup** screen. E-paper refreshes
   are slow, so allow up to 20 seconds for this screen to settle.
2. In the app settings, choose **BLE** as the device type and keep
   **Encrypted Communication** enabled. This maps to the firmware's Security 1
   configuration. Do not set a username: that option is for Security 2.
3. Select the advertised `ONDA-XXXXXX` device and enter the exact eight-character
   PoP shown on the display. The service name is not the PoP.
4. Select a 2.4 GHz Wi-Fi network and submit its credentials. A successful
   connection finishes at **Ready / Wi-Fi connected**.

The device rejects an incorrect PoP without accepting Wi-Fi credentials and
continues BLE advertising so setup can be retried. Do not share Wi-Fi passwords
or the PoP in source control, issue text, or logs.

## Reconfiguration and recovery

Hold **PWR** for three seconds while the application is in READY to clear stored
Wi-Fi credentials and start BLE provisioning again. This action is ignored while
recording or in the application ERROR state. Reconfiguration keeps the device
PoP unchanged.

After provisioning, Onda reconnects automatically at boot. A temporarily
unavailable network follows bounded short retries, then remains offline and
retries periodically; it never clears credentials automatically. Recording
remains available throughout these network states.
