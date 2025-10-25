# Load local environment variables and use if present
include(./.env.cmake OPTIONAL RESULT_VARIABLE LOCAL_ENV)
message(STATUS "Local .env.cmake: ${LOCAL_ENV}")

cmake_minimum_required(VERSION 3.20)

# Use consistent runtime library for both Debug and Release to avoid DLL conflicts
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
endif()

set(NAME VeApp)

# Enforce C++20 uniformly, so no permissive for msvc
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for IntelliSense support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

include(${CMAKE_SOURCE_DIR}/cmake/Options.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Leaks.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/Targets.cmake)

foreach(shader_target IN LISTS SHADER_TARGETS)
	add_dependencies(${PROJECT_NAME} ${shader_target})
endforeach()

if (VE_BUILD_TESTS)
	include(${CMAKE_SOURCE_DIR}/cmake/Tests.cmake)
endif()

# Configure optimization levels for release-oriented builds (does not change default build type)
if (NOT MSVC)
	string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O3 -DNDEBUG")
endif()
