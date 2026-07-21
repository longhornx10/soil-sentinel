# Architecture

Soil Sentinel is a wake-measure-decide-report-sleep firmware for the Seeed Studio XIAO ESP32-C6 Soil Moisture Sensor.

## Energy rules

1. Probe excitation exists only around an ADC burst.
2. A local sample does not imply a radio transmission.
3. Zigbee starts only when the state engine says a report has value.
4. LEDs are manual-interaction feedback, not routine decoration.
5. Routine samples remain in RAM/RTC state; NVS writes are event-driven.
6. Failed network contact causes backoff, not a battery-draining retry tantrum.

## Modules

- `core/soil_model`: platform-independent calibration, battery model, event detection, adaptive sampling, and survival behavior.
- `main/board`: XIAO carrier GPIO, 200 kHz probe excitation, trimmed ADC sampling, noise estimate, LEDs, and button.
- `main/storage`: calibration/policy and sparse checkpoints in NVS.
- `main/zigbee_transport`: sleepy end-device data model and on-demand reporting.
- `main/app_main`: one wake-cycle orchestrator.

## LP-core plan

The initial firmware uses full deep sleep because it establishes the lowest honest baseline. LP-core work is deliberately gated behind current measurements. It will be added only if a measured hybrid mode consumes less energy than a short main-core ADC wake. The first LP experiment will determine whether the ESP32-C6 LP core can directly control the probe PWM and ADC on this board revision.
