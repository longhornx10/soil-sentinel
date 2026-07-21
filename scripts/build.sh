#!/usr/bin/env bash
set -euo pipefail
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
idf.py set-target esp32c6
idf.py build
