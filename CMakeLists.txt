# Option to enable macOS 'leaks' tool (Valgrind alternative)
option(VE_USE_LEAKS "Enable debug info and add 'leaks' target for macOS memory leak checking" OFF)

include(./.env.cmake OPTIONAL RESULT_VARIABLE LOCAL_ENV)
message(STATUS "Local .env.cmake: ${LOCAL_ENV}")

cmake_minimum_required(VERSION 3.16) # Need newer policy defaults & FetchContent improvements

set(NAME VeApp)

# Enforce C++20 uniformly
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1)

message(STATUS "using ${CMAKE_GENERATOR}")
if (CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
  set(USE_MINGW TRUE)
  if (DEFINED MINGW_PATH)
    # If a MinGW prefix is provided, prefer its compilers
    set(CMAKE_C_COMPILER "${MINGW_PATH}/bin/gcc.exe" CACHE FILEPATH "MinGW C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${MINGW_PATH}/bin/g++.exe" CACHE FILEPATH "MinGW C++ compiler" FORCE)
    message(STATUS "MinGW compilers set from MINGW_PATH: ${MINGW_PATH}")
  else()
    # Otherwise, rely on MinGW toolchain found in PATH
    message(STATUS "MINGW_PATH not set; using MinGW compilers from PATH")
  endif()
endif()

project(${NAME} VERSION 0.1)

include(${CMAKE_SOURCE_DIR}/cmake/Options.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Leaks.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Targets.cmake)

add_dependencies(VEngineLib Shaders)
add_dependencies(${PROJECT_NAME} Shaders)
add_dependencies(VEngineLib ShadersAxes)
add_dependencies(${PROJECT_NAME} ShadersAxes)
add_dependencies(VEngineLib ShadersPointLight)
add_dependencies(${PROJECT_NAME} ShadersPointLight)
add_dependencies(VEngineLib ShadersParticlesCompute)
add_dependencies(${PROJECT_NAME} ShadersParticlesCompute)
add_dependencies(VEngineLib ShadersParticlesBillboard)
add_dependencies(${PROJECT_NAME} ShadersParticlesBillboard)


if (VE_BUILD_TESTS)
	include(${CMAKE_SOURCE_DIR}/cmake/Tests.cmake)
endif()

# Configure optimization levels for release-oriented builds (does not change default build type)
if (NOT MSVC)
	string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O3 -DNDEBUG")
endif()
