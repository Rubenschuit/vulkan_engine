// Tests for pipeline configuration defaults.
// nothing of note here yet
#include <catch2/catch_test_macros.hpp>
#include <core/ve_pipeline.hpp>
#include <core/ve_device.hpp>
#include <core/ve_window.hpp>

TEST_CASE("defaultPipelineConfigInfo sets sane Vulkan defaults", "[pipeline][config]") {
    ve::PipelineConfigInfo cfg{};
	ve::VeDevice dummy_device{*(new ve::VeWindow(800, 600, "Dummy"))}; // Dummy device for testing
	ve::VePipeline::defaultPipelineConfigInfo(cfg, dummy_device);

    // Dynamic state: viewport & scissor expected
    REQUIRE(cfg.dynamic_state_enables.size() == 2);
    REQUIRE(cfg.dynamic_state_enables[0] == vk::DynamicState::eViewport);
    REQUIRE(cfg.dynamic_state_enables[1] == vk::DynamicState::eScissor);
    REQUIRE(cfg.dynamic_state_info.dynamicStateCount == cfg.dynamic_state_enables.size());
    REQUIRE(cfg.dynamic_state_info.pDynamicStates == cfg.dynamic_state_enables.data());

    // Input assembly
    REQUIRE(cfg.input_assembly_info.topology == vk::PrimitiveTopology::eTriangleList);
    REQUIRE(cfg.input_assembly_info.primitiveRestartEnable == VK_FALSE);

    // Rasterization
    REQUIRE(cfg.rasterization_info.polygonMode == vk::PolygonMode::eFill);
    REQUIRE(cfg.rasterization_info.cullMode == vk::CullModeFlagBits::eFront);
    REQUIRE(cfg.rasterization_info.frontFace == vk::FrontFace::eClockwise);
    REQUIRE(cfg.rasterization_info.lineWidth == 1.0f);

    // Multisample
    REQUIRE(cfg.multisample_info.rasterizationSamples == dummy_device.getSampleCount());
    REQUIRE(cfg.multisample_info.sampleShadingEnable == VK_FALSE);

    // Color blend attachment: blending disabled, write all components
    REQUIRE(cfg.color_blend_attachment.blendEnable == VK_TRUE);
    REQUIRE((cfg.color_blend_attachment.colorWriteMask & (
			vk::ColorComponentFlagBits::eR |
			vk::ColorComponentFlagBits::eG |
			vk::ColorComponentFlagBits::eB |
			vk::ColorComponentFlagBits::eA)) == (
        vk::ColorComponentFlagBits::eR |
        vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB |
        vk::ColorComponentFlagBits::eA));
    REQUIRE(cfg.color_blend_info.attachmentCount == 1);
    REQUIRE(cfg.color_blend_info.pAttachments == &cfg.color_blend_attachment);

    // Pipeline layout & render pass must be supplied by caller later
    REQUIRE(cfg.pipeline_layout == VK_NULL_HANDLE);

    // Color format left undefined until swapchain format chosen
    REQUIRE(cfg.color_format == vk::Format::eUndefined);
}

