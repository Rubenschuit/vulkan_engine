#!/bin/bash

# usage: ./unixBuild.sh [debug|release|test|leaks|clean]
#  - debug: builds in debug mode and runs the app
#  - release (default): builds in release mode and runs the app
#  - test: builds in debug mode with tests enabled, runs all tests via CTest
#  - leaks: builds in debug mode, runs the app and checks for memory leaks (macOS only)
#  - clean: removes the build directory and compiled shader files

set -e

if [[ "$1" == 'clean' ]]; then
	rm -rf build
	rm -rf shaders/*.spv
	echo "Cleaned build directory and shaders"
	exit 0
fi

# Default build type is release
BUILD_TYPE="Release"
MODE="$1"
EXTRA_CMAKE_ARGS=""
case "$MODE" in
	release)
		BUILD_TYPE="Release"
		echo "Building in release mode";;
	debug)
		BUILD_TYPE="Debug"
		echo "Building in debug mode";;
	test)
		BUILD_TYPE="Debug"
		echo "Building in debug mode (tests enabled)";;
	leaks)
		BUILD_TYPE="Debug"
		echo "Building in debug mode (leaks check)";;
	*)
		echo "Building in release mode";;
esac

mkdir -p build
EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=OFF"
if [[ "$MODE" == 'test' ]]; then
	EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=ON"
fi

# if windows
EXTRA_WINDOWS_ARGS=""
if [[ "$OSTYPE" == "msys" ]]; then
	EXTRA_WINDOWS_ARGS="-G 'MinGW Makefiles'"
fi

# build dir is ./build/{BUILD_TYPE}
BUILD_DIR="./build/$BUILD_TYPE"

# Configure and build into ./build
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" $EXTRA_CMAKE_ARGS $EXTRA_WINDOWS_ARGS
cmake --build "$BUILD_DIR" -j

if [[ "$MODE" == 'test' ]]; then
	# Run all tests via CTest; each test file builds its own executable
	ctest --test-dir "$BUILD_DIR" --output-on-failure || exit 2
	exit 0
elif [[ "$MODE" == 'leaks' ]]; then
	if [[ "$(uname)" == "Darwin" ]]; then
		./"$BUILD_DIR"/VeApp &
		PID=$!
		sleep 5 # Give the app a moment to run

		leaks $PID || true
		kill $PID || true
	else
		echo "'leaks' mode is only supported on macOS."
	fi
else
	./"$BUILD_DIR"/VeApp
fi
