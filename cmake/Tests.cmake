include(FetchContent)
FetchContent_Declare(
	Catch2
	GIT_REPOSITORY https://github.com/catchorg/Catch2.git
	GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*.cpp)
if (TEST_SOURCES)
	include(CTest)
	enable_testing()
	foreach(TEST_SRC ${TEST_SOURCES})
	get_filename_component(_test_name ${TEST_SRC} NAME_WE)
	set(_test_target ${_test_name}Tests)
	add_executable(${_test_target} ${TEST_SRC})
	target_link_libraries(${_test_target} PRIVATE Catch2::Catch2WithMain VEngine::Lib)
	target_precompile_headers(${_test_target} REUSE_FROM VEngineLib)
	if (NOT MSVC)
		target_compile_options(${_test_target} PRIVATE -Wall -Wextra -Wconversion -Wpedantic $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:-Werror>)
	else()
		target_compile_options(${_test_target} PRIVATE /W4 $<$<BOOL:${VE_WARNINGS_AS_ERRORS}>:/WX>)
	endif()
	add_test(NAME ${_test_target} COMMAND ${_test_target})
	endforeach()
endif()
