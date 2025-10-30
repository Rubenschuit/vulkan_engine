#pragma once

#include <filesystem>

namespace ve {

/**
 * Gets the absolute path to the running executable on windows.
 * Should not be called on non-Windows platforms.
 *
 * @return The absolute path to the executable file on windows, else empty path
 */
std::filesystem::path getPathToRunningExeWindows();

/**
 * Finds the project root directory by walking up from the executable path
 * or current working directory, looking for models/textures folders.
 *
 * Not perfectly robust, but works for most cases.
 *
 * @param argv Array of command line arguments from main(). First element should
 * be the executable path from main().
 * @return The project root directory path
 */
std::filesystem::path getWorkingDirectory(char** argv);

} // namespace ve

