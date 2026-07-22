# Soil Sentinel

Native ESP-IDF firmware for the Seeed Studio XIAO ESP32-C6 Soil Moisture Sensor, focused on calibrated readings, Zigbee, local event intelligence, and long AA battery life.

## Current capabilities

- Native ESP-IDF 5.5.4 project for ESP32-C6
- ESP Zigbee SDK 2.x sleepy end-device transport
- No Wi-Fi initialization or Wi-Fi runtime
- External 2.4 GHz antenna selection matching the Seeed carrier
- Factory-style 200 kHz probe excitation with a one-second analog settling period
- Ten settled, filtered ADC readings per measurement
- Calibrated 0–100% soil moisture score
- Piecewise alkaline-AA battery estimate
- Standard Zigbee moisture, battery percentage, and battery voltage attributes
- Compact custom Zigbee telemetry for adaptive state and diagnostics
- Included ZHA quirk for Home Assistant diagnostic entities
- Watering-event detection and time-since-watering tracking
- Explicit current-sample validity and watering-history state
- Drying-rate estimate and measurement-confidence score
- Adaptive sampling modes with mode-transition reports
- Critical-low-battery survival mode
- Deep-sleep timer wake and physical-button wake
- USB bench mode that remains awake and resamples from the large carrier button
- Safe probe, LED, and RF-switch GPIO levels latched through deep sleep
- Sparse, versioned NVS checkpoints to limit flash wear
- Host tests for platform-independent logic

## Power policy

The probe excitation, LEDs, and Zigbee radio are not left on between measurements. Wi-Fi is never initialized. Routine battery wakes suppress informational application logging, and output GPIOs are held at known safe levels while the digital GPIO domain is powered down.

| State | Default sample interval |
|---|---:|
| Stable/moist | 4 hours |
| Drying | 1 hour |
| Near dry | 30 minutes |
| Critical dry | 15 minutes |
| Watering capture | 2 minutes |
| Recently watered | 10 minutes |
| Low-battery conservation | 12 hours |
| Critical-battery survival | 12 hours |

Reports are event-driven rather than sent after every sample. A heartbeat report is sent at least every 24 hours, while threshold changes, operating-mode changes, watering, faults, battery-state changes, and manual checks report immediately.

## Local LED behavior

A physical-button measurement shows:

- Red below 20% moisture
- Yellow from 20% through 59.9%
- Green at 60% or above
- Three quick red flashes for an electrically invalid reading

Routine timed samples do not illuminate the LEDs. Factory-new Zigbee steering uses a repeating red blink, pairing success uses green, and commissioning failure uses yellow.

## Home Assistant entities

Native ZHA discovery provides:

- Soil moisture
- Battery percentage
- Battery voltage

The included quirk additionally provides:

- Operating mode
- Sensor fault
- Current measurement valid
- Watering observed
- Measurement confidence
- Drying rate
- Sample interval
- Raw probe voltage
- Measurement noise
- Valid moisture history
- Time since watering
- Report reason flags
- Battery present

See [`docs/ZHA.md`](docs/ZHA.md) for installation and re-pairing instructions.

## Hardware safety before first power

1. Remove the AA battery before opening, flashing, or handling the board.
2. Inspect the external antenna cable where it passes the soldered battery terminals. No terminal or solder point may press into, pierce, or abrade the antenna insulation.
3. Re-route the antenna away from both battery contacts and add a durable insulating barrier before closing the case.
4. Flash, monitor, and perform the first Zigbee join over USB with the AA battery removed.
5. Install a fresh battery only after the case closes without pinching wires and the unit has passed the USB test.
6. If a cell, board, or case becomes hot, disconnect power and retire the affected hardware until the electrical fault is identified.

Firmware cannot protect against a direct mechanical short between a battery terminal and damaged antenna wiring.

## Build and host tests

```bash
git clone https://github.com/longhornx10/soil-sentinel.git
cd soil-sentinel

source ~/esp/esp-idf/export.sh
./scripts/test-host.sh
./scripts/build.sh
idf.py -p /dev/ttyACM0 flash monitor
```

Erase the factory firmware and Zigbee NVRAM only before the first join:

```bash
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

Do not erase an already paired sensor during routine firmware updates.

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

GPIO12 and GPIO13 remain reserved for native USB D- and D+. GPIO9 remains a boot strap and is not used by the application.

## Planned work

- [Issue #5](https://github.com/longhornx10/soil-sentinel/issues/5): verify the already-unused Wi-Fi stack with build-map and current measurements.
- [Issue #6](https://github.com/longhornx10/soil-sentinel/issues/6): add guarded, soil-specific adaptive self-calibration after multiple credible wet/dry cycles.

## Validation status

The platform-independent state engine is host-tested. Every firmware change still requires an ESP-IDF build and one physical-board test because the Zigbee stack, carrier analog front end, GPIO leakage, and actual battery consumption cannot be validated honestly by desktop unit tests alone. Embedded hardware remains committed to making confidence expensive.
