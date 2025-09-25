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

# Find all vertex and fragment sources within shaders directory
# taken from VBlancos vulkan tutorial
# https://github.com/vblanco20-1/vulkan-guide/blob/all-chapters/CMakeLists.txt
if (VE_BUILD_SHADERS)
  find_program(GLSL_VALIDATOR glslangValidator HINTS 
    ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} 
    /usr/bin 
    /usr/local/bin 
    ${VULKAN_SDK_PATH}/Bin
    ${VULKAN_SDK_PATH}/Bin32
    $ENV{VULKAN_SDK}/Bin/ 
    $ENV{VULKAN_SDK}/Bin32/
  )
  if (NOT GLSL_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found; disable VE_BUILD_SHADERS to skip shader compilation")
  endif()

# get all .vert and .frag files in shaders directory
  file(GLOB_RECURSE GLSL_SOURCE_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/shaders/*.frag"
    "${PROJECT_SOURCE_DIR}/shaders/*.vert"
  )

  set(SPIRV_BINARY_FILES "")
  foreach(GLSL ${GLSL_SOURCE_FILES})
    get_filename_component(FILE_NAME ${GLSL} NAME)
    set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/${FILE_NAME}.spv")
    add_custom_command(
      OUTPUT ${SPIRV}
      COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV}
      DEPENDS ${GLSL}
      COMMENT "Compiling shader ${FILE_NAME} -> ${FILE_NAME}.spv"
      VERBATIM)
    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
  endforeach()

  add_custom_target(Shaders DEPENDS ${SPIRV_BINARY_FILES})
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