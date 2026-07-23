# Home Assistant and ZHA setup

## Install the custom quirk

Copy:

```text
integrations/zha_quirks/soil_sentinel.py
```

to:

```text
/config/custom_zha_quirks/soil_sentinel.py
```

Add or merge this block in `configuration.yaml`:

```yaml
zha:
  custom_quirks_path: /config/custom_zha_quirks
```

Restart Home Assistant after replacing the quirk. The first migration to the dual-slot firmware requires re-pairing because its partition-table change relocates Zigbee storage. Later firmware and quirk updates should not require re-pairing.

## Native entities

ZHA provides standard entities for:

- soil moisture
- battery percentage
- battery voltage
- firmware update, when a newer image is present in the configured OTA provider

## User controls

The quirk adds:

- Calibration mode: Stock, Learning, Manual
- Manual dry voltage
- Manual wet voltage
- Dry threshold
- Critical threshold
- Use current reading as dry
- Use current reading as wet
- Copy learned bounds to manual
- Reset learned curve
- Restore manual bounds to stock
- Plant moved / restart learning
- Identify sensor

## Sleepy-device behavior

Configuration controls are intentionally not instant. When a direct write fails because the sensor is asleep, the quirk caches the complete desired configuration. The next telemetry report triggers another write while the sensor is awake.

The firmware applies a configuration only when it receives the final desired revision. It validates the complete candidate, persists it atomically, acknowledges the applied revision, and retains the old configuration if validation or storage fails.

A short physical button press forces a measurement/report window and is the fastest way to deliver queued settings.

One-shot actions are best pressed immediately after waking the device. Persistent settings survive Home Assistant restarts through ZHA’s attribute cache and revision comparison; one-shot actions are deliberately not replayed indefinitely, since repeatedly declaring that a plant moved would eventually become performance art.

## Calibration workflow

### Learning mode

1. Select **Learning**.
2. Let the sensor observe at least two credible dry-to-water cycles.
3. Watch Learning confidence, Learning cycles, Learned dry voltage, and Learned wet voltage.
4. If the learned curve behaves poorly, switch to **Stock** immediately. Learned history remains available but stops affecting moisture.
5. Use **Reset learned curve** or **Plant moved / restart learning** only when you actually want to discard it.

### Manual mode from logged voltage

1. Enable Raw probe voltage in the device diagnostics.
2. Record the raw value near the point you consider dry.
3. Water normally and record the settled wet value.
4. Enter those values in Manual dry voltage and Manual wet voltage.
5. Select **Manual**.

Dry voltage must be greater than wet voltage and the span must be at least 100 mV. Invalid values are rejected as one transaction; the last valid curve remains active.

The **Use current reading as dry/wet** buttons make this faster when the device is already at the desired reference condition.

## Diagnostic entities

The quirk exposes, mostly as diagnostic entities:

- operating mode and active calibration mode
- active curve source
- active and learned voltage bounds
- learning confidence and cycle count
- raw voltage, noise, measurement confidence, and drying rate
- sample interval and time since watering
- pending/applied configuration state and rejection result
- OTA state, result, progress, active slot, validation-pending state, and firmware file version
- report-reason flags and validity/history indicators

Enable only the diagnostics you actually use. Forty entities do not make a fern more scientific.
