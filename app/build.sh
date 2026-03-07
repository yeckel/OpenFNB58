#!/bin/bash
# Build script for FNB58 Qt application
set -e
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake -DCMAKE_PREFIX_PATH=/opt/Qt/6.9.2/gcc_64 \
      -DCMAKE_BUILD_TYPE=Release \
      ..
cmake --build . --parallel "$(nproc)"
echo ""
echo "Build complete → $(pwd)/bin/fnb58app"
echo "Run: ./bin/fnb58app"
