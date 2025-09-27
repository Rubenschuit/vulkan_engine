// Tests for pipeline configuration defaults.
// nothing of note here yet
#include <catch2/catch_test_macros.hpp>
#include <ve_pipeline.hpp>

TEST_CASE("defaultPipelineConfigInfo sets sane Vulkan defaults", "[pipeline][config]") {
    ve::PipelineConfigInfo cfg{};
	ve::VePipeline::defaultPipelineConfigInfo(cfg);

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
    REQUIRE(cfg.rasterization_info.cullMode == vk::CullModeFlagBits::eNone);
    REQUIRE(cfg.rasterization_info.frontFace == vk::FrontFace::eClockwise);
    REQUIRE(cfg.rasterization_info.lineWidth == 1.0f);

    // Multisample
    REQUIRE(cfg.multisample_info.rasterizationSamples == vk::SampleCountFlagBits::e1);
    REQUIRE(cfg.multisample_info.sampleShadingEnable == VK_FALSE);

    // Color blend attachment: blending disabled, write all components
    REQUIRE(cfg.color_blend_attachment.blendEnable == VK_FALSE);
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
    REQUIRE(cfg.render_pass == VK_NULL_HANDLE);
    REQUIRE(cfg.subpass == 0);

    // Color format left undefined until swapchain format chosen
    REQUIRE(cfg.color_format == vk::Format::eUndefined);
}

