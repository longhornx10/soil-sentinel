# Soil Sentinel

Native ESP-IDF firmware for the Seeed Studio XIAO ESP32-C6 Soil Moisture Sensor. It provides native Zigbee reporting, three moisture-calibration modes, queued Home Assistant controls, manually armed Zigbee OTA updates, and aggressive deep-sleep power management for one alkaline AA cell.

## Field-ready 1.0 bundle

- ESP-IDF 5.5.4 and ESP Zigbee SDK 2.x sleepy-end-device firmware
- No Wi-Fi initialization or Wi-Fi runtime
- Dual OTA application slots with ESP-IDF rollback
- Zigbee OTA is available only during a manually armed 15-minute service window
- OTA entry requires a three-second button hold and a battery check before and after Zigbee starts
- Stock, learning, and manual moisture calibration modes
- Controller-side queuing of Home Assistant settings until the sensor next wakes
- Atomic, revisioned configuration with last-known-good fallback
- Manual dry/wet voltage bounds, thresholds, curve reset/copy actions, and identify action
- Firmware, calibration, configuration, measurement, and OTA diagnostics in ZHA
- Sparse NVS checkpoints plus RTC state across normal deep sleep
- Deliberate 20-second physical factory reset gesture
- Alkaline AA battery profile only

## Button behavior

| Gesture | Behavior |
|---|---|
| Short press | Take a fresh measurement, show the moisture color, and report it |
| Hold for 3 seconds, then release | Arm Zigbee OTA mode for 15 minutes |
| Continue holding to 15 seconds | Flash a reset warning; releasing cancels reset and enters OTA mode |
| Continue holding to 20 seconds | Full factory reset, including Zigbee pairing |

OTA entry indications:

- Red, yellow, green: OTA service mode is accepted. Release the button.
- Three red flashes: the alkaline AA voltage is below the provisional OTA-safe threshold.
- Brief yellow blink: waiting for Home Assistant or download progress.
- Three green flashes: image received and selected for reboot.
- Three red flashes: OTA failed or the service window expired.

The OTA battery threshold is deliberately conservative but remains a hardware-validation item. Final voltage limits must be based on a real alkaline cell measured while Zigbee is active, because batteries enjoy changing the answer when current is actually drawn.

## Moisture calibration modes

### Stock

Uses the fixed vendor-style bounds. The current defaults are 2750 mV dry and 1200 mV wet until physical characterization replaces them.

### Learning

Starts on the stock curve and observes credible drying/watering cycles. It rejects invalid, noisy, implausibly narrow, and relocation-like data. A learned curve is blended in only after at least two credible cycles, adequate raw-voltage span, and minimum confidence. Switching modes does not erase learned history.

### Manual

Uses explicit dry and wet raw-voltage bounds supplied from Home Assistant. The full configuration is validated atomically, and an invalid request leaves the previous curve active.

Home Assistant also exposes actions to use the current reading as dry or wet, copy learned bounds to manual, reset learning, restore stock manual bounds, mark the plant as moved, and identify the physical sensor.

## Power policy

The probe excitation, LEDs, and Zigbee radio remain off between measurements. Wi-Fi is never initialized. GPIO output levels are held through deep sleep.

| State | Default interval |
|---|---:|
| Stable/moist | 4 hours |
| Drying | 1 hour |
| Near dry | 30 minutes |
| Critical dry | 15 minutes |
| Watering capture | 2 minutes |
| Recently watered | 10 minutes |
| Low-battery conservation | 12 hours |
| Critical-battery survival | 12 hours |

Reports are event-driven. Threshold crossings, watering, operating-mode changes, faults, material battery transitions, configuration changes, and manual checks report immediately. A heartbeat is sent at least every 24 hours.

## One-time migration warning

This release changes the flash map from one factory application partition to two OTA slots. The old Zigbee storage partition sits inside the new first OTA application slot, so the one-time migration **cannot preserve the existing pairing**. Each already-flashed sensor must be erased, flashed, and paired once more.

After that migration, ordinary OTA updates preserve Zigbee pairing, settings, manual calibration, and learned calibration.

Perform the migration with the AA cell removed:

```bash
source ~/esp/esp-idf/export.sh
./scripts/test-host.sh
./scripts/build.sh
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

Do not erase flash during later routine updates.

## Release build

```bash
source ~/esp/esp-idf/export.sh
./scripts/build-release.sh
```

Outputs:

- `build/soil_sentinel.bin`: raw ESP-IDF application image
- `dist/soil-sentinel-1.0.0.ota`: Zigbee OTA container
- `dist/index.json`: local zigpy OTA index
- `dist/SHA256SUMS`: release checksums

Bump `PROJECT_VER`, `SOIL_OTA_FILE_VERSION`, and the release-script arguments together for every OTA release. Zigbee firmware versions must increase monotonically, because apparently software needs paperwork too.

## Home Assistant

See:

- [`docs/ZHA.md`](docs/ZHA.md) for the custom quirk and queued controls
- [`docs/OTA.md`](docs/OTA.md) for local ZHA OTA hosting and update procedure
- [`docs/FIELD_TEST.md`](docs/FIELD_TEST.md) for the mandatory first-board validation

## Audited pin map

| Function | GPIO | Firmware behavior |
|---|---:|---|
| Battery ADC | 0 | ADC1 input |
| Soil ADC | 1 | ADC1 input |
| Button / deep-sleep wake | 2 | Active low with pull-up |
| RF switch control enable | 3 | Driven low and held during sleep |
| External antenna select | 14 | Driven high and held during sleep |
| Yellow LED | 18 | Active high; held low during sleep |
| Green LED | 19 | Active high; held low during sleep |
| Red LED | 20 | Active high; held low during sleep |
| Probe excitation | 21 | 200 kHz, 174/255 duty only while measuring; held low during sleep |

GPIO12 and GPIO13 remain reserved for native USB D- and D+. GPIO9 remains a boot strap and is not used.

## Safety

1. Remove the AA battery before opening, flashing, or handling the board.
2. Inspect the antenna cable where it passes the battery terminals.
3. Prevent any terminal or solder point from pressing into or abrading the antenna insulation.
4. Add a durable insulating barrier and close the case without pinching the cable.
5. Perform the first flash, serial test, and pairing over USB with the AA removed.
6. Retire any board, cell, or enclosure that becomes hot until the fault is understood.

Firmware cannot prevent a mechanical short. Lithium-ion/14500 operation is not supported by this release.

## Validation status

The host logic suite currently covers:

- three calibration modes and fallback
- atomic revisioned configuration
- invalid manual bounds and thresholds
- learning convergence, reset, and disable behavior
- current-reading dry/wet actions
- watering and mode transitions
- telemetry status flags
- alkaline low-battery behavior
- OTA/refusal/factory-reset button policy

An ESP-IDF build and physical-board validation are still mandatory before flashing all four units. Zigbee SDK APIs, partition sizes, ADC behavior, current draw, RF reliability, and rollback cannot be honestly certified by a desktop C compiler, despite the software industry’s recurring efforts to manifest hardware through confidence.
