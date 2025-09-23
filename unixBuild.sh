#!/bin/bash


if [[ "$1" == 'clean' ]]; then
  rm -rf build/
  rm -rf shaders/*.spv
  echo "Cleaned build directory"
  exit 0
fi

# Default build type is Debug, unless 'release' is passed as argument
BUILD_TYPE="Debug"
if [[ "$1" == 'release' ]]; then
  BUILD_TYPE="Release"
  echo "Building in Release mode"
else
  echo "Building in Debug mode"
fi

mkdir -p build
cd build
cmake -S ../ -B . -DCMAKE_BUILD_TYPE=$BUILD_TYPE
make && make Shaders && ./VEngine
cd ..