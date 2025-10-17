# Vulkan Engine

Small modern C++20 Vulkan 1.3+ renderer using Vulkan-Hpp RAII, GLFW, and Slang. Produces a static library (`VEngineLib`) and an app (`VeApp`). Shaders compile to SPIR-V.

## Features
- Clear split between engine library and application (to be improved)
- Cross-platform builds: Windows (MSVC or MinGW), macOS (MoltenVK), and Linux
- Uses modern Vulkan features like dynamic rendering and timeline semaphores
- Simple render system to render .obj files with textures
- Simple point light system
- Particle system with compute shaders
- FPS-style camera
- More coming...

## Requirements

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
- Slang: https://github.com/shader-slang/slang/releases
- Vulkan SDK (LunarG): https://vulkan.lunarg.com/sdk/home

Note: CMake will auto-discover Slang (`slangc`) and the Vulkan SDK in common install locations and via standard environment variables. Use `.env.cmake` only when installing to uncommon/custom paths, or to explicitly override detection.

Download instructions and common install locations:
- ##### Windows
	- Slang: `C:\Program Files\Slang\bin\slangc.exe`
	- Vulkan SDK: `C:\VulkanSDK\<version>` 

	With MSYS2/MinGW (run in the MinGW64 shell):
	```bash
	pacman -S --needed \
		mingw-w64-x86_64-toolchain \
		mingw-w64-x86_64-cmake \
		mingw-w64-x86_64-glfw \
		mingw-w64-x86_64-glm \
	# Slang is not in MSYS2; install via releases or vcpkg (see links above)
	```
	Alternatively, use VS with vcpkg.

- ##### macOS
	- Slang: `/usr/local/bin/slangc`
	- Vulkan SDK: `/Applications/VulkanSDK/<version>`

	With homebrew:	
	```bash
	brew install cmake glfw glm
	```
- ##### Linux
	- Slang: `/usr/local/bin/slangc`
	- Vulkan SDK: system install (distro packages) or SDK under `/usr/local`

	TODO: add packages


## Quick start with scripts

##### macOS and Linux:

```bash
./unixBuild.sh           # Debug build, run
./unixBuild.sh release   # Release build, run
./unixBuild.sh test      # Build + run tests
./unixBuild.sh clean     # Remove build and shaders/*.spv
```

##### Windows (cmd or PowerShell):

```bat
windowsBuild.bat                 # Debug + MinGW, run
windowsBuild.bat release msvc    # Release + MSVC, run
windowsBuild.bat test mingw      # Build tests with MinGW
windowsBuild.bat clean           # Clean build dirs and shaders/*.spv

```

## Manual build
From repository root:

##### macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/VeApp
```

##### Windows with Visual Studio:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVE_FETCH_GLFW=ON
cmake --build build --config Debug
build\Debug\VeApp.exe
```

##### Windows with MinGW:

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
build\VeApp.exe
```

##### Linux

TODO

## Tests

- Enable with `-DVE_BUILD_TESTS=ON` or run `./unixBuild.sh test` / `windowsBuild.bat test <gen>`.
- Tests run via CTest: `ctest --test-dir build --output-on-failure` (add `-C Debug/Release` for MSVC).