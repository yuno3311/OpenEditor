#!/bin/sh
# Build from a shell without Qt Creator. Usage: ./build.sh [target]
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/Ninja:/c/Qt/Tools/CMake_64/bin:/c/Qt/6.11.1/mingw_64/bin:$PATH"
BUILD="build/ninja-release"
[ -f "$BUILD/build.ninja" ] || cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/c/Qt/6.11.1/mingw_64 -DCMAKE_CXX_COMPILER=g++ || exit 1
cmake --build "$BUILD" ${1:+--target "$1"}
