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

	if (NOT SLANGC)
		message(FATAL_ERROR "add_slang_spirv_target: slangc not found (SLANGC unset)")
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

	# Ensure output directory exists at build time within shader compile rules (avoid declaring a directory as an OUTPUT)

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
				COMMAND ${CMAKE_COMMAND} -E make_directory "${_OUT_DIR}"
				COMMAND "${SLANGC}" "${SLANG_SRC}" -target spirv -profile ${_PROFILE} -fvk-use-gl-layout -entry ${_VERT_ENTRY} -stage vertex -entry ${_FRAG_ENTRY} -stage fragment -emit-spirv-directly -fvk-use-entrypoint-name -o "${_OUT_FILE}"
				DEPENDS "${SLANG_SRC}"
				COMMENT "Slang compiling ${_FN}.slang -> ${_FN}.spv (vert=${_VERT_ENTRY}, frag=${_FRAG_ENTRY})"
				VERBATIM
			)
		else()
			add_custom_command(
				OUTPUT ${_OUT_FILE}
				COMMAND ${CMAKE_COMMAND} -E make_directory "${_OUT_DIR}"
				COMMAND "${SLANGC}" "${SLANG_SRC}" -target spirv -profile ${_PROFILE} -fvk-use-gl-layout -entry ${_ENTRY} -stage compute -emit-spirv-directly -fvk-use-entrypoint-name -o "${_OUT_FILE}"
				DEPENDS "${SLANG_SRC}"
				COMMENT "Slang compiling ${_FN}.slang -> ${_FN}.spv (compute entry=${_ENTRY})"
				VERBATIM
			)
		endif()
		list(APPEND _SPV_FILES ${_OUT_FILE})
	endforeach()

	add_custom_target(${TARGET} DEPENDS ${_SPV_FILES})
endfunction()

# Build candidate hint paths for slangc across platforms
set(_SLANG_HINTS
	$ENV{SLANG_HOME}/bin
	${SLANG_HOME}/bin
	$ENV{SLANG_ROOT}/bin
	${SLANG_ROOT}/bin
	$ENV{SLANG_PATH}/bin
	${SLANG_PATH}/bin
	/usr/local/bin
	/usr/bin
	./bin
)

# Common Windows locations (Program Files, vcpkg, MSYS2/MinGW)
if (WIN32)
	list(APPEND _SLANG_HINTS
		$ENV{ProgramFiles}/Slang/bin
		$ENV{ProgramW6432}/Slang/bin
		$ENV{ProgramFiles}/Slang/bin/windows-x64/release
		$ENV{ProgramW6432}/Slang/bin/windows-x64/release
		${MINGW_PATH}/bin
		C:/msys64/mingw64/bin
		C:/msys64/usr/bin
		$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/tools/slang
		${VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/tools/slang
		$ENV{VCPKG_ROOT}/installed/x64-windows/tools/slang
		$ENV{VCPKG_ROOT}/installed/x64-windows-static/tools/slang
	)
endif()

unset(SLANGC CACHE)
find_program(SLANGC
	NAMES slangc
	HINTS ${_SLANG_HINTS}
)
if (NOT SLANGC)
	message(FATAL_ERROR "slangc not found. Slang is required. Set SLANG_HOME or ensure slangc is on PATH.")
else()
	message(STATUS "Found slangc: ${SLANGC}")
endif()

# Glob recurese all .slang files in shaders/ directory
file(GLOB_RECURSE SLANG_SHADER_FILES "${PROJECT_SOURCE_DIR}/shaders/*.slang")
# Create a target for each shader file and add target to list
foreach(SLANG_SHADER ${SLANG_SHADER_FILES})
	get_filename_component(SLANG_SHADER_NAME ${SLANG_SHADER} NAME_WE)
	add_slang_spirv_target(${SLANG_SHADER_NAME}
		TYPE GRAPHICS
		SOURCES ${SLANG_SHADER}
		VERT_ENTRY vertMain
		FRAG_ENTRY fragMain
		PROFILE spirv_1_5
		OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
		OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/${SLANG_SHADER_NAME}.spv"
	)
	if ( SLANG_SHADER_NAME MATCHES ".*_compute$")
		add_slang_spirv_target(${SLANG_SHADER_NAME}c
			TYPE COMPUTE
			SOURCES ${SLANG_SHADER}
			ENTRY compMain
			PROFILE spirv_1_5
			OUT_DIR "${PROJECT_SOURCE_DIR}/shaders"
			OUT_FILE "${PROJECT_SOURCE_DIR}/shaders/${SLANG_SHADER_NAME}c.spv"
		)
		list(APPEND SHADER_TARGETS ${SLANG_SHADER_NAME}c)
	endif()
	# Add to list of shader targets
	list(APPEND SHADER_TARGETS ${SLANG_SHADER_NAME})

endforeach()