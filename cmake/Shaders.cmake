# Slang shader target helper
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

if (VE_BUILD_SHADERS)
  find_program(SLANGC_EXECUTABLE slangc HINTS $ENV{SLANG_HOME}/bin /usr/local/bin /usr/bin)
  if (NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR "slangc not found; install Slang or set VE_BUILD_SHADERS=OFF to skip shader compilation")
  endif()

  add_slang_shader_target(Shaders
    SOURCES "${PROJECT_SOURCE_DIR}/shaders/simple_shader.slang"
    VERT_ENTRY vertMain
    FRAG_ENTRY fragMain
    PROFILE spirv_1_5
    OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
    OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/simple_shader.spv"
  )
endif()
