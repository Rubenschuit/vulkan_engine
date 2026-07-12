#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include "ve_export.hpp"

namespace ve {

inline std::string pathToUtf8(const std::filesystem::path& p) {
	const std::u8string s = p.u8string();
	return std::string(s.begin(), s.end());
}

inline std::string pathToUtf8Generic(const std::filesystem::path& p) {
	const std::u8string s = p.generic_u8string();
	return std::string(s.begin(), s.end());
}

inline std::filesystem::path utf8ToPath(std::string_view s) {
	return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

/**
 * Finds the project root directory by determining the executable location
 * and walking up the directory tree looking for models/textures folders.
 *
 * Uses platform-specific methods for executable path detection:
 * - Windows (MSVC/MinGW/Clang): GetModuleFileNameW
 * - macOS: _NSGetExecutablePath
 * - Linux: /proc/self/exe
 * - Fallback: argv[0] (less reliable)
 *
 * This allows the application to find resource folders regardless of the
 * current working directory from which the executable was launched.
 *
 * @param argv Array of command line arguments from main(). Used as fallback
 * if platform-specific methods fail.
 * @return The project root directory path
 * @throws std::runtime_error if executable path cannot be determined or
 * if models/textures folders cannot be found
 */
VENGINE_API std::filesystem::path getProjectRoot(char** argv);

} // namespace ve