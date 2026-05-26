#include "pch.hpp"
#include "rendering/skinned_points_render_system.hpp"
#include "rendering/skinning_pre_pass.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace ve {

struct DebugPushConstant {
	glm::mat4 model;
	glm::vec4 color; // rgb = color, a = point pixel size
};

SkinnedPointsRenderSystem::SkinnedPointsRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path,
		EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)) {

	event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		recreatePipeline(e.offscreen_format, e.sample_count);
	});

	createPipelineLayout(global_set_layout);
	createPipeline(color_format, sample_count);
}

SkinnedPointsRenderSystem::~SkinnedPointsRenderSystem() = default;

void SkinnedPointsRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex,
		.offset = 0,
		.size = sizeof(DebugPushConstant),
	};
	vk::PipelineLayoutCreateInfo info{
		.setLayoutCount = 1,
		.pSetLayouts = &*global_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), info);
}

void SkinnedPointsRenderSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = sample_count;
	config.input_assembly_info.topology = vk::PrimitiveTopology::ePointList;
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.color_format = color_format;
	config.pipeline_layout = *m_pipeline_layout;
	config.binding_descriptions = {vk::VertexInputBindingDescription{
		.binding = 0, .stride = sizeof(glm::vec3), .inputRate = vk::VertexInputRate::eVertex,
	}};
	config.attribute_descriptions = {vk::VertexInputAttributeDescription{
		.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = 0,
	}};

	m_pipeline = std::make_unique<VePipeline>(m_ve_device, m_shader_path, config);
}

void SkinnedPointsRenderSystem::recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	m_pipeline.reset();
	createPipeline(color_format, sample_count);
}

void SkinnedPointsRenderSystem::render(VeFrameInfo& fi, const SkinningPrePass& prepass, const PbrMegaBuffer& mega_buffer) {
	if (!fi.registry || !mega_buffer.isValid())
		return;
	auto& cmd = fi.cmd();
	auto& registry = *fi.registry;

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 1> sets{*fi.global_descriptor_set};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout, 0, sets, {});

	vk::Buffer buffers[] = {mega_buffer.getMegaShadowVbo()->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	cmd.bindVertexBuffers(0, buffers, offsets);

	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>()) {
		VeMesh* mesh = mc.getMesh();
		if (!mesh || !mesh->hasSkinning())
			continue;
		uint32_t vo = prepass.getSkinnedVertexOffset(entity, fi.current_frame, mega_buffer);
		if (vo == SkinningPrePass::INVALID_OFFSET)
			continue;

		// Color from low-bit hash of entity id (yellow-ish bias for visibility on dark background).
		uint32_t id = entity.id();
		float r = 0.6f + 0.4f * static_cast<float>((id * 2654435761u) & 0xFF) / 255.0f;
		float g = 0.6f + 0.4f * static_cast<float>((id * 40503u) & 0xFF) / 255.0f;
		float b = 0.4f + 0.2f * static_cast<float>((id * 19349663u) & 0xFF) / 255.0f;

		DebugPushConstant pc{
			.model = registry.getWorldTransform(entity),
			.color = {r, g, b, m_point_size},
		};
		cmd.pushConstants<DebugPushConstant>(*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex, 0, pc);

		cmd.draw(mesh->getVertexCount(), 1, vo, 0);
	}
}

} // namespace ve