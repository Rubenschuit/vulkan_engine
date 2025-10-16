#[[
Unified Slang -> SPIR-V target helper.

Usage examples:
	add_slang_spirv_target(ShadersSimple TYPE GRAPHICS SOURCES file.slang VERT_ENTRY vertMain FRAG_ENTRY fragMain OUT_FILE path/to/output.spv)
	add_slang_spirv_target(ShadersParticles TYPE COMPUTE SOURCES file.slang ENTRY compMain OUT_FILE path/to/output.spv)
]]
function(add_slang_spirv_target TARGET)
	set(options "")
	set(oneValueArgs TYPE OUT_DIR OUT_FILE VERT_ENTRY FRAG_ENTRY ENTRY PROFILE)
	set(multiValueArgs SOURCES)
	cmake_parse_arguments(SLANG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

	if (NOT SLANG_SOURCES)
		message(FATAL_ERROR "add_slang_spirv_target: SOURCES not specified")
	endif()
	if (NOT SLANG_TYPE)
		message(FATAL_ERROR "add_slang_spirv_target: TYPE must be GRAPHICS or COMPUTE")
	endif()

	if (NOT SLANGC_EXECUTABLE)
		message(FATAL_ERROR "add_slang_spirv_target: SLANGC_EXECUTABLE not found")
	endif()

	string(TOUPPER ${SLANG_TYPE} _TYPE_UP)
	if (NOT _TYPE_UP STREQUAL "GRAPHICS" AND NOT _TYPE_UP STREQUAL "COMPUTE")
		message(FATAL_ERROR "add_slang_spirv_target: TYPE must be GRAPHICS or COMPUTE")
	endif()

	set(_OUT_DIR ${SLANG_OUT_DIR})
	if (NOT _OUT_DIR)
		set(_OUT_DIR ${PROJECT_SOURCE_DIR}/shaders)
	endif()

	set(_PROFILE ${SLANG_PROFILE})
	if (NOT _PROFILE)
		set(_PROFILE spirv_1_5)
	endif()

	# Defaults for entries
	if (_TYPE_UP STREQUAL "GRAPHICS")
		set(_VERT_ENTRY ${SLANG_VERT_ENTRY})
		if (NOT _VERT_ENTRY)
			set(_VERT_ENTRY vertMain)
		endif()
		set(_FRAG_ENTRY ${SLANG_FRAG_ENTRY})
		if (NOT _FRAG_ENTRY)
			set(_FRAG_ENTRY fragMain)
		endif()
	elseif(_TYPE_UP STREQUAL "COMPUTE")
		set(_ENTRY ${SLANG_ENTRY})
		if (NOT _ENTRY)
			set(_ENTRY compMain)
		endif()
	endif()

	add_custom_command(
		OUTPUT ${_OUT_DIR}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${_OUT_DIR}
		COMMENT "Creating shader output directory: ${_OUT_DIR}"
		VERBATIM
	)

	set(_SPV_FILES)
	foreach(SLANG_SRC ${SLANG_SOURCES})
		get_filename_component(_FN ${SLANG_SRC} NAME_WE)
		set(_OUT_FILE ${SLANG_OUT_FILE})
		if (NOT _OUT_FILE)
			set(_OUT_FILE ${_OUT_DIR}/${_FN}.spv)
		endif()

		if (_TYPE_UP STREQUAL "GRAPHICS")
			add_custom_command(
				OUTPUT ${_OUT_FILE}
				COMMAND ${SLANGC_EXECUTABLE} ${SLANG_SRC} -target spirv -profile ${_PROFILE} -entry ${_VERT_ENTRY} -stage vertex -entry ${_FRAG_ENTRY} -stage fragment -emit-spirv-directly -fvk-use-entrypoint-name -o ${_OUT_FILE}
				DEPENDS ${SLANG_SRC} ${_OUT_DIR}
				WORKING_DIRECTORY ${_OUT_DIR}
				COMMENT "Slang compiling ${_FN}.slang -> ${_FN}.spv (vert=${_VERT_ENTRY}, frag=${_FRAG_ENTRY})"
				VERBATIM
			)
		else()
			add_custom_command(
				OUTPUT ${_OUT_FILE}
				COMMAND ${SLANGC_EXECUTABLE} ${SLANG_SRC} -target spirv -profile ${_PROFILE} -entry ${_ENTRY} -stage compute -emit-spirv-directly -fvk-use-entrypoint-name -o ${_OUT_FILE}
				DEPENDS ${SLANG_SRC} ${_OUT_DIR}
				WORKING_DIRECTORY ${_OUT_DIR}
				COMMENT "Slang compiling ${_FN}.slang -> ${_FN}.spv (compute entry=${_ENTRY})"
				VERBATIM
			)
		endif()
		list(APPEND _SPV_FILES ${_OUT_FILE})
	endforeach()

	add_custom_target(${TARGET} DEPENDS ${_SPV_FILES})
endfunction()

if (VE_BUILD_SHADERS)
	find_program(SLANGC_EXECUTABLE slangc HINTS $ENV{SLANG_HOME}/bin /usr/local/bin /usr/bin)
	if (NOT SLANGC_EXECUTABLE)
		message(FATAL_ERROR "slangc not found; install Slang or set VE_BUILD_SHADERS=OFF to skip shader compilation")
	endif()

	add_slang_spirv_target(Shaders
		TYPE GRAPHICS
		SOURCES "${PROJECT_SOURCE_DIR}/shaders/simple_shader.slang"
		VERT_ENTRY vertMain
		FRAG_ENTRY fragMain
		PROFILE spirv_1_5
		OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
		OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/simple_shader.spv"
	)

	add_slang_spirv_target(ShadersAxes
		TYPE GRAPHICS
		SOURCES "${PROJECT_SOURCE_DIR}/shaders/axes_shader.slang"
		VERT_ENTRY vertMain
		FRAG_ENTRY fragMain
		PROFILE spirv_1_5
		OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
		OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/axes_shader.spv"
	)

	add_slang_spirv_target(ShadersPointLight
		TYPE GRAPHICS
		SOURCES "${PROJECT_SOURCE_DIR}/shaders/point_light_shader.slang"
		VERT_ENTRY vertMain
		FRAG_ENTRY fragMain
		PROFILE spirv_1_5
		OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
		OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/point_light_shader.spv"
	)

	add_slang_spirv_target(ShadersParticlesCompute
		TYPE COMPUTE
		SOURCES "${PROJECT_SOURCE_DIR}/shaders/particle_compute.slang"
		ENTRY compMain
		PROFILE spirv_1_5
		OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
		OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/particle_compute.spv"
	)

	# Graphics variant for particle billboards (vert/frag in same Slang file)
	add_slang_spirv_target(ShadersParticlesBillboard
		TYPE GRAPHICS
		SOURCES "${PROJECT_SOURCE_DIR}/shaders/particle_compute.slang"
		VERT_ENTRY vertMain
		FRAG_ENTRY fragMain
		PROFILE spirv_1_5
		OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
		OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/particle_billboard.spv"
	)
endif()
