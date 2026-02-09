
#include <catch2/catch_test_macros.hpp>
#include <platform/ve_file_system.hpp>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("VeFileSystem::readFile reads file contents correctly", "[core][filesystem]") {
	namespace fs = std::filesystem;

	fs::path tmp_dir = fs::temp_directory_path();
	fs::path tmp_file = tmp_dir / "ve_test_readfile.txt";

	// Write test content
	const std::string content = "Hello, Vulkan Engine!\nLine 2\nBinary: \x00\x01\x02";
	{
		std::ofstream out(tmp_file, std::ios::binary);
		REQUIRE(out.is_open());
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
	}

	// Clean up on scope exit
	struct Cleanup {
		fs::path path;
		~Cleanup() { fs::remove(path); }
	} cleanup{tmp_file};

	auto data = ve::VeFileSystem::readFile(tmp_file);

	REQUIRE(data.size() == content.size());
	REQUIRE(std::string(data.begin(), data.end()) == content);
}

TEST_CASE("VeFileSystem::readFile throws for non-existent file", "[core][filesystem]") {
	REQUIRE_THROWS_AS(
		ve::VeFileSystem::readFile("/nonexistent/path/that/does/not/exist.txt"),
		std::runtime_error
	);
}
