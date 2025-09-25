#!/bin/bash


if [[ "$1" == 'clean' ]]; then
  rm -rf build/
  rm -rf shaders/*.spv
  echo "Cleaned build directory"
  exit 0
fi

# Default build type is Debug, unless 'release' is passed as argument
BUILD_TYPE="Debug"
MODE="$1"
case "$MODE" in
  release)
    BUILD_TYPE="Release"
    echo "Building in Release mode" ;;
  test)
    echo "Building in Debug mode (tests enabled)" ;;
  *)
    echo "Building in Debug mode" ;;
esac

mkdir -p build
cd build
cmake -S ../ -B . -DCMAKE_BUILD_TYPE=$BUILD_TYPE
make || exit 1
make Shaders || exit 1

if [[ "$MODE" == 'test' ]]; then
  ctest --output-on-failure || exit 2
fi

./VEngine
cd ..