# Option to enable macOS 'leaks' tool (Valgrind alternative)
option(VE_USE_LEAKS "Enable debug info and add 'leaks' target for macOS memory leak checking" OFF)

include(./.env.cmake OPTIONAL RESULT_VARIABLE LOCAL_ENV)
message(STATUS "Local .env.cmake: ${LOCAL_ENV}")

cmake_minimum_required(VERSION 3.16) # Need newer policy defaults & FetchContent improvements

set(NAME VEngine)

# Enforce C++20 uniformly
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1)

message(STATUS "using ${CMAKE_GENERATOR}")
if (CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
    if (NOT MINGW_PATH)
      message(FATAL_ERROR "MINGW_PATH not set in .env.cmake")
    endif()
    set(USE_MINGW "True")
    set(CMAKE_C_COMPILER ${MINGW_PATH}/bin/gcc.exe)
    set(CMAKE_CXX_COMPILER  ${MINGW_PATH}/bin/g++.exe)
endif()

project(${NAME} VERSION 0.1)

include(${CMAKE_SOURCE_DIR}/cmake/Options.cmake)

include(${CMAKE_SOURCE_DIR}/cmake/Leaks.cmake)


include(${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake)


include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)

include(${CMAKE_SOURCE_DIR}/cmake/Targets.cmake)


add_dependencies(VEngineLib Shaders)
add_dependencies(${PROJECT_NAME} Shaders)

if (VE_BUILD_TESTS)
  include(${CMAKE_SOURCE_DIR}/cmake/Tests.cmake)
endif()