#include "pch.hpp"
#include "core/ve_file_system.hpp"

namespace ve {

// Reads a binary file and returns its contents as a vector of chars
std::vector<char> VeFileSystem::readFile(const std::filesystem::path& file_path) {

	std::ifstream file(file_path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		throw std::runtime_error(std::string("failed to open file: ") + file_path.string());
	}
	VE_LOGI("Reading file: " << file_path.string());

	std::streampos end = file.tellg();
	file.seekg(0, std::ios::beg);
	size_t file_size = static_cast<size_t>(end - file.tellg());
	std::vector<char> buffer(file_size);

	file.seekg(0);
	file.read(buffer.data(), static_cast<std::streamsize>(file_size));
	file.close();
	return buffer;
}

}