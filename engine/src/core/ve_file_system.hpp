#pragma once
#include <filesystem>
#include <vector>

namespace ve {

class VeFileSystem {
	public:
		static std::vector<char> readFile(const std::filesystem::path& file_path);
};

}