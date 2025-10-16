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
)

# Treat third-party headers as system includes to suppress their warnings
if (STB_PATH)
	target_include_directories(VEngineLib SYSTEM PUBLIC ${STB_PATH})
endif()

# Link dependencies
target_link_libraries(VEngineLib PUBLIC ${GLFW_LIB} ${Vulkan_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE VEngine::Lib)

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
	if (USE_MINGW)
		target_include_directories(VEngineLib PUBLIC ${MINGW_PATH}/include)
		target_link_directories(VEngineLib PUBLIC ${MINGW_PATH}/lib)
	endif()
endif()
