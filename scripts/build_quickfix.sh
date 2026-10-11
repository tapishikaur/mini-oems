#!/usr/bin/env bash
# Build QuickFIX from a pinned upstream commit, with the MinGW patch applied,
# and install it under <build root>/_deps/quickfix-install (see scripts/common.sh).
#
# Works on Linux and on Windows (Git Bash or MSYS2 shell, MinGW-w64 GCC).
# Usage:  scripts/build_quickfix.sh
# Env:    QUICKFIX_TESTS=ON  also build QuickFIX's own test suites (ut, at)
#         JOBS=N             parallel build jobs (default 4)
set -euo pipefail

QUICKFIX_REPO="https://github.com/quickfix/quickfix.git"
QUICKFIX_COMMIT="386ce46e917ae494ab6e90b1be90fd421cdbe3f9"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/common.sh"
PATCH="$ROOT/third_party/quickfix/mingw.patch"
DEPS="$MINI_OEMS_BUILD_ROOT/_deps"
SRC="$DEPS/quickfix-src"
BLD="$DEPS/quickfix-build"
QUICKFIX_TESTS="${QUICKFIX_TESTS:-OFF}"
JOBS="${JOBS:-4}"

# Fetch exactly the pinned commit. autocrlf=false keeps files byte-identical to the
# upstream repository on every OS, so the patch applies the same way everywhere.
if [ ! -d "$SRC/.git" ]; then
  git init -q "$SRC"
  git -C "$SRC" config core.autocrlf false
  git -C "$SRC" remote add origin "$QUICKFIX_REPO"
fi
git -C "$SRC" fetch -q --depth 1 origin "$QUICKFIX_COMMIT"
git -C "$SRC" checkout -q --force FETCH_HEAD
git -C "$SRC" clean -q -fdx            # drop outputs of any previous build in the clone
git -C "$SRC" apply --whitespace=nowarn "$PATCH"

cmake -S "$SRC" -B "$BLD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$QUICKFIX_PREFIX" \
  -DQUICKFIX_SHARED_LIBS=OFF \
  -DQUICKFIX_EXAMPLES=OFF \
  -DQUICKFIX_TESTS="$QUICKFIX_TESTS" \
  -DHAVE_SSL=OFF
cmake --build "$BLD" -j "$JOBS"
cmake --install "$BLD" > /dev/null

echo "QuickFIX ${QUICKFIX_COMMIT:0:8} installed to: $QUICKFIX_PREFIX"
