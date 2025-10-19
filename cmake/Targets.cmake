# Collect sources
file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)

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

# Common include paths for engine (public so tests and exe inherit)
target_include_directories(VEngineLib
	PUBLIC
		${PROJECT_SOURCE_DIR}/src
		${TINYOBJ_PATH}
		$<BUILD_INTERFACE:${GLFW_INCLUDE_DIRS}>
		$<BUILD_INTERFACE:${Vulkan_INCLUDE_DIRS}>
		$<BUILD_INTERFACE:${GLM_INCLUDE_DIRS}>
)

# Treat third-party headers as system includes to suppress their warnings
if (STB_PATH)
	target_include_directories(VEngineLib SYSTEM PUBLIC ${STB_PATH})
endif()

# Link dependencies
target_link_libraries(VEngineLib PUBLIC ${GLFW_LIB} ${Vulkan_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE VEngine::Lib)

# Add Dear ImGui sources to VEngineLib
if (IMGUI_DIR)
	set(IMGUI_SOURCES
		${IMGUI_DIR}/imgui.cpp
		${IMGUI_DIR}/imgui_draw.cpp
		${IMGUI_DIR}/imgui_tables.cpp
		${IMGUI_DIR}/imgui_widgets.cpp
		${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
		${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
	)
	target_sources(VEngineLib PRIVATE ${IMGUI_SOURCES})
	# Mark ImGui headers as system to silence warnings coming from third-party headers
	target_include_directories(VEngineLib SYSTEM PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)

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
target_precompile_headers(VEngineLib PRIVATE src/pch.hpp)
target_precompile_headers(${PROJECT_NAME} REUSE_FROM VEngineLib)

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
	if (USE_MINGW AND MINGW_PATH)
		target_include_directories(VEngineLib PUBLIC ${MINGW_PATH}/include)
		target_link_directories(VEngineLib PUBLIC ${MINGW_PATH}/lib)
	endif()
endif()
