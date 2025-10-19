# Locate and configure third-party dependencies
# - Vulkan SDK
# - GLFW (optionally via FetchContent)
# - TinyObjLoader
# - stb_image

# Auto-detect common MSYS2/MinGW prefixes on Windows (if not provided)
if (WIN32 AND NOT DEFINED MINGW_PATH)
	set(_VE_MINGW_CANDIDATES
		"C:/msys64/mingw64"
		"C:/msys64/ucrt64"
		"C:/msys64/clang64"
		"C:/mingw64"
	)
	foreach(_ve_prefix IN LISTS _VE_MINGW_CANDIDATES)
		if (EXISTS "${_ve_prefix}/include" AND EXISTS "${_ve_prefix}/lib")
			set(MINGW_PATH "${_ve_prefix}" CACHE PATH "Auto-detected MSYS2/MinGW prefix" FORCE)
			message(STATUS "Auto-detected MSYS2/MinGW prefix: ${MINGW_PATH}")
			break()
		endif()
	endforeach()
	unset(_ve_prefix)
	unset(_VE_MINGW_CANDIDATES)
endif()

# If we have a MinGW prefix and we're not using MSVC, add it to search paths
if (MINGW_PATH AND NOT MSVC)
	list(PREPEND CMAKE_PREFIX_PATH "${MINGW_PATH}")
	list(PREPEND CMAKE_INCLUDE_PATH "${MINGW_PATH}/include")
	list(PREPEND CMAKE_LIBRARY_PATH "${MINGW_PATH}/lib")
	message(STATUS "Added MinGW include/lib to CMake search paths")
endif()

# Vulkan SDK
if (DEFINED VULKAN_SDK_PATH)
	set(Vulkan_INCLUDE_DIRS "${VULKAN_SDK_PATH}/Include")
	if (APPLE)
		if (EXISTS "${VULKAN_SDK_PATH}/lib")
			set(Vulkan_LIBRARIES "${VULKAN_SDK_PATH}/lib")
		elseif (EXISTS "${VULKAN_SDK_PATH}/lib/macOS")
			set(Vulkan_LIBRARIES "${VULKAN_SDK_PATH}/lib/macOS")
		endif()
	else()
		set(Vulkan_LIBRARIES "${VULKAN_SDK_PATH}/Lib")
	endif()
	set(Vulkan_FOUND TRUE)
else() # Find Vulkan SDK from system
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

# GLFW
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
	# Try CMake config first
	find_package(glfw3 3.3 QUIET)
	if (glfw3_FOUND)
		set(GLFW_INCLUDE_DIRS "")
		set(GLFW_LIB glfw)
		message(STATUS "Found system GLFW3")
	else()
		# On MSYS2/MinGW, glfw3 typically installs headers/libs under MINGW_PATH
		if (MINGW_PATH AND NOT MSVC AND EXISTS "${MINGW_PATH}/include/GLFW/glfw3.h")
			set(GLFW_INCLUDE_DIRS "${MINGW_PATH}/include")
			# MSYS2 library name is usually glfw3 or glfw3dll; prefer static first
			set(GLFW_LIB glfw3)
			link_directories("${MINGW_PATH}/lib")
			message(STATUS "Using GLFW from MSYS2 at: ${MINGW_PATH}")
		else()
			message(FATAL_ERROR "GLFW not found. Set GLFW_PATH, enable VE_FETCH_GLFW, or install glfw3.")
		endif()
	endif()
endif()
if (GLFW_EXTRA_LIB_DIR)
	link_directories(${GLFW_EXTRA_LIB_DIR})
endif()

# GLM (header-only)
if (DEFINED GLM_PATH)
	message(STATUS "Using GLM path specified in .env: ${GLM_PATH}")
	set(GLM_INCLUDE_DIRS "${GLM_PATH}")
else()
	# Try to find a system-installed GLM (Homebrew, default Unix locations, MSYS2)
	set(_VE_GLM_HINTS
		$ENV{GLM_PATH}
		/opt/homebrew/include
		/usr/local/include
		/usr/include
		C:/msys64/mingw64/include
	)
	if (MINGW_PATH)
		list(PREPEND _VE_GLM_HINTS "${MINGW_PATH}/include")
	endif()
	find_path(GLM_INCLUDE_DIRS "glm/glm.hpp" HINTS ${_VE_GLM_HINTS})
	unset(_VE_GLM_HINTS)
	if (GLM_INCLUDE_DIRS)
		message(STATUS "Found GLM at: ${GLM_INCLUDE_DIRS}")
	elseif (VE_FETCH_GLM) # Not found, fetch via FetchContent
		include(FetchContent)
		FetchContent_Declare(glm
			GIT_REPOSITORY https://github.com/g-truc/glm.git
			GIT_TAG 1.0.1
		)
		FetchContent_MakeAvailable(glm)
		set(GLM_INCLUDE_DIRS ${glm_SOURCE_DIR})
	endif()
	if (GLM_INCLUDE_DIRS)
		message(STATUS "Using GLM include path: ${GLM_INCLUDE_DIRS}")
	else()
		message(WARNING "GLM not found. Set GLM_PATH, enable VE_FETCH_GLM, or install GLM so headers are discoverable.")
	endif()
endif()

# TinyObj
if (NOT TINYOBJ_PATH)
	message(STATUS "TINYOBJ_PATH not specified in .env.cmake, using external/tinyobjloader")
	set(TINYOBJ_PATH external/tinyobjloader)
endif()

# stb_image (header-only)
if (NOT STB_PATH)
	message(STATUS "STB_PATH not specified in .env.cmake, using external/stb")
	set(STB_PATH external/stb)
endif()

# Dear ImGui: prefer vendored source under external/imgui, otherwise fetch
set(_VE_IMGUI_VENDORED_DIR "${CMAKE_SOURCE_DIR}/external/imgui")
if (EXISTS "${_VE_IMGUI_VENDORED_DIR}/imgui.h")
	set(IMGUI_DIR "${_VE_IMGUI_VENDORED_DIR}")
	message(STATUS "Using vendored Dear ImGui at: ${IMGUI_DIR}")
else()
	include(FetchContent)
	FetchContent_Declare(imgui
		GIT_REPOSITORY https://github.com/ocornut/imgui.git
		GIT_TAG v1.90.9-docking
	)
	FetchContent_MakeAvailable(imgui)
	set(IMGUI_DIR ${imgui_SOURCE_DIR})
	message(STATUS "Fetched Dear ImGui from Git (docking): ${IMGUI_DIR}")
endif()

# Dear ImGui (fetch if not provided)
if (NOT IMGUI_DIR)
	message(STATUS "IMGUI_DIR not specified; fetching Dear ImGui via FetchContent")
	include(FetchContent)
	FetchContent_Declare(imgui
		GIT_REPOSITORY https://github.com/ocornut/imgui.git
		GIT_TAG v1.90.9-docking
	)
	FetchContent_MakeAvailable(imgui)
	# Expose IMGUI_DIR for target includes and sources
	set(IMGUI_DIR ${imgui_SOURCE_DIR} CACHE PATH "Path to Dear ImGui sources")
endif()
