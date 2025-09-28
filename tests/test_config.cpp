#include <catch2/catch_test_macros.hpp>
#include <ve_config.hpp>

TEST_CASE("ve_config basic constants and lists", "[config]") {
    // Frames in flight reasonable
    REQUIRE(ve::MAX_FRAMES_IN_FLIGHT >= 1);

    // Device extensions: expect at least swapchain and dynamic rendering
    REQUIRE_FALSE(ve::REQUIRED_DEVICE_EXTENSIONS.empty());

    bool hasSwapchain = false;
    bool hasDynamicRendering = false;
    for (auto* ext : ve::REQUIRED_DEVICE_EXTENSIONS) {
        if (std::string(ext) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) hasSwapchain = true;
        if (std::string(ext) == VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) hasDynamicRendering = true;
    }
    REQUIRE(hasSwapchain);
    REQUIRE(hasDynamicRendering);

    // Instance extensions list non-empty on portability setups
    REQUIRE_FALSE(ve::REQUIRED_INSTANCE_EXTENSIONS.empty());

    // Validation layers include Khronos validation layer
    bool hasValidation = false;
    for (auto* layer : ve::VALIDATION_LAYERS) {
        if (std::string(layer) == "VK_LAYER_KHRONOS_validation") hasValidation = true;
    }
    REQUIRE(hasValidation);
}
