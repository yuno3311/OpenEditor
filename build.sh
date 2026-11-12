#!/bin/sh
# Build Open Editor without Qt Creator.
#
# Works from Git Bash, MSYS2, Cygwin and WSL on Windows (and plain Linux with Qt 6 installed),
# from any folder. Qt, MinGW, CMake and Ninja are found automatically; set QT_DIR (the kit,
# e.g. C:/Qt/6.11.1/mingw_64) or QT_ROOT (e.g. D:/Qt) to choose a different installation.
#
# Usage: ./build.sh [options] [target]       ./build.sh --help for the list

usage() {
    cat <<'EOF'
Usage: ./build.sh [options] [target]

  target              build only this target (appOpen_Editor, oe-mcp, oe_v3_test, ...)
  -d, --debug         Debug build in build/ninja-debug (default: Release in build/ninja-release)
  -c, --clean         delete the build folder first and build everything again
      --reconfigure   run CMake again (after installing a new Qt, or when the cache is broken)
  -j N, --jobs N      run N compile jobs at once (default: all cores)
  -t, --test          run the headless test suites after building
  -v, --verbose       show every compiler command
  -h, --help          show this help

Environment: QT_DIR (Qt kit folder), QT_ROOT (Qt install folder, default C:/Qt),
             MINGW_DIR, CMAKE, NINJA (override the tools that are found)
EOF
}

die()  { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }
warn() { printf 'build.sh: warning: %s\n' "$*" >&2; }
note() { printf 'build.sh: %s\n' "$*"; }

# ---------------------------------------------------------------- options
CONFIG=Release; TARGET=""; CLEAN=0; RECONF=0; JOBS=""; VERBOSE=0; TEST=0
while [ $# -gt 0 ]; do
    case $1 in
        -h|--help|/?) usage; exit 0 ;;
        -d|--debug) CONFIG=Debug ;;
        -r|--release) CONFIG=Release ;;
        -c|--clean) CLEAN=1 ;;
        --reconfigure|--configure) RECONF=1 ;;
        -t|--test) TEST=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -j|--jobs) [ $# -gt 1 ] || die "$1 needs a number"; JOBS=$2; shift ;;
        -j*) JOBS=${1#-j} ;;
        --jobs=*) JOBS=${1#--jobs=} ;;
        --) shift; [ $# -gt 0 ] && TARGET=$1; break ;;
        -*) die "unknown option '$1' (see ./build.sh --help)" ;;
        *) [ -z "$TARGET" ] || die "only one target can be given ('$TARGET' and '$1')"; TARGET=$1 ;;
    esac
    shift
done
case $JOBS in ''|*[!0-9]*) [ -z "$JOBS" ] || die "the job count must be a number, not '$JOBS'" ;; esac
[ "$JOBS" != 0 ] || die "the job count must be at least 1"

# Run from the project folder, wherever the script was started from.
cd "$(dirname "$0")" 2>/dev/null || die "cannot enter the folder that holds build.sh"
[ -f CMakeLists.txt ] || die "CMakeLists.txt is not next to build.sh ($(pwd))"
SRC=$(pwd)

# A script saved with Windows line endings fails in confusing ways; say so plainly.
if grep -q "$(printf '\r')" "$0" 2>/dev/null; then
    die "build.sh has Windows (CRLF) line endings. Convert it with: sed -i 's/\\r\$//' build.sh"
fi

# ---------------------------------------------------------------- environment
case $(uname -s 2>/dev/null) in
    MINGW*|MSYS*) ENV=msys ;;
    CYGWIN*) ENV=cygwin ;;
    Linux*) if grep -qi microsoft /proc/version 2>/dev/null; then ENV=wsl; else ENV=linux; fi ;;
    *) ENV=other ;;
esac

# winpath: a path as Windows tools want it (C:/x/y). unixpath: back to this shell's form.
winpath() {
    case $ENV in
        msys|cygwin) cygpath -m "$1" ;;
        wsl) wslpath -m "$1" 2>/dev/null || printf '%s\n' "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}
unixpath() {
    case $ENV in
        msys|cygwin) cygpath -u "$1" ;;
        wsl) wslpath -u "$1" 2>/dev/null || printf '%s\n' "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}
EXE=""; [ "$ENV" = linux ] || [ "$ENV" = other ] || EXE=.exe

# Newest entry of a folder listing by version number (6.9 < 6.10 < 6.11.1, mingw810 < mingw1310).
newest() { sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1; }

# ---------------------------------------------------------------- tools
find_tools() {
    if [ "$ENV" = linux ] || [ "$ENV" = other ]; then
        CMAKE=${CMAKE:-$(command -v cmake)}; NINJA=${NINJA:-$(command -v ninja)}
        [ -n "$CMAKE" ] || die "cmake was not found. Install it (e.g. sudo apt install cmake)."
        [ -n "$NINJA" ] || die "ninja was not found. Install it (e.g. sudo apt install ninja-build)."
        [ -n "${QT_DIR:-}" ] && QTKIT=$QT_DIR || QTKIT=""
        CXX=""; CC=""
        return
    fi

    # Qt kit: QT_DIR, else the newest Qt 6 MinGW kit under QT_ROOT / C:/Qt / D:/Qt.
    QTKIT=""
    if [ -n "${QT_DIR:-}" ]; then
        QTKIT=$(unixpath "$QT_DIR")
        [ -d "$QTKIT/lib/cmake/Qt6" ] || die "QT_DIR=$QT_DIR is not a Qt 6 kit (no lib/cmake/Qt6 in it)"
    else
        for root in ${QT_ROOT:+"$(unixpath "$QT_ROOT")"} "$(unixpath C:/Qt)" "$(unixpath D:/Qt)"; do
            [ -d "$root" ] || continue
            ver=$(ls "$root" 2>/dev/null | grep -E '^6\.[0-9]+(\.[0-9]+)?$' | newest)
            # Walk down from the newest version until one has a MinGW kit.
            for v in $(ls "$root" 2>/dev/null | grep -E '^6\.[0-9]+(\.[0-9]+)?$' | sort -t. -k1,1nr -k2,2nr -k3,3nr); do
                if [ -d "$root/$v/mingw_64/lib/cmake/Qt6" ]; then QTKIT="$root/$v/mingw_64"; break; fi
            done
            [ -n "$QTKIT" ] && { QTROOT=$root; break; }
            [ -n "$ver" ] && warn "Qt $ver in $(winpath "$root") has no mingw_64 kit"
        done
        [ -n "$QTKIT" ] || die "no Qt 6 MinGW kit found. Install 'MinGW 64-bit' for Qt 6 with the Qt
  Maintenance Tool, or point QT_DIR at the kit, e.g.  QT_DIR=C:/Qt/6.11.1/mingw_64 ./build.sh"
    fi
    QTROOT=${QTROOT:-$(cd "$QTKIT/../.." && pwd)}

    # MinGW: MINGW_DIR, else the newest Tools/mingw*_64 next to Qt, else g++ on PATH.
    if [ -n "${MINGW_DIR:-}" ]; then
        MINGW=$(unixpath "$MINGW_DIR")
    else
        m=$(ls "$QTROOT/Tools" 2>/dev/null | grep -E '^mingw[0-9]+_64$' | sed 's/^mingw\([0-9]*\)_64$/\1 &/' | sort -n | tail -n 1 | cut -d' ' -f2)
        MINGW=${m:+$QTROOT/Tools/$m}
    fi
    if [ -n "$MINGW" ] && [ -x "$MINGW/bin/g++.exe" ]; then
        CXX="$MINGW/bin/g++.exe"; CC="$MINGW/bin/gcc.exe"
    else
        CXX=$(command -v g++.exe 2>/dev/null || true); CC=$(command -v gcc.exe 2>/dev/null || true)
        [ -n "$CXX" ] || die "MinGW (g++) was not found. Install 'MinGW 13.1.0 64-bit' under Developer
  and Designer Tools in the Qt Maintenance Tool, or set MINGW_DIR."
        MINGW=$(dirname "$(dirname "$CXX")")
    fi

    # CMake and Ninja: Qt's own copies first, then PATH.
    if [ -z "${CMAKE:-}" ]; then
        if [ -x "$QTROOT/Tools/CMake_64/bin/cmake.exe" ]; then CMAKE="$QTROOT/Tools/CMake_64/bin/cmake.exe"
        else CMAKE=$(command -v cmake.exe 2>/dev/null || command -v cmake 2>/dev/null || true); fi
    fi
    [ -n "$CMAKE" ] || die "CMake was not found. Install CMake under Developer and Designer Tools
  in the Qt Maintenance Tool, or put cmake on PATH."
    if [ -z "${NINJA:-}" ]; then
        if [ -x "$QTROOT/Tools/Ninja/ninja.exe" ]; then NINJA="$QTROOT/Tools/Ninja/ninja.exe"
        else NINJA=$(command -v ninja.exe 2>/dev/null || command -v ninja 2>/dev/null || true); fi
    fi
    [ -n "$NINJA" ] || die "Ninja was not found. Install Ninja under Developer and Designer Tools
  in the Qt Maintenance Tool, or put ninja on PATH."

    # The compiler, Qt's DLLs (for the tests and for moc/rcc) and the tools go first on PATH.
    # WSL passes PATH on to Windows programs, translated, so this works there too.
    PATH="$MINGW/bin:$QTKIT/bin:$(dirname "$NINJA"):$(dirname "$CMAKE"):$PATH"
    export PATH
}
find_tools
CTEST="$(dirname "$CMAKE")/ctest$EXE"; [ -x "$CTEST" ] || CTEST=ctest

if [ "$CONFIG" = Debug ]; then BUILD=build/ninja-debug; else BUILD=build/ninja-release; fi

# ---------------------------------------------------------------- configure
lower() { tr '[:upper:]' '[:lower:]' | tr '\\' '/'; }
cache_value() { sed -n "s/^$1:[A-Z]*=//p" "$BUILD/CMakeCache.txt" 2>/dev/null | tr -d '\r' | head -n 1; }

if [ $CLEAN = 1 ] && [ -d "$BUILD" ]; then
    note "removing $BUILD"
    rm -rf "$BUILD" || die "could not remove $BUILD. Close OpenEditor and any test that is still running."
fi

if [ -f "$BUILD/CMakeCache.txt" ]; then
    # A cache from another folder (the project was moved or copied) cannot be reused.
    home=$(cache_value CMAKE_HOME_DIRECTORY | lower)
    here=$(winpath "$SRC" | lower)
    if [ -n "$home" ] && [ "$home" != "$here" ]; then
        warn "$BUILD was configured for $(cache_value CMAKE_HOME_DIRECTORY); starting it over here"
        RECONF=1
    fi
    # A cache whose compiler has since been removed or upgraded.
    cxx=$(cache_value CMAKE_CXX_COMPILER)
    if [ -n "$cxx" ] && [ ! -e "$(unixpath "$cxx")" ] && ! command -v "$cxx" >/dev/null 2>&1; then
        warn "the compiler this build was set up with is gone ($cxx); configuring again"
        RECONF=1
    fi
fi
if [ $RECONF = 1 ]; then rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"; fi

if [ ! -f "$BUILD/build.ninja" ] || [ ! -f "$BUILD/CMakeCache.txt" ]; then
    note "configuring $CONFIG build ($ENV shell)"
    note "  Qt:       $(winpath "${QTKIT:-system}")"
    [ -n "$CXX" ] && note "  compiler: $(winpath "$CXX")"
    set -- -S "$(winpath "$SRC")" -B "$(winpath "$SRC/$BUILD")" -G Ninja -DCMAKE_BUILD_TYPE=$CONFIG
    [ -n "$NINJA" ] && set -- "$@" "-DCMAKE_MAKE_PROGRAM=$(winpath "$NINJA")"
    [ -n "$QTKIT" ] && set -- "$@" "-DCMAKE_PREFIX_PATH=$(winpath "$QTKIT")"
    [ -n "$CXX" ] && set -- "$@" "-DCMAKE_CXX_COMPILER=$(winpath "$CXX")" "-DCMAKE_C_COMPILER=$(winpath "$CC")"
    # The icon resource compiler by full path: build steps run through cmd.exe, which under WSL
    # does not always see the PATH set here.
    [ -x "$MINGW/bin/windres.exe" ] && set -- "$@" "-DCMAKE_RC_COMPILER=$(winpath "$MINGW/bin/windres.exe")"
    if ! "$CMAKE" "$@"; then
        rm -f "$BUILD/CMakeCache.txt"   # so the next run starts clean instead of reusing a half cache
        die "CMake could not configure the project (see the messages above)."
    fi
fi

# ---------------------------------------------------------------- build
running_editor() {
    [ -n "$EXE" ] || return 1
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' tasklist.exe /NH /FI "IMAGENAME eq OpenEditor.exe" 2>/dev/null | grep -qi OpenEditor.exe
}

set -- --build "$(winpath "$SRC/$BUILD")"
[ -n "$TARGET" ] && set -- "$@" --target "$TARGET"
[ -n "$JOBS" ] && set -- "$@" --parallel "$JOBS"
[ $VERBOSE = 1 ] && set -- "$@" --verbose

start=$(date +%s 2>/dev/null || echo 0)
"$CMAKE" "$@"
rc=$?
if [ $rc != 0 ]; then
    printf '\n' >&2
    if running_editor; then
        warn "OpenEditor.exe is running. Windows locks a running program, so it cannot be rebuilt."
        warn "Close the editor (and any test still running) and build again."
    else
        case $TARGET in
            ''|all|clean) ;;
            *) grep -q "^build $TARGET\(\.exe\)\?:" "$BUILD/build.ninja" 2>/dev/null ||
                   warn "there is no target named '$TARGET'. Try: ./build.sh, ./build.sh appOpen_Editor, ./build.sh oe_v3_test" ;;
        esac
        warn "if the errors mention missing files or a moved folder, try: ./build.sh --reconfigure"
    fi
    die "the build failed (exit code $rc)."
fi
end=$(date +%s 2>/dev/null || echo 0)
[ "$start" != 0 ] && note "$CONFIG build finished in $((end - start))s: $BUILD"

# ---------------------------------------------------------------- tests
if [ $TEST = 1 ]; then
    note "running the headless test suites"
    if ! "$CTEST" --test-dir "$(winpath "$SRC/$BUILD")" --output-on-failure ${JOBS:+-j "$JOBS"}; then
        die "some tests failed (see above)."
    fi
fi
exit 0
