# Load local environment variables and use if present
include(./.env.cmake OPTIONAL RESULT_VARIABLE LOCAL_ENV)
message(STATUS "Local .env.cmake: ${LOCAL_ENV}")

cmake_minimum_required(VERSION 3.20)

set(NAME VeApp)

# Enforce C++20 uniformly, so no permissive for msvc
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use Vulkan 0-1 Z range for GLM
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)

# Generate compile_commands.json for IntelliSense support to /build/Debug/compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON )

add_compile_definitions(VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1)

message(STATUS "using ${CMAKE_GENERATOR}")
if (CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
	set(USE_MINGW TRUE)
	if (DEFINED MINGW_PATH)
		if (NOT CMAKE_C_COMPILER)
			set (CMAKE_C_COMPILER "${MINGW_PATH}/bin/gcc.exe" CACHE FILEPATH "MinGW C compiler")
		endif()
		if (NOT CMAKE_CXX_COMPILER)
			set (CMAKE_CXX_COMPILER "${MINGW_PATH}/bin/g++.exe" CACHE FILEPATH "MinGW C++ compiler")
		endif()
		message(STATUS "Using MinGW compilers from MINGW_PATH: ${MINGW_PATH}")
	else()
		message(STATUS "MINGW_PATH not set; using MinGW compilers from PATH")
	endif()
elseif (MSVC)
	message(STATUS "Configuring for MSVC compiler")
endif()

project(${NAME} VERSION 0.1)

# Prevent Windows.h from defining min/max macros that break std::min/std::max
if(WIN32)
	add_compile_definitions(NOMINMAX)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/Options.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Leaks.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Targets.cmake)

foreach(shader_target IN LISTS SHADER_TARGETS)
	add_dependencies(${PROJECT_NAME} ${shader_target})
endforeach()

if (VE_BUILD_TESTS)
	include(${CMAKE_SOURCE_DIR}/cmake/Tests.cmake)
endif()

# Configure optimization levels for release builds
if (MSVC)
	string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2 /Ob2 /GL")
	string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /LTCG")
	string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " /LTCG")
else()
	string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O3 -DNDEBUG")
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
message(STATUS "Release build flags: ${CMAKE_CXX_FLAGS_RELEASE}")
