#pragma once

#include <filesystem>
#include "ve_export.hpp"

namespace ve {

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
VENGINE_API std::filesystem::path getWorkingDirectory(char** argv);

} // namespace ve