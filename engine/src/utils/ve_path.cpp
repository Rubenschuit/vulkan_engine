#include "utils/ve_path.hpp"

#include <cassert>
#include <filesystem>

#if _MSC_VER && !__INTEL_COMPILER
#include <windows.h>
#include <vector>
#endif

namespace ve {

std::filesystem::path getPathToRunningExeWindows() {
#if _MSC_VER && !__INTEL_COMPILER
    std::vector<wchar_t> buf(MAX_PATH);
    DWORD len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
	assert(len > 0 && "GetModuleFileNameW failed");
    if (len < buf.size()) {
        return std::filesystem::path(std::wstring(buf.data(), len));
    }
    // Path too long, grow buffer
    buf.resize(len + 1);
    len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
    return std::filesystem::path(std::wstring(buf.data(), len));
#else
    // For non-Windows platforms, this function should not be called
	VE_LOGE("getPathToRunningExe should not be called on non-Windows platforms");
    return std::filesystem::path();
#endif
}

std::filesystem::path getWorkingDirectory(char** argv) {
	// Get executable path
	std::filesystem::path exe_path = argv[0];
#if _MSC_VER && !__INTEL_COMPILER
	exe_path = getPathToRunningExe();
#else
	// Resolve relative paths to absolute
	if (!exe_path.is_absolute()) {
		exe_path = std::filesystem::current_path() / exe_path;
	}
	try {
		if (std::filesystem::exists(exe_path)) {
			exe_path = std::filesystem::canonical(exe_path);
		}
	} catch (...) {}
#endif

	// Walk up from executable directory looking for models/textures
	assert(exe_path.has_parent_path() && "Executable path has no parent path");
	std::filesystem::path search = exe_path.parent_path();
	while (search.has_parent_path() && search != search.root_path()) {
		if (std::filesystem::exists(search / "models") &&
		    std::filesystem::exists(search / "textures")) {
			return search;
		}
		search = search.parent_path();
	}

	// Fallback: check current directory
	std::filesystem::path cwd = std::filesystem::current_path();
	if (std::filesystem::exists(cwd / "models") && std::filesystem::exists(cwd / "textures")) {
		return cwd;
	}

	// Fail with assertion
	assert(false && "Failed to find project root (models/textures folders not found)");
	VE_LOGD("Found working directory: " << search);
	return search;
}

} // namespace ve

