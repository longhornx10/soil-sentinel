#!/usr/bin/env bash
set -euo pipefail
./scripts/test-host.sh
./scripts/build.sh
python3 ./scripts/build-zigbee-ota.py \
  build/soil_sentinel.bin \
  --output-dir dist
if command -v sha256sum >/dev/null 2>&1; then
  SHA256="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
  SHA256="shasum -a 256"
else
  echo "error: no sha256sum or shasum found" >&2
  exit 1
fi
$SHA256 build/soil_sentinel.bin dist/soil-sentinel-*.ota > dist/SHA256SUMS
