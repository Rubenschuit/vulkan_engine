# Vulkan Engine

Simple modern C++20 Vulkan renderer using Vulkan-Hpp RAII, GLFW, and Slang. This repository builds a small engine library (`VEngineLib`) and an executable (`VEngine`). Shaders are compiled to SPIR-V.

## Prerequisites

- CMake ≥ 3.16
- A C++20 compiler (Clang 14+/MSVC 2019+/GCC 11+)
- Vulkan SDK (MoltenVK on macOS)
- GLFW 3.3+
- Slang (`slangc`) on PATH for shader compilation


## macOS (MoltenVK)

1) Install dependencies

- Using Homebrew:
	- brew install glfw glm glslang

2) Vulkan SDK

- https://vulkan.lunarg.com/doc/sdk/1.4.321.0/mac/getting_started.html

3) Configure and build

- Debug (default):
	- cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
	- cmake --build build

- Release:
	- cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	- cmake --build build --config Release

4) Run

- From build directory: ./VeApp

Or use script to build and run:

- ./unixBuild.sh
- ./unixBuild.sh release
- ./unixBuild.sh test
- ./unixBuild.sh clean



## Windows

Visual Studio
1) Install Vulkan SDK (Windows installer) and ensure `slangc` is on PATH
2) Configure + build:
	 - cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVE_FETCH_GLFW=ON
	 - cmake --build build --config Debug
3) Run: build/Debug/VeApp.exe

MinGW Makefiles
1) Install MSYS2 MinGW-w64 and open the MinGW x64 shell (ensure `g++` and `slangc` are available).
2) Configure + build:
	 - cmake -S . -B build -G "MinGW Makefiles" -DVE_FETCH_GLFW=ON
	 - cmake --build build
3) Run: build/VeApp.exe

## Linux

todo