#include <catch2/catch_test_macros.hpp>
#include <utils/ve_path.hpp>
#include <filesystem>
#include <iostream>

TEST_CASE("getProjectRoot finds valid directory", "[utils][path]") {
    // This test assumes it's running in an environment where the project root exists
    try {
        std::filesystem::path root = ve::getProjectRoot(nullptr);

        REQUIRE_FALSE(root.empty());
        REQUIRE(std::filesystem::exists(root));
        REQUIRE(std::filesystem::is_directory(root));

        REQUIRE(std::filesystem::exists(root / "models"));
        REQUIRE(std::filesystem::exists(root / "textures"));

    } catch (const std::exception& e) {
        FAIL("getProjectRoot threw exception: " << e.what());
    }
}

