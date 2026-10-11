#!/usr/bin/env bash
# One command to build and test MiniOEMS.
# Builds the pinned QuickFIX first if it's missing or out of date, then the project,
# then runs the tests. Build output location: see scripts/common.sh.
#
# Usage:  scripts/build.sh [Release|Debug]      (default: Release)
# Env:    JOBS=N                 parallel build jobs (default 4)
#         MINI_OEMS_BUILD_ROOT   where build output goes
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
CONFIG="${1:-Release}"
BUILD_DIR="$MINI_OEMS_BUILD_ROOT/$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
echo "Build output: $MINI_OEMS_BUILD_ROOT"

# Rebuild QuickFIX only when the pinned commit or the patch has changed.
STAMP="$QUICKFIX_PREFIX/.stamp"
WANT="$(git hash-object "$ROOT/scripts/build_quickfix.sh" "$ROOT/third_party/quickfix/mingw.patch" | tr '\n' ' ')"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$WANT" ]; then
  bash "$ROOT/scripts/build_quickfix.sh"
  echo "$WANT" > "$STAMP"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DMINI_OEMS_QUICKFIX_PREFIX="$QUICKFIX_PREFIX"
cmake --build "$BUILD_DIR" -j "${JOBS:-4}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
