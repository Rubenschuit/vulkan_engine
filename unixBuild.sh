#!/bin/bash

# usage: ./unixBuild.sh [debug|release|test|tracy|leaks|clean] [no-vk-val]
#  - debug: builds in debug mode and runs the app
#  - release (default): builds in release mode and runs the app
#  - test: builds in debug mode with tests enabled, runs all tests via CTest
#  - tracy: builds in RelWithDebInfo mode with Tracy profiler enabled
#  - leaks: builds in debug mode, runs the app and checks for memory leaks (macOS only)
#  - clean: removes the build directory and compiled shader files
#  - no-vk-val: skips enabling VK_LAYER_KHRONOS_validation and the engine debug messenger

set -e

DISABLE_VK_VALIDATION="OFF"
MODE=""
for arg in "$@"; do
	case "$arg" in
		no-vk-val|--no-vk-val|--no-vk-validation)
			DISABLE_VK_VALIDATION="ON";;
		*)
			if [[ -z "$MODE" ]]; then
				MODE="$arg"
			fi;;
	esac
done

if [[ "$MODE" == 'clean' ]]; then
	rm -rf build
	rm -rf shaders/*.spv
	echo "Cleaned build directory and shaders"
	exit 0
fi

# Default build type is release
BUILD_TYPE="Release"
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
	tracy)
		BUILD_TYPE="RelWithDebInfo"
		echo "Building in RelWithDebInfo mode (Tracy profiler enabled)";;
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
elif [[ "$MODE" == 'tracy' ]]; then
	EXTRA_CMAKE_ARGS="-DVE_BUILD_TESTS=OFF -DVE_ENABLE_TRACY=ON"
fi

EXTRA_CMAKE_ARGS="$EXTRA_CMAKE_ARGS -DVE_DISABLE_VK_VALIDATION=$DISABLE_VK_VALIDATION"
if [[ "$DISABLE_VK_VALIDATION" == "ON" ]]; then
	echo "Engine validation layer + debug messenger disabled (no-vk-val)"
fi

GENERATOR_ARGS=""
if [[ "$OSTYPE" == "msys" ]]; then
	GENERATOR_ARGS="-G 'MinGW Makefiles'"
elif command -v ninja >/dev/null 2>&1; then
	GENERATOR_ARGS="-G Ninja"
fi

if [[ "$(uname)" == "Darwin" ]]; then
	JOBS=$(sysctl -n hw.physicalcpu)
elif command -v nproc >/dev/null 2>&1; then
	JOBS=$(nproc)
else
	JOBS=4
fi

# build dir is ./build/{BUILD_TYPE}
BUILD_DIR="./build/$BUILD_TYPE"

# Configure and build into ./build
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $EXTRA_CMAKE_ARGS $GENERATOR_ARGS
cp "$BUILD_DIR/compile_commands.json" ./compile_commands.json 2>/dev/null || true
cmake --build "$BUILD_DIR" -j"$JOBS"

if [[ "$MODE" == 'test' ]]; then
	# Run all tests via CTest; each test file builds its own executable
	ctest --test-dir "$BUILD_DIR" --output-on-failure || exit 2
	exit 0
elif [[ "$MODE" == 'leaks' ]]; then
	if [[ "$(uname)" == "Darwin" ]]; then
		./"$BUILD_DIR"/VeApp &
		PID=$!
		sleep 40 # Give the app a moment to run

		leaks $PID || true
		kill $PID || true
	else
		echo "'leaks' mode is only supported on macOS."
	fi
else
	./"$BUILD_DIR"/VeApp
fi
