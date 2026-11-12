#!/bin/sh
# Refresh the release folder and build the Inno Setup installer.
# Usage: tools/package.sh [release folder] [path to .iss]
set -e
STAGE="${1:-/c/Users/foryu/Downloads/v3.0.0}"
ISS="${2:-/c/Users/foryu/Downloads/OpenEditor.iss}"
ISCC="/c/Program Files/Inno Setup 7/ISCC.exe"
BUILD=build/ninja-release

./build.sh
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/6.11.1/mingw_64/bin:$PATH"
# Take FFmpeg from a full distribution (one with its LICENSE beside bin), not from an installed
# copy of the editor that happens to be on PATH.
FFBIN=""
for f in $(which -a ffmpeg 2>/dev/null); do
    d="$(dirname "$f")"
    if [ -f "$d/../LICENSE" ] && [ -f "$d/ffprobe.exe" ]; then FFBIN="$d"; break; fi
done
[ -n "$FFBIN" ] || { echo "No FFmpeg distribution with a LICENSE file found on PATH." >&2; exit 1; }

mkdir -p "$STAGE/licenses"
cp "$BUILD/OpenEditor.exe" "$BUILD/oe-mcp.exe" "$STAGE/"
cp -r "$BUILD/oe-plugins" "$STAGE/"
windeployqt --release "$STAGE/OpenEditor.exe"
windeployqt --release "$STAGE/oe-mcp.exe"
cp "$FFBIN/ffmpeg.exe" "$FFBIN/ffprobe.exe" "$STAGE/"
cp "$FFBIN/../LICENSE" "$STAGE/licenses/FFmpeg-LICENSE.txt"
cp src/ui/icons/LICENSE-tabler.txt "$STAGE/licenses/"

"$ISCC" "$(cygpath -w "$ISS")"
