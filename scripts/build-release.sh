#!/usr/bin/env bash
set -euo pipefail
./scripts/test-host.sh
./scripts/build.sh
python3 ./scripts/build-zigbee-ota.py \
  build/soil_sentinel.bin \
  --output-dir dist \
  --version-name 1.0.1 \
  --file-version 0x00010001
sha256sum build/soil_sentinel.bin dist/soil-sentinel-1.0.1.ota > dist/SHA256SUMS
