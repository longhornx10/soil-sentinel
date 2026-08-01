# Zigbee OTA workflow

Soil Sentinel uses the standard Zigbee OTA Upgrade client cluster. The firmware never starts Wi-Fi and never performs routine update checks. OTA is available only after a deliberate physical button hold.

## Build an OTA release

Update all three version locations before building:

1. `PROJECT_VER` in the root `CMakeLists.txt`
2. `SOIL_OTA_FILE_VERSION` in `main/firmware_update.h`
3. `--version-name` and `--file-version` in `scripts/build-release.sh`

Then run:

```bash
source ~/esp/esp-idf/export.sh
./scripts/build-release.sh
```

The script wraps the ESP-IDF application image in a Zigbee OTA container and creates a local zigpy index using a SHA3-256 checksum.

## Install the local provider in Home Assistant

Create:

```text
/config/zigpy_ota/soil-sentinel/
```

Copy these release files into it:

```text
soil-sentinel-<version>.ota
index.json
```

Add or merge this configuration:

```yaml
zha:
  custom_quirks_path: /config/custom_zha_quirks
  zigpy_config:
    ota:
      extra_providers:
        - type: zigpy_local
          index_file: /config/zigpy_ota/soil-sentinel/index.json
```

Restart Home Assistant after changing the provider or index. Confirm the device shows an Update entity with the new version before approaching the flowerpot with ceremonial intent.

## Update a sensor

1. Use a healthy alkaline AA cell.
2. Hold the sensor button for three seconds.
3. Wait for red, yellow, green, then release.
4. In Home Assistant, open the device’s Update entity and select **Install** within 90 seconds.
5. The sensor retries Query Next Image every 10 seconds until a transfer starts.
6. A brief yellow flash indicates waiting/progress.
7. Three green flashes indicate the image was accepted and the sensor is rebooting.
8. The new slot publishes a health report before it marks itself valid.
9. If first-boot validation fails, the ESP-IDF bootloader rolls back to the previous slot.

Press and release the sensor button during OTA to cancel immediately. The device also exits if no transfer begins within 90 seconds, if progress stalls for 90 seconds, if loaded battery voltage falls below the OTA threshold, or when the absolute 15-minute service window expires. The final OTA result is published before deep sleep whenever the Zigbee link remains available.

This replaces the original behavior that sent one query and then stayed awake for the entire 15-minute window even after Home Assistant explicitly returned **No image available**. That behavior was technically a service window and practically a small battery funeral.

## Battery refusal

The button wake checks battery voltage before starting Zigbee. Once Zigbee is running, the sensor checks voltage again under active radio load and then every five seconds during OTA. If any check is below the OTA-safe threshold, the device flashes red three times, aborts an active transfer, reports low battery, and returns to normal sleep.

`SOIL_OTA_MIN_BATTERY_MV` is currently a provisional 1250 mV. Do not treat that as laboratory truth until issue #2/#3 hardware measurements are complete.

## Rollback diagnostics

The device reports:

- current OTA state
- last OTA result
- progress percentage
- active application slot
- validation-pending state
- monotonic Zigbee file version

A successful download is not recorded as successful until the newly booted image initializes storage, measures the probe, restores Zigbee, publishes successfully, and calls the ESP-IDF rollback-cancel API.

## Trust boundary

The local zigpy index verifies the OTA container checksum, and `esp_ota_end()` validates the ESP-IDF application image before selecting it. Secure Boot and signed-release enforcement are not enabled in this bundle. Do not put untrusted files in the local OTA directory, a sentence that should not need writing but has met computers before.
