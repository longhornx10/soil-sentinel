#!/usr/bin/env bash
set -euo pipefail
CC=${CC:-$(command -v cc || command -v gcc || command -v clang || true)}
if [ -z "$CC" ]; then
  echo "error: no C compiler found (looked for cc, gcc, clang)" >&2
  exit 1
fi
TMP="$(mktemp /tmp/soil-sentinel-tests.XXXXXX)"
trap 'rm -f "$TMP"' EXIT
$CC -std=c11 -Wall -Wextra -Werror -pedantic -UNDEBUG \
  -Icore/include \
  core/src/soil_model.c core/src/soil_service.c tests/test_soil_model.c -lm \
  -o "$TMP"
"$TMP"
