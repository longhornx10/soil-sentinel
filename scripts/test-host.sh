#!/usr/bin/env bash
set -euo pipefail
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -Icore/include core/src/soil_model.c tests/test_soil_model.c -lm \
  -o /tmp/soil-sentinel-tests
/tmp/soil-sentinel-tests
