#!/bin/bash

# usage: ./unixBuild.sh [debug|release|test|leaks|clean]
#  - debug: builds in Debug mode and runs the app
#  - release (default): builds in Release mode and runs the app
#  - test: builds in Debug mode with tests enabled, runs all tests via CTest
#  - leaks: builds in Debug mode, runs the app and checks for memory leaks (macOS only)
#  - clean: removes the build directory and compiled shader files

set -e

if [[ "$1" == 'clean' ]]; then
	rm -rf build/
	rm -rf shaders/*.spv
	echo "Cleaned build directory"
	exit 0
fi

# Default build type is Release
BUILD_TYPE="Release"
MODE="$1"
EXTRA_CMAKE_ARGS=""
case "$MODE" in
	release)
		BUILD_TYPE="Release"
		echo "Building in Release mode";;
	debug)
		BUILD_TYPE="Debug"
		echo "Building in Debug mode";;
	test)
		BUILD_TYPE="Debug"
		echo "Building in Debug mode (tests enabled)";;
	leaks)
		BUILD_TYPE="Debug"
		echo "Building in Debug mode (leaks check)";;
	*)
		echo "Building in Release mode";;
esac

mkdir -p build
EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=OFF"
if [[ "$MODE" == 'test' ]]; then
	EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=ON"
fi

# Configure and build into ./build
cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" $EXTRA_CMAKE_ARGS
cmake --build build -j

if [[ "$MODE" == 'test' ]]; then
	# Run all tests via CTest; each test file builds its own executable
	ctest --test-dir build --output-on-failure || exit 2
	exit 0
elif [[ "$MODE" == 'leaks' ]]; then
	if [[ "$(uname)" == "Darwin" ]]; then
		./build/VeApp &
		PID=$!
		sleep 5 # Give the app a moment to run

		leaks $PID || true
		kill $PID || true
	else
		echo "'leaks' mode is only supported on macOS."
	fi
else
	./build/VeApp
fi
