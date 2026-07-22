#!/usr/bin/env bash
set -euo pipefail
idf.py set-target esp32c6
idf.py build
