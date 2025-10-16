#!/bin/bash

set -e

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
EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=OFF"
if [[ "$MODE" == 'test' ]]; then
	EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=ON"
fi

# Configure and build into ./build
cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" $EXTRA_CMAKE_ARGS
cmake --build build -j
# Ensure shader SPIR-V targets are also up to date (redundant if add_dependencies is set)
cmake --build build --target Shaders -j || true

if [[ "$MODE" == 'test' ]]; then
	# Run all tests via CTest; each test file builds its own executable
	ctest --test-dir build --output-on-failure || exit 2
	exit 0
elif [[ "$MODE" == 'leaks' ]]; then
	if [[ "$(uname)" == "Darwin" ]]; then
		./build/VEngine &
		PID=$!
		sleep 1 # Give the app a moment to start
		leaks $PID || true
		kill $PID || true
	else
		echo "'leaks' mode is only supported on macOS."
	fi
else
	./build/VEngine
fi
