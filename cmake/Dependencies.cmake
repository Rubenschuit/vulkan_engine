# Locate and configure third-party dependencies
# - Vulkan SDK
# - GLFW (optionally via FetchContent)
# - tinygltf
# - KTX-Software
# - Dear ImGui
# - GLM

# Auto-detect common MSYS2/MinGW prefixes on Windows (if not provided)
if (WIN32 AND NOT DEFINED MINGW_PATH AND CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
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
	message(STATUS "Vulkan version: ${Vulkan_VERSION}")
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
	)
	if (CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
		list(APPEND _VE_GLM_HINTS
			"C:/msys64/mingw64/include"
			"C:/msys64/ucrt64/include"
			"C:/msys64/clang64/include"
		)
	endif()
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

# TinyGltf (header-only)
if (NOT TINYGLTF_PATH)
	message(STATUS "TINYGLTF_PATH not specified in .env.cmake, using external/tinygltf")
	set(TINYGLTF_PATH external/tinygltf)
endif()

# KTX (Khronos Texture)
if (KTX_PATH)
	# Use explicit path from .env.cmake
	message(STATUS "Using KTX from KTX_PATH: ${KTX_PATH}")
	set(KTX_INCLUDE_DIRS "${KTX_PATH}/include")
	find_library(KTX_LIBRARIES NAMES ktx HINTS ${KTX_PATH}/lib)
	if (NOT KTX_LIBRARIES)
		message(FATAL_ERROR "KTX_PATH set but library not found at ${KTX_PATH}/lib")
	endif()
	# On Windows, find the DLL
	if(WIN32)
		if(EXISTS "${KTX_PATH}/bin/ktx.dll")
			set(KTX_DLL_PATH "${KTX_PATH}/bin/ktx.dll" CACHE FILEPATH "Path to KTX DLL")
		elseif(EXISTS "${KTX_PATH}/lib/ktx.dll")
			set(KTX_DLL_PATH "${KTX_PATH}/lib/ktx.dll" CACHE FILEPATH "Path to KTX DLL")
		endif()
	endif()
	message(STATUS "KTX configured: ${KTX_LIBRARIES} | ${KTX_INCLUDE_DIRS}")
else()
	# Try to find KTX in system
	find_library(KTX_LIBRARIES NAMES ktx HINTS
		/usr/local/lib /opt/homebrew/lib
		"C:/Program Files/KTX-Software/lib"
		"C:/msys64/mingw64/lib"
		"C:/msys64/ucrt64/lib"
		"C:/msys64/clang64/lib"
		"C:/VulkanSDK/${VULKAN_SDK_VERSION}/Third-Party/Bin"
	)
	if (KTX_LIBRARIES)
		# Try to find include directory near the library
		get_filename_component(KTX_LIB_DIR ${KTX_LIBRARIES} DIRECTORY)
		find_path(KTX_INCLUDE_DIRS "ktx.h" PATHS
			${KTX_LIB_DIR}/../include
			${KTX_LIB_DIR}/../../include
			/usr/local/include
			/opt/homebrew/include
			"C:/Program Files/KTX-Software/include"
		)
		if (NOT KTX_INCLUDE_DIRS)
			if (UNIX OR APPLE)
				set(KTX_INCLUDE_DIRS "/usr/local/include")
			elseif (WIN32)
				set(KTX_INCLUDE_DIRS "${KTX_LIB_DIR}/../include")
			endif()
		endif()
		message(STATUS "Found KTX in system: ${KTX_LIBRARIES} | ${KTX_INCLUDE_DIRS}")

		# On Windows, find the DLL for runtime copying
		if(WIN32)
			get_filename_component(KTX_LIB_DIR ${KTX_LIBRARIES} DIRECTORY)
			if(EXISTS "${KTX_LIB_DIR}/ktx.dll")
				set(KTX_DLL_PATH "${KTX_LIB_DIR}/ktx.dll" CACHE FILEPATH "Path to KTX DLL")
			elseif(EXISTS "${KTX_LIB_DIR}/../bin/ktx.dll")
				get_filename_component(KTX_DLL_PATH "${KTX_LIB_DIR}/../bin/ktx.dll" ABSOLUTE)
				set(KTX_DLL_PATH "${KTX_DLL_PATH}" CACHE FILEPATH "Path to KTX DLL")
			else()
				message(WARNING "KTX DLL not found near ${KTX_LIB_DIR}")
			endif()
		endif()
	else()
		# Not found on system — fetch via FetchContent
		message(STATUS "KTX not found on system, fetching from GitHub...")
		include(FetchContent)
		FetchContent_Declare(ktx
			GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
			GIT_TAG v4.4.2
		)
		# Disable features we don't use to minimize build time
		set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
		set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
		set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
		set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
		set(KTX_FEATURE_LOADTEST_APPS "" CACHE STRING "" FORCE)
		set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
		FetchContent_MakeAvailable(ktx)
		message(STATUS "KTX fetched: ${ktx_SOURCE_DIR}")
	endif()
endif()

# Dear ImGui: vendored source under external/imgui
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/external/imgui")
if (NOT EXISTS "${IMGUI_DIR}/imgui.h")
	message(FATAL_ERROR "Dear ImGui not found at ${IMGUI_DIR}. Ensure external/imgui is populated.")
endif()
message(STATUS "Using vendored Dear ImGui at: ${IMGUI_DIR}")

# meshoptimizer (mesh optimization and LOD generation)
include(FetchContent)
FetchContent_Declare(meshoptimizer
	GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
	GIT_TAG v1.0
)
FetchContent_MakeAvailable(meshoptimizer)
message(STATUS "meshoptimizer: ${meshoptimizer_SOURCE_DIR}")
