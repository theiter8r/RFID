# Fast ESP32 Flashing Guide

## Current hardware configuration

- Relay module `IN`: GPIO27
- BC547 buzzer base through 1 kΩ: GPIO33
- Door contact: not installed
- `DOOR_SENSOR_INSTALLED = false`: five-second timed relock
- FC-51 `OUT`: GPIO21
- MFRC522 SPI: SS 17, SCK 18, MISO 19, MOSI 23, RST 16
- Green LED: GPIO25
- Red LED: GPIO26

The complete program is `firmware/src/main.cpp`. It was compile-verified for
PlatformIO board `esp32dev` on 2026-08-07.

## Before connecting USB

1. Disconnect both 12 V lock wires from the relay contacts.
2. The relay control pins may remain connected to 5 V, GND, and GPIO27.
3. Measure the LM2596 output before using it: it must be approximately 5.00 V.
4. For the first flash, power the ESP32 by USB only. Do not simultaneously connect
   an unverified LM2596 supply.
5. Confirm MFRC522 and FC-51 VCC are connected to ESP32 3V3, never 5 V.

## Create the local configuration

Copy `firmware/include/secrets.example.h` to
`firmware/include/secrets.h`, then edit `secrets.h`.

Required values:

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `DEVICE_API_URL`
- `SUPABASE_PUBLISHABLE_KEY`
- `DEVICE_ID`
- `DEVICE_SECRET`
- current root CA certificate for the Supabase hostname

For an isolated bench connectivity test only, `ALLOW_INSECURE_TLS` can be set to
`true`. Set it back to `false` and install the correct CA before deployment.

If Supabase is not configured yet, placeholder API/device values will still let
the firmware compile and let you test FC-51, LEDs, buzzer, relay timing, and serial
messages. RFID database authorization will correctly fail until the backend and
local cache exist.

## Flash with VS Code and PlatformIO

1. Install Visual Studio Code.
2. Install the **PlatformIO IDE** extension.
3. In VS Code choose **File → Open Folder** and open the `firmware` directory,
   not only `main.cpp`.
4. Connect ESP32 with a USB **data** cable.
5. Wait for PlatformIO to install the ESP32 platform and libraries.
6. Click the PlatformIO checkmark to build.
7. Click the PlatformIO right-arrow to upload.
8. If it remains at `Connecting...`, hold **BOOT**, start upload, release BOOT as
   soon as percentage/progress begins. Press EN once afterward if needed.
9. Open PlatformIO Serial Monitor at **115200 baud**.

Expected boot text includes:

```text
[door] lock output: LOCK
[cache] no local allow-list
[sensor] FC-51 proximity ready; adjust its potentiometer to 5-12 cm
RFID controller firmware 0.1.0 ready; door_closed=true
```

`door_closed=true` is intentional while the sensor is disabled.

## First bench test sequence

Keep the 12 V lock disconnected.

1. Reset ESP32. Relay must settle into the intended locked/default state.
2. If the relay is energized in the wrong state, change
   `LOCK_OUTPUT_ACTIVE_HIGH` in `main.cpp` from `true` to `false`, rebuild and
   flash again.
3. Wave a hand 5–12 cm from FC-51. Green LED and buzzer should activate and the
   relay should enter unlock state.
4. After five seconds the relay should return to locked/default state.
5. Present an RFID card. Its UID should appear in Serial Monitor. Database access
   works only after valid Supabase configuration or a previously synced cache.
6. Confirm red LED/buzzer denial for an unknown card.
7. Only after relay polarity is correct should the separately fused lock branch
   be connected.

## Terminal alternative

From a shell with PlatformIO available:

```text
cd firmware
pio run
pio run --target upload --upload-port COM5
pio device monitor --port COM5 --baud 115200
```

Replace `COM5` with the port shown in Windows Device Manager or PlatformIO.

