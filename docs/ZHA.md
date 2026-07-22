# Home Assistant ZHA integration

Soil Sentinel deliberately uses a mixed Zigbee data model:

- Standard Zigbee clusters for values ZHA already understands.
- One compact custom telemetry attribute for device-specific diagnostics and adaptive-state information.

This keeps radio airtime low while still exposing useful state in Home Assistant.

## Native entities

These come from standard clusters and do not require a custom quirk:

- Soil moisture
- Battery percentage
- Battery voltage

The battery entities are reported only while an AA cell is present. USB bench mode intentionally reports the battery as absent rather than manufacturing a dramatic 0% battery emergency.

## Quirk-provided entities

`integrations/zha_quirks/soil_sentinel.py` translates the compact telemetry frame into:

- Operating mode
- Sensor fault
- Measurement confidence
- Drying rate
- Sample interval
- Raw probe voltage
- Measurement noise
- Time since watering, initially disabled
- Report reason flags, initially disabled
- Battery present, initially disabled

All of these values travel in one telemetry report rather than one Zigbee packet per entity.

## Install the local quirk

1. Create a directory in the Home Assistant configuration area, for example:

   ```text
   /config/custom_zha_quirks
   ```

2. Copy `integrations/zha_quirks/soil_sentinel.py` into that directory.

3. Add the custom quirk path to `configuration.yaml`:

   ```yaml
   zha:
     custom_quirks_path: /config/custom_zha_quirks
   ```

4. Restart Home Assistant completely.

5. After flashing firmware that adds or changes Zigbee clusters, remove and re-pair the sensor so ZHA performs a new device interview. Merely pressing the sensor button does not make ZHA rediscover its endpoint descriptor.

6. Open the device page and confirm that the applied quirk is listed as `SoilSentinelTelemetryCluster` or otherwise shows the local Soil Sentinel quirk.

## Expected report confirmations

A successful state report should include confirmations similar to:

```text
moisture report confirmed: ... cluster=0x000c
telemetry report confirmed: ... cluster=0xfc00
battery percentage report confirmed: ... cluster=0x0001
battery voltage report confirmed: ... cluster=0x0001
```

USB bench mode omits the two battery reports because no AA is installed.

## Telemetry schema v1

The custom cluster is `0xFC00`, attribute `0x0000`, encoded as a 21-byte ZCL octet string payload:

| Offset | Size | Value |
|---:|---:|---|
| 0 | 1 | Schema version |
| 1 | 1 | Operating mode |
| 2 | 2 | Report event flags, little-endian |
| 4 | 1 | Sensor fault |
| 5 | 1 | Confidence percent |
| 6 | 2 | Raw probe millivolts |
| 8 | 2 | Measurement noise millivolts |
| 10 | 2 | Drying rate in hundredths of percent per hour |
| 12 | 4 | Adaptive sample interval in seconds |
| 16 | 4 | Seconds since detected watering |
| 20 | 1 | Battery present |

The first byte visible to the C implementation is the ZCL octet-string length prefix and is not part of the 21-byte payload consumed by the Python quirk.
