#include "utils/ve_path.hpp"
#include "utils/ve_log.hpp"

#include <cassert>
#include <filesystem>
#include <stdexcept>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <vector>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <vector>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#endif

namespace ve {

std::filesystem::path getProjectRoot(char** argv) {
	// Get executable path using platform-specific methods
	std::filesystem::path exe_path;
	bool path_found = false;

#if defined(_WIN32) || defined(_WIN64)
	// Windows: Use GetModuleFileNameW (works for MSVC, MinGW, Clang)
	std::vector<wchar_t> buf(MAX_PATH);
	DWORD len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
	if (len > 0 && len < buf.size()) {
		exe_path = std::filesystem::path(std::wstring(buf.data(), len));
		path_found = true;
		VE_LOGD("Found executable path (Windows): " << exe_path);
	}
	else if (len >= buf.size()) {
		// Path too long, grow buffer and retry
		buf.resize(32768); // Windows max path length
		len = GetModuleFileNameW(NULL, buf.data(), static_cast<DWORD>(buf.size()));
		if (len > 0 && len < buf.size()) {
			exe_path = std::filesystem::path(std::wstring(buf.data(), len));
			path_found = true;
			VE_LOGD("Found executable path (Windows, extended): " << exe_path);
		}
		else {
			VE_LOGW("GetModuleFileNameW failed or path too long");
		}
	}
	else {
		VE_LOGW("GetModuleFileNameW failed with error");
	}
#elif defined(__APPLE__)
	// macOS: Use _NSGetExecutablePath
	uint32_t size = PATH_MAX;
	std::vector<char> buf(size);
	if (_NSGetExecutablePath(buf.data(), &size) == 0) {
		try {
			exe_path = std::filesystem::canonical(buf.data());
			path_found = true;
			VE_LOGD("Found executable path (macOS): " << exe_path);
		} catch (const std::filesystem::filesystem_error& e) {
			VE_LOGW("Failed to canonicalize executable path on macOS: " << e.what());
		}
	}
	else {
		// Buffer too small, size now contains required size
		buf.resize(size);
		if (_NSGetExecutablePath(buf.data(), &size) == 0) {
			try {
				exe_path = std::filesystem::canonical(buf.data());
				path_found = true;
				VE_LOGD("Found executable path (macOS, extended): " << exe_path);
			} catch (const std::filesystem::filesystem_error& e) {
				VE_LOGW("Failed to canonicalize executable path on macOS: " << e.what());
			}
		}
		else {
			VE_LOGW("_NSGetExecutablePath failed on macOS");
		}
	}
#elif defined(__linux__)
	// Linux: Use /proc/self/exe
	char buf[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (len != -1) {
		buf[len] = '\0';
		try {
			exe_path = std::filesystem::canonical(buf);
			path_found = true;
			VE_LOGD("Found executable path (Linux): " << exe_path);
		} catch (const std::filesystem::filesystem_error& e) {
			VE_LOGW("Failed to canonicalize executable path on Linux: " << e.what());
		}
	}
	else {
		VE_LOGW("Failed to read /proc/self/exe on Linux");
	}
#endif

	// Fallback to argv[0] if platform-specific method failed
	if (!path_found && argv && argv[0]) {
		VE_LOGD("Falling back to argv[0]: " << argv[0]);
		exe_path = argv[0];
		// Try to resolve relative paths
		if (!exe_path.is_absolute()) {
			exe_path = std::filesystem::current_path() / exe_path;
		}
		try {
			if (std::filesystem::exists(exe_path)) {
				exe_path = std::filesystem::canonical(exe_path);
				path_found = true;
				VE_LOGD("Resolved executable path from argv[0]: " << exe_path);
			}
		} catch (const std::filesystem::filesystem_error& e) {
			VE_LOGW("Failed to resolve argv[0] path: " << e.what());
		}
	}

	if (!path_found || !exe_path.has_parent_path()) {
		VE_LOGE("Failed to determine executable path");
		throw std::runtime_error("Failed to determine executable path");
	}

	// Walk up from executable directory looking for models/textures
	VE_LOGD("Searching for project root starting from: " << exe_path.parent_path());
	std::filesystem::path search = exe_path.parent_path();
	while (search.has_parent_path() && search != search.root_path()) {
		if (std::filesystem::exists(search / "models") &&
		    std::filesystem::exists(search / "textures")) {
			VE_LOGI("Found project root: " << search);
			return search;
		}
		search = search.parent_path();
	}

	// Fallback: check current working directory
	std::filesystem::path cwd = std::filesystem::current_path();
	VE_LOGD("Project root not found in exe path tree, checking CWD: " << cwd);
	if (std::filesystem::exists(cwd / "models") && std::filesystem::exists(cwd / "textures")) {
		VE_LOGI("Found project root (from CWD): " << cwd);
		return cwd;
	}

	// Failed to find project root
	VE_LOGE("Failed to find project root directory. Could not locate 'models' and 'textures' folders from executable path: " << exe_path);
	throw std::runtime_error(
		"Failed to find project root directory. "
		"Could not locate 'models' and 'textures' folders from executable path: " +
		exe_path.string()
	);
}

} // namespace ve

