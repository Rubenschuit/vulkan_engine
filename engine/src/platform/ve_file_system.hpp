#pragma once
#include "ve_export.hpp"
#include <filesystem>
#include <vector>

namespace ve {

class VENGINE_API VeFileSystem {
	public:
		static std::vector<char> readFile(const std::filesystem::path& file_path);
		
		static void revealInFileManager(const std::filesystem::path& path);
};

}
