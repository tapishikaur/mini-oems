#!/usr/bin/env bash
# One command to build and test MiniOEMS.
# Builds the pinned QuickFIX first if it's missing or out of date, then the project,
# then runs the tests.
#
# Usage:  scripts/build.sh [Release|Debug]      (default: Release)
# Env:    JOBS=N   parallel build jobs (default 4)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-Release}"
BUILD_DIR="$ROOT/build/$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    [ -d /c/msys64/mingw64/bin ] && export PATH="/c/msys64/mingw64/bin:$PATH" ;;
esac

# Rebuild QuickFIX only when the pinned commit or the patch has changed.
STAMP="$ROOT/build/_deps/quickfix-install/.stamp"
WANT="$(git hash-object "$ROOT/scripts/build_quickfix.sh" "$ROOT/third_party/quickfix/mingw.patch" | tr '\n' ' ')"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$WANT" ]; then
  bash "$ROOT/scripts/build_quickfix.sh"
  echo "$WANT" > "$STAMP"
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" -j "${JOBS:-4}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
