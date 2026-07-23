# First-board field validation

Do not flash all four sensors until one unit completes this checklist.

## 1. Build gate

```bash
source ~/esp/esp-idf/export.sh
./scripts/test-host.sh
./scripts/build-release.sh
idf.py size
idf.py size-components
```

Record:

- ESP-IDF version and managed Zigbee component version
- application binary size
- free space in both OTA slots
- warnings or deprecations

Any app image at or above the 0x190000 slot size is a hard stop.

## 2. One-time migration

Remove the AA battery and inspect/insulate the antenna cable around both battery terminals.

```bash
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

This migration erases the old Zigbee pairing because the dual-slot partition table relocates Zigbee storage. Pair the test unit again in ZHA.

## 3. Basic behavior

Verify:

- existing pin map and external antenna selection
- short press takes a new sample
- dry/mid/moist displays red/yellow/green
- invalid electrical sample flashes red three times
- no LEDs remain on after the action
- timer wake and GPIO2 button wake both return to deep sleep
- native moisture, battery percentage, and battery voltage arrive in ZHA
- all schema-v2 diagnostic values decode correctly

## 4. Queued Home Assistant configuration

While the device sleeps:

1. Change Manual dry/wet bounds and thresholds.
2. Change calibration mode.
3. Confirm Home Assistant shows the desired values as pending.
4. Short-press the sensor.
5. Confirm the full configuration applies as one revision.
6. Submit an invalid dry/wet pair and verify the old curve remains active with a rejection reason.
7. Restart Home Assistant with a valid pending configuration and verify it retries on the next telemetry report.

Test every action: current-as-dry, current-as-wet, copy learned, reset learned, restore stock, plant moved, and identify.

## 5. Learning simulation on hardware

Capture raw voltage and noise in:

- air
- dry potting mix
- normal target moisture
- freshly watered/settled mix
- saturated mix
- water, only if electrically safe for the probe assembly

Run at least two realistic dry/water cycles. Confirm the learned curve remains inactive until confidence gates pass, does not map a narrow range to 0-100%, and can be disabled instantly by selecting Stock.

## 6. OTA success test

1. Build a second image with a strictly higher Zigbee file version.
2. Install its `.ota` and `index.json` in Home Assistant.
3. Hold the sensor button for three seconds; confirm red-yellow-green.
4. Release and start Install from the Update entity.
5. Verify progress, reboot into the other slot, first-boot health report, validation, and immediate success diagnostics.
6. Confirm pairing, settings, manual bounds, and learned state survive.

## 7. Intentional rollback test

Use a dedicated test build that deliberately withholds first-boot validation. Confirm the bootloader returns to the prior slot and reports Rolled back. Never deploy that test image to the other three units. Humans have already invented enough accidental rollback tests.

## 8. Low-battery OTA refusal

Using a current-limited bench supply or characterized alkaline cell, verify:

- pre-radio refusal below threshold
- active-radio voltage check catches load sag
- three red flashes and no partition write
- ordinary measurement still works afterward

Tune `SOIL_OTA_MIN_BATTERY_MV` from measured behavior.

## 9. Power budget

Measure integrated charge for:

- deep sleep
- normal measurement without report
- successful Zigbee report
- failed report/retry/backoff
- join/rejoin
- button sample
- watering capture
- 15-minute OTA waiting window
- full OTA transfer

Confirm no Wi-Fi tasks or activity exist and no GPIO leaks defeat deep sleep. Record the result in issues #2, #3, and #5 before declaring the remaining sensors field-ready.
