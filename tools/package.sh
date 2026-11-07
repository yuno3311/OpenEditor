#!/bin/sh
# Refresh the release folder and build the Inno Setup installer.
# Usage: tools/package.sh [release folder] [path to .iss]
set -e
STAGE="${1:-/c/Users/foryu/Downloads/v1.0.0}"
ISS="${2:-/c/Users/foryu/Downloads/OpenEditor.iss}"
ISCC="/c/Program Files/Inno Setup 7/ISCC.exe"
BUILD=build/ninja-release

./build.sh
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/6.11.1/mingw_64/bin:$PATH"
FFBIN="$(dirname "$(command -v ffmpeg)")"

mkdir -p "$STAGE/licenses"
cp "$BUILD/OpenEditor.exe" "$BUILD/oe-mcp.exe" "$STAGE/"
cp -r "$BUILD/oe-plugins" "$STAGE/"
windeployqt --release "$STAGE/OpenEditor.exe"
windeployqt --release "$STAGE/oe-mcp.exe"
cp "$FFBIN/ffmpeg.exe" "$FFBIN/ffprobe.exe" "$STAGE/"
cp "$FFBIN/../LICENSE" "$STAGE/licenses/FFmpeg-LICENSE.txt"
cp src/ui/icons/LICENSE-tabler.txt "$STAGE/licenses/"

"$ISCC" "$(cygpath -w "$ISS")"
