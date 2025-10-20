# Vulkan Engine

Small modern C++20 Vulkan 1.3+ renderer using Vulkan-Hpp RAII, GLFW, and Slang. Produces a static library (`VEngineLib`) and an app (`VeApp`).

## Features
- Modern Vulkan: dynamic rendering, Vulkan-Hpp RAII
- Cross-platform builds: Windows (MSVC or MinGW), macOS (MoltenVK), and Linux
- Dear ImGui overlay
- Particle system with compute shaders
- Simple renderer for textured .obj models and a skybox
- Point lights
- FPS-style camera

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
  - [Downloads](#downloads)
    - [Windows](#windows)
    - [macOS](#macos)
    - [Linux](#linux)
- [Quick start with scripts](#quick-start-with-scripts)
  - [macOS, Linux, MinGW](#macos-linux-and-windows-mingw64-shell)
  - [Windows (cmd or PowerShell)](#windows-cmd-or-powershell)
- [Manual build](#manual-build)
  - [Unix or MinGW shell](#unix-or-mingw64-shell)
  - [Windows (Visual Studio)](#windows-with-visual-studio)
  - [Linux](#linux-1)
- [Controls](#controls)
- [Credits](#credits)


## Requirements

- Git
- CMake ≥ 3.16
- C++20 toolchain (Clang 14+/MSVC 2019+/GCC 11+)
- Vulkan SDK ≥ 1.3 (MoltenVK on macOS)
- Slang compiler (`slangc`) — required for building shaders
- GLFW 3.3+ (automatically fetched if missing)
- Tinyobjloader (included in external)
- stb_image (included in external)

Optional environment file: `.env.cmake` in repository root. Common variables:
- `MINGW_PATH`
- `GLFW_PATH`
- `VULKAN_SDK_PATH`
- `SLANG_HOME`

#### Downloads:
Note: CMake will auto-discover Slang (`slangc`) and the Vulkan SDK in common install locations and via standard environment variables. Use `.env.cmake` only when installing to uncommon/custom paths, or to explicitly override detection.

- Slang: https://github.com/shader-slang/slang/releases
	- Windows: `C:\Program Files\Slang\bin\slangc.exe`
	- Unix: `/usr/local/bin/slangc`

- Vulkan SDK (LunarG): https://vulkan.lunarg.com/sdk/home

- Optional (will be fetched if no path is set):
	- GLFW: https://www.glfw.org/download.html
	- GLM: https://github.com/g-truc/glm/releases

To install required packages:
- ##### Windows
	With MSYS2/MinGW (run in the MinGW64 shell):
	```bash
	pacman -S --needed \
		mingw-w64-x86_64-toolchain \
		mingw-w64-x86_64-cmake \
		mingw-w64-x86_64-glfw \
		mingw-w64-x86_64-glm
	```
	Alternatively, use VS with vcpkg.

- ##### macOS
	With homebrew:
	```bash
	brew install cmake glfw glm
	```
- ##### Linux
	TODO: add packages


## Quick start with scripts
Build and run with one script

##### macOS, Linux and Windows (MinGW64 shell):

```bash
./unixBuild.sh
```
usage:

	./unixBuild.sh [debug|release|test|leaks|clean]

##### Windows (cmd or PowerShell):

```cmd
.\windowsBuild.bat
```
Usage:

	windowsBuild.bat [mode] [generator]

	mode: debug | release (default) | test | clean

	generator: mingw | msvc (default)


## Manual build
From repository root:

##### Unix or MinGW64 shell:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/VeApp
```

##### Windows with Visual Studio:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\release\VeApp.exe
```

##### Linux

TODO

## Controls

- Camera: WASD/C/Space to move, arrow keys or mouse to look
- UI toggle: Tab
- Particle modes: 1 / 2 / 3 / 4
- Reset particles: F (explosion) or G (rotating disk)

## Credits

Huge thanks to:

- Brendan Galea for his excellent Vulkan video series: https://www.youtube.com/@BrendanGalea
- The Khronos Vulkan Tutorial: https://docs.vulkan.org/tutorial/latest/00_Introduction.html