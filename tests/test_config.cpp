#include <catch2/catch_test_macros.hpp>
#include "ve_config.hpp"
#include <glm/glm.hpp>

TEST_CASE("ve_config basic constants and lists", "[config]") {
    // Frames in flight reasonable
    REQUIRE(ve::MAX_FRAMES_IN_FLIGHT >= 1);

    // Device extensions: expect at least swapchain and dynamic rendering
    REQUIRE_FALSE(ve::REQUIRED_DEVICE_EXTENSIONS.empty());

    bool hasSwapchain = false;
    for (auto* ext : ve::REQUIRED_DEVICE_EXTENSIONS) {
        if (std::string(ext) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) hasSwapchain = true;
    }
    REQUIRE(hasSwapchain);

    // REQUIRED_INSTANCE_EXTENSIONS may be empty: GLFW-required extensions and
    // VK_KHR_portability_enumeration are added at runtime in VeDevice.

    // Validation layers include Khronos validation layer
    bool hasValidation = false;
    for (auto* layer : ve::VALIDATION_LAYERS) {
        if (std::string(layer) == "VK_LAYER_KHRONOS_validation") hasValidation = true;
    }
    REQUIRE(hasValidation);
}

TEST_CASE("ve_config lighting and shadow constants", "[config]") {
    REQUIRE(ve::MAX_LIGHTS >= 1);
    REQUIRE(ve::MAX_SHADOW_LIGHTS >= 1);

    REQUIRE(ve::CSM_CASCADE_RESOLUTIONS[0] >= 256);
    REQUIRE(ve::SHADOW_BIAS >= 0.0f);
    REQUIRE(ve::SHADOW_BIAS < 0.01f);

    auto ambient = ve::DEFAULT_AMBIENT_LIGHT_COLOR;
    REQUIRE(ambient.r >= 0.0f);
    REQUIRE(ambient.g >= 0.0f);
    REQUIRE(ambient.b >= 0.0f);
}

TEST_CASE("ve_config portability extensions on macOS", "[config]") {
#ifdef __APPLE__
    for (auto* ext : ve::REQUIRED_DEVICE_EXTENSIONS) {
        REQUIRE(std::string(ext) != VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif
}
