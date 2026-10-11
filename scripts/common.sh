# Shared settings for the build scripts. Sourced, not run; the caller sets ROOT.

# Build output goes in build/ beside the sources, unless MINI_OEMS_BUILD_ROOT is set.
MINI_OEMS_DEFAULT_BUILD_ROOT="$ROOT/build"

case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    # Prefer the MSYS2 MinGW-w64 toolchain (g++, ninja) if it's installed.
    [ -d /c/msys64/mingw64/bin ] && export PATH="/c/msys64/mingw64/bin:$PATH"
    # Inside a OneDrive folder, OneDrive syncs the build output and turns folders into
    # read-only cloud placeholders, so later builds can't clean or rewrite them.
    # There, default to a local folder that isn't synced.
    case "$ROOT" in
      */OneDrive*) MINI_OEMS_DEFAULT_BUILD_ROOT="$(cygpath -u "$LOCALAPPDATA")/mini-oems/build" ;;
    esac
    ;;
esac

MINI_OEMS_BUILD_ROOT="${MINI_OEMS_BUILD_ROOT:-$MINI_OEMS_DEFAULT_BUILD_ROOT}"
QUICKFIX_PREFIX="$MINI_OEMS_BUILD_ROOT/_deps/quickfix-install"
