#include "pch.hpp"
#include "rendering/pbr_render_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_scene.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

struct PbrPushConstantData {
	alignas(16) glm::mat4 transform;
	alignas(16) glm::mat3x4 normal_transform;
	alignas(4)  float has_texture;
	alignas(4)  float alpha_cutoff;
	alignas(4)  uint32_t material_flags;  // bits 0-1: alpha_mode, bit 2: double_sided, bit 3: flip_tex_coord_v, bit 4: use_spec_gloss_texture
};
static_assert(sizeof(PbrPushConstantData) <= 128, "Push constants must be 128 bytes for stable layout");

// Objects with any transmission (> 0) are sorted into the transparent pass for proper
// alpha compositing after the skybox. Among those, only objects above this threshold
// fully disable depth writes; low-transmission objects still write depth so they
// properly occlude geometry behind them. Must match the threshold in pbr_shader.slang.
static constexpr float HIGH_TRANSMISSION_THRESHOLD = 0.5f;

PbrRenderSystem::PbrRenderSystem(
	VeDevice& device,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(std::move(shader_path)), m_color_format(color_format), m_sample_count(sample_count) {

	createPipelineLayout(global_set_layout, material_set_layout, shadow_set_layout);
	createPipeline(m_color_format, m_sample_count);
}

PbrRenderSystem::~PbrRenderSystem() = default;

void PbrRenderSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& shadow_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(PbrPushConstantData)
	};

	vk::DescriptorSetLayout layouts[3] = {*global_set_layout, *material_set_layout, *shadow_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 3,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void PbrRenderSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eCullMode);
	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eDepthWriteEnable);
	pipeline_config.dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(pipeline_config.dynamic_state_enables.size());
	pipeline_config.dynamic_state_info.pDynamicStates = pipeline_config.dynamic_state_enables.data();
	pipeline_config.multisample_info.rasterizationSamples = sample_count;
	pipeline_config.color_format = color_format;
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptions();
	pipeline_config.input_assembly_info.topology = m_topology;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create pipeline");
}

void PbrRenderSystem::prepareFrame(VeFrameInfo& frame_info) const {
	m_opaque_drawables.clear();
	m_transparent_drawables.clear();
	const size_t visible_count = frame_info.visible_game_objects.size();
	m_opaque_drawables.reserve(std::max(visible_count, m_opaque_drawables.capacity()));
	m_transparent_drawables.reserve(std::max(visible_count, m_transparent_drawables.capacity()));

	const glm::vec3 camera_pos = frame_info.camera.getPosition();

	for (auto& [id, entry] : frame_info.visible_game_objects) {
		VeGameObject& obj = *entry.obj;
		MeshComponent* mesh = entry.mesh;
		if (!mesh || !obj.getComponent<TransformComponent>())
			continue;
		auto* mat = mesh->getMaterial();
		vk::raii::DescriptorSet& mat_set = mat->hasDescriptorSet()
			? mat->getDescriptorSet()
			: frame_info.material_descriptor_set;
		MaterialAlphaProps alpha_props = mat->getAlphaProps();
		float transmission = mat->getMaterialFactors().transmission_factor;
		bool use_transparent_pass = (alpha_props.alpha_mode == AlphaMode::BLEND) || (transmission > 0.0f);
		glm::vec3 obj_pos = obj.getTransform() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		float dist_sq = glm::dot(obj_pos - camera_pos, obj_pos - camera_pos);
		if (use_transparent_pass)
			m_transparent_drawables.emplace_back(*mat_set, entry.obj, mesh, dist_sq, alpha_props.alpha_mode);
		else
			m_opaque_drawables.emplace_back(*mat_set, entry.obj, mesh, dist_sq, alpha_props.alpha_mode);
	}

	std::sort(m_opaque_drawables.begin(), m_opaque_drawables.end(),
		[](const Drawable& a, const Drawable& b) {
			if (a.dist_sq != b.dist_sq)
				return a.dist_sq > b.dist_sq;
			// Same distance: OPAQUE before MASK (pillar before vine)
			if (a.alpha_mode != b.alpha_mode)
				return static_cast<uint32_t>(a.alpha_mode) < static_cast<uint32_t>(b.alpha_mode);
			// Same alpha mode: sort by material for batching.
			return a.material_set < b.material_set;
		});
	std::sort(m_transparent_drawables.begin(), m_transparent_drawables.end(),
		[](const Drawable& a, const Drawable& b) { return a.dist_sq > b.dist_sq; });
}

void PbrRenderSystem::renderOpaque(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	VkDescriptorSet bound_material_set = VK_NULL_HANDLE;
	for (const auto& d : m_opaque_drawables) {
		if (d.material_set != bound_material_set) {
			bound_material_set = d.material_set;
			frame_info.command_buffer.bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				*m_pipeline_layout,
				0,
				{*frame_info.global_descriptor_set, bound_material_set, *frame_info.shadow_descriptor_set},
				{}
			);
		}
		VeGameObject& obj = *d.obj;
		MeshComponent* mesh = d.mesh;
		auto* mat_ptr = mesh->getMaterial();
		MaterialAlphaProps alpha_props = mat_ptr ? mat_ptr->getAlphaProps() : MaterialAlphaProps{};
		PbrPushConstantData push{};
		const glm::mat3 nrm = obj.getNormalTransform();
		push.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		push.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		push.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		push.transform = obj.getTransform();
		push.has_texture = mesh->has_texture;
		push.alpha_cutoff = alpha_props.alpha_cutoff;
		push.material_flags = static_cast<uint32_t>(alpha_props.alpha_mode) | (alpha_props.double_sided ? 4u : 0u)
			| (mat_ptr && mat_ptr->getFlipTexCoordV() ? 8u : 0u)
			| (alpha_props.use_spec_gloss_texture ? 16u : 0u);
		if (alpha_props.double_sided)
			frame_info.command_buffer.setCullMode(vk::CullModeFlagBits::eNone);
		else
			frame_info.command_buffer.setCullMode(vk::CullModeFlagBits::eFront);
		frame_info.command_buffer.setDepthWriteEnable(VK_TRUE);
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(PbrPushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		mesh->getMesh()->bindVertexBuffer(frame_info.command_buffer);
		mesh->getMesh()->bindIndexBuffer(frame_info.command_buffer);
		mesh->getMesh()->drawIndexed(frame_info.command_buffer);
	}
}

void PbrRenderSystem::renderTransparent(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	VkDescriptorSet bound_material_set = VK_NULL_HANDLE;
	for (const auto& d : m_transparent_drawables) {
		if (d.material_set != bound_material_set) {
			bound_material_set = d.material_set;
			frame_info.command_buffer.bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,
				*m_pipeline_layout,
				0,
				{*frame_info.global_descriptor_set, bound_material_set, *frame_info.shadow_descriptor_set},
				{}
			);
		}
		VeGameObject& obj = *d.obj;
		MeshComponent* mesh = d.mesh;
		auto* mat_ptr = mesh->getMaterial();
		MaterialAlphaProps alpha_props = mat_ptr ? mat_ptr->getAlphaProps() : MaterialAlphaProps{};
		float transmission = mat_ptr ? mat_ptr->getMaterialFactors().transmission_factor : 0.0f;
		bool is_transparent = (alpha_props.alpha_mode == AlphaMode::BLEND) || (transmission > HIGH_TRANSMISSION_THRESHOLD);
		PbrPushConstantData push{};
		const glm::mat3 nrm = obj.getNormalTransform();
		push.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		push.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		push.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		push.transform = obj.getTransform();
		push.has_texture = mesh->has_texture;
		push.alpha_cutoff = alpha_props.alpha_cutoff;
		push.material_flags = static_cast<uint32_t>(alpha_props.alpha_mode) | (alpha_props.double_sided ? 4u : 0u)
			| (mat_ptr && mat_ptr->getFlipTexCoordV() ? 8u : 0u)
			| (alpha_props.use_spec_gloss_texture ? 16u : 0u);
		if (alpha_props.double_sided)
			frame_info.command_buffer.setCullMode(vk::CullModeFlagBits::eNone);
		else
			frame_info.command_buffer.setCullMode(vk::CullModeFlagBits::eFront);
		frame_info.command_buffer.setDepthWriteEnable(!is_transparent);
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(PbrPushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		mesh->getMesh()->bindVertexBuffer(frame_info.command_buffer);
		mesh->getMesh()->bindIndexBuffer(frame_info.command_buffer);
		mesh->getMesh()->drawIndexed(frame_info.command_buffer);
	}
}

void PbrRenderSystem::renderObjects(VeFrameInfo& frame_info) const {
	prepareFrame(frame_info);
	renderOpaque(frame_info);
	renderTransparent(frame_info);
}

} // namespace ve

