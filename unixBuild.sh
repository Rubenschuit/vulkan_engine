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
EXTRA_CMAKE_ARGS=""
case "$MODE" in
  release)
    BUILD_TYPE="Release"
    echo "Building in Release mode" ;;
  test)
    echo "Building in Debug mode (tests enabled)" ;;
  leaks)
    echo "Building in Debug mode (leaks check)" ;;
  *)
    echo "Building in Debug mode" ;;
esac

mkdir -p build
cd build
EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=OFF"
if [[ "$MODE" == 'test' ]]; then
  EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=ON"
fi

cmake -S ../ -B . -DCMAKE_BUILD_TYPE=$BUILD_TYPE $EXTRA_CMAKE_ARGS
make || exit 1
make Shaders || exit 1

if [[ "$MODE" == 'test' ]]; then
  # Run all tests via CTest; each test file builds its own executable
  ctest --output-on-failure || exit 2
  exit 0
elif [[ "$MODE" == 'leaks' ]]; then
  if [[ "$(uname)" == "Darwin" ]]; then
    ./VEngine &
    PID=$!
    sleep 1 # Give the app a moment to start
    leaks $PID
    kill $PID
  else
    echo "'leaks' mode is only supported on macOS."
  fi
else
  ./VEngine
fi
cd ..