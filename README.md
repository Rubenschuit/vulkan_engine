# Vulkan Engine
![PBR Rendering](screenshots/bistro_1.png)
![PBR Rendering](screenshots/sponza.png)

Cross-platform C++20 Vulkan 1.3+ GPU-driven clustered forward renderer with an integrated editor, built from scratch as a personal project to explore real-time graphics and engine architecture. Produces a shared library and an app.

**Built with:** C++20 · Vulkan 1.3 (Vulkan-Hpp, VMA, dynamic rendering, timeline semaphores) · Slang shaders · GLFW · Jolt Physics · Dear ImGui + ImGuizmo · meshoptimizer · KTX-Software · TinyGLTF + MikkTSpace · Catch2 · Tracy (optional)

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
  - [Downloads](#downloads)
- [Quick start with scripts](#quick-start-with-scripts)
  - [macOS, Linux, Windows (MinGW)](#macos-linux-and-windows-mingw64-shell)
  - [Windows (cmd or PowerShell)](#windows-cmd-or-powershell)
- [Manual build](#manual-build)
  - [Unix or MinGW shell](#unix-or-mingw64-shell)
  - [Windows (Visual Studio)](#windows-with-visual-studio)
- [Credits](#credits)


![Simple scene](screenshots/simple_scene.png)

![Particle System](screenshots/particles.png)
![Fireworks](screenshots/fireworks.png)

## Features

#### Rendering
- Modern Vulkan 1.3: dynamic rendering, timeline semaphores, Vulkan-Hpp RAII
- PBR (physically based rendering) for .gltf models
- Clustered forward rendering with depth pre-pass and infinite reverse-Z
- Image-Based Lighting
- Order-independent transparency (WBOIT)
- Bindless textures with KTX2 (BC7/ASTC) transcoding
- GPU-driven rendering: frustum + Hi-Z occlusion culling, multi-draw indirect, draw compaction
- Meshlet pipeline with two-pass meshlet culling
- Automatic LOD selection
- Multi-threaded command buffer recording with secondary command buffers
- Shaders written in Slang, compiled to SPIR-V at build time

#### Lighting & Shadows
- Point lights, directional lights, spot lights
- Cascaded Shadow Maps
- Screen-space shadow mask (compute), PCF, PCSS
- GTAO (Ground Truth Ambient Occlusion)

#### Architecture
- Shared-library + app split
- Custom ECS with typed component pools, parent/child hierarchy
- Engine-wide event bus and separate ECS-local event dispatcher
- Refcounted, type-keyed resource handles with auto-unload
- Per-frame primary and per-thread secondary command buffer management
- Async glTF loading

#### Simulation
- Rigid-body physics via Jolt
- Skeletal animation: glTF skin import, keyframed TRS clips, compute skinning
- Compute-based particle system, async on a dedicated compute queue when available

#### Post-processing & Effects
- Bloom
- HDR with several tone mapping options
- Fireworks demo, app-side example built on top of the engine's particle system
- Skybox
- MSAA


#### Editor & Tools
- Dear ImGui docked UI with hierarchy, inspector, viewport, graphics settings, performance, and environment panels
- ImGuizmo 3D transform gizmos
- Outline rendering for selected entities
- Tracy profiler integration (optional)
- Cross-platform builds: Windows (MSVC or MinGW), macOS, and Linux
- FPS-style camera (WASD + mouse)


## Requirements

- Git
- CMake ≥ 3.16
- C++20 toolchain: Clang 14+, MSVC 2019+, or GCC 11+
- Vulkan SDK ≥ 1.3 with Slang compiler

Fetched automatically if not found on the system:
- KTX, GLFW 3.3+, GLM, Meshoptimizer

Included in `external/`:
- TinyGLTF, Dear ImGui, ImGuizmo, Mikktspace, Portable File Dialogs


#### Downloads:
Besides git, cmake and a c++20 compiler, the Vulkan SDK must be installed manually.

- Vulkan SDK (LunarG): https://vulkan.lunarg.com/sdk/home
	- macOS and Windows: Check 'System global installation' component in the installer
	- Linux: Consult https://vulkan.lunarg.com/doc/sdk/1.4.328.1/linux/getting_started.html (1.4.328) for instructions to install the tar file.

- Extra (fetched automatically, but can be installed manually):
	- KTX: https://github.com/KhronosGroup/KTX-Software/releases
	- Slang (if not included in your Vulkan SDK): https://github.com/shader-slang/slang/releases
	- GLFW: https://www.glfw.org/download.html
	- GLM: https://github.com/g-truc/glm/releases

- Additional packages for fresh ubuntu install:
```bash
sudo apt update
sudo apt upgrade
sudo apt install git cmake xorg-dev libglfw3-dev libglm-dev libxcb-xinerama0-dev libxcb-xinput-dev libxcb-cursor-dev
```


## Quick start with scripts
After installing all the dependencies, we can build and run with one script.

- ##### macOS, Linux and Windows (MinGW64 shell):
Optional arguments include [release|debug|test|clean]. Default is release.
```bash
cd vulkan_engine
./unixBuild.sh
```

- ##### Windows (cmd or PowerShell):
Optional arguments include [release|debug|tracy|test|clean] and [vs2022|vs2026]. Default is release vs2026.
```cmd
cd vulkan_engine
.\windowsBuild.bat
```

## Manual build
From repository root:

##### Unix or MinGW64 shell:

```bash
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
./build/Release/VeApp
```

##### Windows with MSVC:

From command prompt:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
build\Release\VeApp.exe
```

Or generate VeApp.sln to open with Visual Studio:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
```

Then, in VS, right-click the VeApp target, set as startup project, build and then run (f5)

## Credits

Huge thanks to:

- Brendan Galea for his excellent Vulkan video series: https://www.youtube.com/@BrendanGalea
- The Khronos Vulkan Tutorial: https://docs.vulkan.org/tutorial/latest/00_Introduction.html
- Physically Based Rendering in Filament: https://google.github.io/filament/Filament.md.html
- Vulkan samples by Sascha Willems: https://github.com/SaschaWillems/Vulkan