#!/usr/bin/env bash
set -euo pipefail
if ! command -v idf.py >/dev/null 2>&1; then
  echo "error: idf.py not found - source the ESP-IDF environment first (e.g. . \$HOME/esp/esp-idf/export.sh)" >&2
  exit 1
fi
idf.py set-target esp32c6
idf.py build
