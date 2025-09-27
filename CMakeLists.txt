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

# Options
option(VE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(VE_BUILD_TESTS "Build unit tests" ON)
option(VE_FETCH_GLFW "Fetch GLFW if not found" OFF)
option(VE_BUILD_SHADERS "Build shaders" ON)
## Slang is the only supported shader compiler path (simpler, single-file module)

# If VE_USE_LEAKS is enabled, force debug symbols
if(VE_USE_LEAKS)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
  add_compile_options(-g)
endif()
## Slang is the only supported shader compiler path (simpler, single-file module)


# Utility: Create a shader target using Slang
# Usage:
#   add_slang_shader_target(<TARGET>
#     SOURCES <slang files>
#     [VERT_ENTRY <name>] [FRAG_ENTRY <name>]
#     [PROFILE <spirv_profile>] [OUT_DIR <dir>])
# Defaults: VERT_ENTRY=vertMain, FRAG_ENTRY=fragMain, PROFILE=spirv_1_5, OUT_DIR=${PROJECT_SOURCE_DIR}/shaders
function(add_slang_shader_target TARGET)
  set(options "")
  set(oneValueArgs OUT_DIR OUT_FILE VERT_ENTRY FRAG_ENTRY PROFILE)
  set(multiValueArgs SOURCES)
  cmake_parse_arguments(SHADER "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if (NOT SHADER_SOURCES)
    message(FATAL_ERROR "add_slang_shader_target: SOURCES not specified")
  endif()

  if (NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR "add_slang_shader_target: SLANGC_EXECUTABLE not found")
  endif()

  set(_OUT_DIR ${SHADER_OUT_DIR})
  if (NOT _OUT_DIR)
    set(_OUT_DIR ${PROJECT_SOURCE_DIR}/shaders)
  endif()

  set(_VERT_ENTRY ${SHADER_VERT_ENTRY})
  if (NOT _VERT_ENTRY)
    set(_VERT_ENTRY vertMain)
  endif()

  set(_FRAG_ENTRY ${SHADER_FRAG_ENTRY})
  if (NOT _FRAG_ENTRY)
    set(_FRAG_ENTRY fragMain)
  endif()

  set(_PROFILE ${SHADER_PROFILE})
  if (NOT _PROFILE)
    set(_PROFILE spirv_1_5)
  endif()

  # Ensure output directory exists at build time
  add_custom_command(
    OUTPUT ${_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_OUT_DIR}
    COMMENT "Creating shader output directory: ${_OUT_DIR}"
    VERBATIM
  )

  set(_SPV_FILES)
  foreach(SLANG_SRC ${SHADER_SOURCES})
    get_filename_component(_FN ${SLANG_SRC} NAME_WE)
    set(_OUT_FILE ${SHADER_OUT_FILE})
    if (NOT _OUT_FILE)
      set(_OUT_FILE ${_OUT_DIR}/${_FN}.spv)
    endif()

    add_custom_command(
      OUTPUT ${_OUT_FILE}
      COMMAND ${SLANGC_EXECUTABLE} ${SLANG_SRC} -target spirv -profile ${_PROFILE} -entry ${_VERT_ENTRY} -stage vertex -entry ${_FRAG_ENTRY} -stage fragment -emit-spirv-directly -fvk-use-entrypoint-name -o ${_OUT_FILE}
      DEPENDS ${SLANG_SRC} ${_OUT_DIR}
      WORKING_DIRECTORY ${_OUT_DIR}
      COMMENT "Slang compiling ${_FN}.slang -> ${_FN}.spv (entries: ${_VERT_ENTRY}, ${_FRAG_ENTRY})"
      VERBATIM
    )
    list(APPEND _SPV_FILES ${_OUT_FILE})
  endforeach()

  add_custom_target(${TARGET} DEPENDS ${_SPV_FILES})
endfunction()


# 1. Set VULKAN_SDK_PATH in .env.cmake to target specific vulkan version
if (DEFINED VULKAN_SDK_PATH)
  set(Vulkan_INCLUDE_DIRS "${VULKAN_SDK_PATH}/Include")
  if (APPLE)
    # On macOS, MoltenVK libs typically reside in lib or lib/macOS
    if (EXISTS "${VULKAN_SDK_PATH}/lib")
      set(Vulkan_LIBRARIES "${VULKAN_SDK_PATH}/lib")
    elseif (EXISTS "${VULKAN_SDK_PATH}/lib/macOS")
      set(Vulkan_LIBRARIES "${VULKAN_SDK_PATH}/lib/macOS")
    endif()
  else()
    set(Vulkan_LIBRARIES "${VULKAN_SDK_PATH}/Lib")
  endif()
  set(Vulkan_FOUND TRUE)
else()
  find_package(Vulkan REQUIRED)
  if (TARGET Vulkan::Vulkan)
    get_target_property(_vk_interface_inc Vulkan::Vulkan INTERFACE_INCLUDE_DIRECTORIES)
    set(Vulkan_INCLUDE_DIRS ${_vk_interface_inc})
    set(Vulkan_LIBRARIES Vulkan::Vulkan)
    set(Vulkan_FOUND TRUE)
  endif()
endif()
if (NOT Vulkan_FOUND)
  message(FATAL_ERROR "Could not locate Vulkan SDK. Set VULKAN_SDK_PATH or install system Vulkan.")
else()
  message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIRS}")
endif()


# 2. Set GLFW_PATH in .env.cmake to target specific glfw
set(GLFW_LIB glfw)
if (DEFINED GLFW_PATH)
  message(STATUS "Using GLFW path specified in .env: ${GLFW_PATH}")
  set(GLFW_INCLUDE_DIRS "${GLFW_PATH}/include")
  if (MSVC)
    set(GLFW_EXTRA_LIB_DIR "${GLFW_PATH}/lib-vc2019")
  elseif (CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
    set(GLFW_EXTRA_LIB_DIR "${GLFW_PATH}/lib-mingw-w64")
  endif()
elseif (VE_FETCH_GLFW)
  include(FetchContent)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.9
  )
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(glfw)
  set(GLFW_INCLUDE_DIRS ${glfw_SOURCE_DIR}/include)
  set(GLFW_LIB glfw)
else()
  find_package(glfw3 3.3 QUIET)
  if (glfw3_FOUND)
    set(GLFW_INCLUDE_DIRS "") # target provided by package
    set(GLFW_LIB glfw)
    message(STATUS "Found system GLFW3")
  else()
    message(FATAL_ERROR "GLFW not found. Set GLFW_PATH, enable VE_FETCH_GLFW, or install glfw3.")
  endif()
endif()
if (GLFW_EXTRA_LIB_DIR)
  link_directories(${GLFW_EXTRA_LIB_DIR})
endif()

# Avoid global include_directories; use target_include_directories instead.

# If TINYOBJ_PATH not specified in .env.cmake, try fetching from git repo
if (NOT TINYOBJ_PATH)
  message(STATUS "TINYOBJ_PATH not specified in .env.cmake, using external/tinyobjloader")
  set(TINYOBJ_PATH external/tinyobjloader)
endif()

file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)

# Split main from engine sources for reuse
set(ENGINE_SOURCES)
set(MAIN_SOURCE)
foreach(src_file ${ALL_SOURCES})
  if (src_file MATCHES ".*/main.cpp$")
    set(MAIN_SOURCE ${src_file})
  else()
    list(APPEND ENGINE_SOURCES ${src_file})
  endif()
endforeach()

add_library(VEngineLib STATIC ${ENGINE_SOURCES})
add_library(VEngine::Lib ALIAS VEngineLib)
add_executable(${PROJECT_NAME} ${MAIN_SOURCE})
target_link_libraries(${PROJECT_NAME} PRIVATE VEngine::Lib)

# Add a custom target to run 'leaks' on the executable (macOS only)
if(VE_USE_LEAKS AND APPLE)
  add_custom_target(leaks
    COMMAND leaks --atExit -- ./VEngine
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${PROJECT_NAME}
    COMMENT "Run macOS leaks tool on VEngine executable"
  )
endif()

# Precompiled header (PCH) to speed up builds. This creates a compiled
# version of heavy headers (Vulkan-Hpp) reused by all translation units.
target_precompile_headers(VEngineLib PRIVATE src/pch.hpp)
target_precompile_headers(${PROJECT_NAME} REUSE_FROM VEngineLib)

# Warning flags (applied per-target)
if (MSVC)
  target_compile_options(VEngineLib PRIVATE /W4 $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:/WX>)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:/WX>)
else()
  foreach(tgt IN ITEMS VEngineLib ${PROJECT_NAME})
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wconversion -Wpedantic $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:-Werror>)
  endforeach()
endif()

set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/build")

if (WIN32)
  message(STATUS "CREATING BUILD FOR WINDOWS")
  if (USE_MINGW)
    target_include_directories(VEngineLib PUBLIC ${MINGW_PATH}/include)
    target_link_directories(VEngineLib PUBLIC ${MINGW_PATH}/lib)
  endif()
endif()

# Common include paths for engine (public so tests and exe inherit)
target_include_directories(VEngineLib
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${TINYOBJ_PATH}
    $<BUILD_INTERFACE:${GLFW_INCLUDE_DIRS}>
    $<BUILD_INTERFACE:${Vulkan_INCLUDE_DIRS}>
)

# Link dependencies
target_link_libraries(VEngineLib PUBLIC ${GLFW_LIB} ${Vulkan_LIBRARIES})

if (APPLE)
  # Link necessary macOS frameworks for GLFW/Vulkan surface integration
  target_link_libraries(VEngineLib PUBLIC
    "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework Metal"
  )
endif()


############## Build SHADERS #######################

# Generate SPIR-V with either Slang or glslangValidator
if (VE_BUILD_SHADERS)
  find_program(SLANGC_EXECUTABLE slangc HINTS $ENV{SLANG_HOME}/bin /usr/local/bin /usr/bin)
  if (NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR "slangc not found; install Slang or set VE_BUILD_SHADERS=OFF to skip shader compilation")
  endif()

  # Build shaders from the single Slang module
  add_slang_shader_target(Shaders
    SOURCES "${PROJECT_SOURCE_DIR}/shaders/simple_shader.slang"
    VERT_ENTRY vertMain
    FRAG_ENTRY fragMain
    PROFILE spirv_1_5
    OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
    OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/simple_shader.spv"
  )
  add_dependencies(VEngineLib Shaders)
  add_dependencies(${PROJECT_NAME} Shaders)
endif()

##################### Tests ################################
if (VE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
  )
  FetchContent_MakeAvailable(Catch2)

  file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*.cpp)
  if (TEST_SOURCES)
    add_executable(EngineTests ${TEST_SOURCES})
    target_link_libraries(EngineTests PRIVATE Catch2::Catch2WithMain VEngine::Lib)
    target_precompile_headers(EngineTests REUSE_FROM VEngineLib)
    if (NOT MSVC)
      target_compile_options(EngineTests PRIVATE -Wall -Wextra -Wconversion -Wpedantic $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:-Werror>)
    else()
      target_compile_options(EngineTests PRIVATE /W4 $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:/WX>)
    endif()
    add_test(NAME EngineTests COMMAND EngineTests)
    enable_testing()
  endif()
endif()