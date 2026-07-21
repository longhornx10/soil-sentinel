# Soil Sentinel

Native ESP-IDF firmware for the Seeed Studio XIAO ESP32-C6 Soil Moisture Sensor, focused on calibrated readings, Zigbee, local event intelligence, and multi-year AA battery life.

## Current foundation

- Native ESP-IDF 5.5.4 project, ESP32-C6 target
- ESP Zigbee SDK 2.x sleepy end-device transport
- Calibrated 0–100% soil moisture score
- Trimmed ADC burst sampling and noise estimation
- Piecewise alkaline-AA battery estimate
- Watering-event detection
- Adaptive sampling modes
- Critical-low-battery survival mode
- Zigbee Analog Input moisture entity and Power Configuration battery entity
- Deep-sleep wake cycle with button wake
- Sparse NVS checkpoints to limit flash wear
- Host tests for platform-independent logic
- External 2.4 GHz antenna selection matching the Seeed soil-sensor assembly

## Hardware safety before first power

1. Remove the AA battery before opening, flashing, or handling the board.
2. Inspect the external antenna cable where it passes the soldered battery terminals. No terminal or solder point may press into, pierce, or abrade the antenna insulation.
3. Re-route the antenna away from both battery contacts and add a durable insulating barrier before closing the case.
4. Flash, monitor, and perform the first Zigbee join over USB with the AA battery removed.
5. Install a fresh battery only after the case closes without pinching wires and the unit has passed the USB test.
6. If a cell, board, or case becomes hot, disconnect power and retire the affected hardware until the electrical fault is identified.

Firmware cannot protect against a direct mechanical short between a battery terminal and damaged antenna wiring.

## Build

```bash
git clone https://github.com/longhornx10/soil-sentinel.git
cd soil-sentinel

# ESP-IDF 5.5.4 environment
source ~/esp/esp-idf/export.sh
./scripts/test-host.sh
./scripts/build.sh
idf.py -p /dev/ttyACM0 flash monitor
```

Erase the factory firmware/NVRAM before the first Zigbee join:

```bash
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

## Pin map

| Function | GPIO |
|---|---:|
| Battery ADC | 0 |
| Soil ADC | 1 |
| Button / wake | 2 |
| RF switch control enable, active low | 3 |
| External antenna select, high | 14 |
| Probe excitation, 200 kHz | 21 |
| Yellow LED | 18 |
| Green LED | 19 |
| Red LED | 20 |

## Development order

1. Compile and flash one sacrificially brave sensor.
2. Characterize dry/wet ADC values and probe stabilization time.
3. Measure deep-sleep, local-sample, Zigbee-report, retry, and rejoin energy.
4. Correct any board-specific ADC scaling and Zigbee interview behavior.
5. Add button calibration and Zigbee diagnostics/custom attributes.
6. Prototype LP-core ADC/PWM supervision and retain it only if it wins the power measurements.
7. Add ZHA and Zigbee2MQTT integration definitions where standard discovery is insufficient.

## Status

This is an initial engineering build. The host-side state engine is tested. Hardware and Zigbee compilation/behavior still require validation against a physical sensor and the pinned Espressif SDK, because embedded development enjoys charging admission at the hardware boundary.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
