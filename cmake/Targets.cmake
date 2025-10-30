# Collect sources
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/engine/src/*.cpp)
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/app/src/*.cpp)

add_library(VEngineLib SHARED ${ENGINE_SOURCES})
if (MSVC)
	target_compile_definitions(VEngineLib PRIVATE VENGINE_EXPORTS) # Define export macro for DLL declspec on Windows
	target_compile_options(VEngineLib PRIVATE /wd4251) # Suppress DLL interface warnings
endif()
add_library(VEngine::Lib ALIAS VEngineLib)

add_executable(${PROJECT_NAME} ${APP_SOURCES})
if (MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE /wd4251) # Suppress DLL interface warnings
endif()

# Common include paths for engine (public so tests and exe inherit)
target_include_directories(VEngineLib
	PUBLIC
		${PROJECT_SOURCE_DIR}/engine/src
		${PROJECT_SOURCE_DIR}/include
		$<BUILD_INTERFACE:${GLFW_INCLUDE_DIRS}>
		$<BUILD_INTERFACE:${Vulkan_INCLUDE_DIRS}>
		$<BUILD_INTERFACE:${GLM_INCLUDE_DIRS}>
)

if(TINYGLTF_PATH)
	target_include_directories(VEngineLib SYSTEM PUBLIC ${TINYGLTF_PATH})
endif()

# Add KTX include directories
if(KTX_INCLUDE_DIRS)
	target_include_directories(VEngineLib SYSTEM PUBLIC ${KTX_INCLUDE_DIRS})
endif()

# Link dependencies
target_link_libraries(VEngineLib PUBLIC ${GLFW_LIB} ${Vulkan_LIBRARIES})

# Link KTX library
if(TARGET ktx)
	target_link_libraries(VEngineLib PUBLIC ktx)
	message(STATUS "Linking with ktx library target")
elseif(KTX_LIBRARIES)
	target_link_libraries(VEngineLib PUBLIC ${KTX_LIBRARIES})
	message(STATUS "Linking with system KTX library: ${KTX_LIBRARIES}")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE VEngine::Lib)

# For MinGW, prevent exception symbol conflicts when linking against DLL
if(USE_MINGW OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
	target_link_options(${PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)
endif()

#Add Dear ImGui sources to VEngineLib
if (IMGUI_DIR)
	set(IMGUI_SOURCES
		${IMGUI_DIR}/imgui.cpp
		${IMGUI_DIR}/imgui_draw.cpp
		${IMGUI_DIR}/imgui_tables.cpp
		${IMGUI_DIR}/imgui_widgets.cpp
		${IMGUI_DIR}/imgui_demo.cpp
		${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
		${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
	)
	target_sources(VEngineLib PRIVATE ${IMGUI_SOURCES})
	# Mark ImGui headers as system to silence warnings coming from third-party headers
	target_include_directories(VEngineLib SYSTEM PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
	# Also add ImGui headers to app target so app code can use ImGui API
	target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)

	# Suppress noisy conversion/sign warnings for ImGui sources only
	if (MSVC)
		set(_IMGUI_MSVC_FLAGS /wd4244 /wd4267 /wd4365 /wd4389)
		set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "${_IMGUI_MSVC_FLAGS}")
	else()
		set(_IMGUI_FLAGS -Wno-conversion -Wno-sign-conversion)
		if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
			list(APPEND _IMGUI_FLAGS -Wno-implicit-int-conversion -Wno-implicit-int-float-conversion -Wno-shorten-64-to-32)
		endif()
		set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "${_IMGUI_FLAGS}")
 	endif()
endif()

if (APPLE)
	target_link_libraries(VEngineLib PUBLIC
		"-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework Metal"
	)
endif()

# PCH & warnings
target_precompile_headers(VEngineLib PRIVATE engine/src/pch.hpp)

if (MSVC)
	target_compile_options(VEngineLib PRIVATE /W4 $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:/WX>)
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:/WX>)
	# Add MSVC debug-specific flags (optional)
	target_compile_options(VEngineLib PRIVATE $<$<CONFIG:Debug>:/Od /RTC1>)
	target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:/Od /RTC1>)
else()
	foreach(tgt IN ITEMS VEngineLib ${PROJECT_NAME})
		target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wconversion -Wpedantic $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:-Werror>)
	endforeach()
endif()

set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/build")

# Set sensible output directories for libs and executables when building locally
if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
if (NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Ensure the executable can find the shared library at runtime during development
if (APPLE OR UNIX)
	# Use rpath relative to the binary so the executable finds the shared lib in ../bin
	set_target_properties(${PROJECT_NAME} PROPERTIES
		BUILD_RPATH "$ORIGIN"
		INSTALL_RPATH "$ORIGIN"
	)
	set_target_properties(VEngineLib PROPERTIES
		BUILD_RPATH "$ORIGIN"
		INSTALL_RPATH "$ORIGIN"
	)
elseif (WIN32)
	# Windows: Set DLL output directory and copy DLL to executable directory
	set_target_properties(VEngineLib PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
	)
	# Copy DLL to executable directory after build
	# For MSVC multi-config generators, we need to handle config-specific paths
	if(MSVC)
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:VEngineLib>
			$<TARGET_FILE_DIR:${PROJECT_NAME}>
			COMMENT "Copying VEngineLib DLL to executable directory"
		)
	else()
		add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:VEngineLib>
			$<TARGET_FILE_DIR:${PROJECT_NAME}>
		)
	endif()

	# Copy KTX DLL if it's a system library (common logic for both MSVC and MinGW)
	if(KTX_DLL_PATH)
		if(EXISTS "${KTX_DLL_PATH}" AND NOT TARGET ktx)
			add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${KTX_DLL_PATH}"
				$<TARGET_FILE_DIR:${PROJECT_NAME}>
				COMMENT "Copying KTX DLL to executable directory"
			)
			message(STATUS "Added KTX DLL copy command: ${KTX_DLL_PATH}")
		elseif(NOT EXISTS "${KTX_DLL_PATH}")
			message(WARNING "KTX_DLL_PATH set but file does not exist: ${KTX_DLL_PATH}")
		endif()
	else()
		message(STATUS "KTX_DLL_PATH not set, skipping DLL copy")
	endif()
endif()

if (WIN32)
	message(STATUS "CREATING BUILD FOR WINDOWS")
	if (USE_MINGW AND MINGW_PATH)
		target_include_directories(VEngineLib PUBLIC ${MINGW_PATH}/include)
		target_link_directories(VEngineLib PUBLIC ${MINGW_PATH}/lib)
	endif()
endif()

