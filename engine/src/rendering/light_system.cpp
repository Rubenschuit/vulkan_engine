#include "pch.hpp"
#include "rendering/light_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace ve {

//TODO put scale in pos.w
struct SimplePushConstantData {
	glm::vec4 position;
	glm::vec4 color;
	float scale;
	uint32_t padding[3];
};

LightSystem::LightSystem( VeDevice& device,
									const vk::raii::DescriptorSetLayout& global_set_layout,
									const vk::raii::DescriptorSetLayout& material_set_layout,
									vk::Format color_format,
									vk::SampleCountFlagBits sample_count,
									std::filesystem::path shader_path)
									: m_ve_device(device), m_shader_path(shader_path) {

	createPipelineLayout(global_set_layout, material_set_layout);
	createPipeline(color_format, sample_count);
}

LightSystem::~LightSystem() {
}

void LightSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0, // Used for indexing multiple push constant ranges
		.size = sizeof(SimplePushConstantData)
	};
	std::array<vk::DescriptorSetLayout, 2> set_layouts{*global_set_layout, *material_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void LightSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = sample_count;

	// set formats for dynamic rendering
	pipeline_config.color_format = color_format;
	pipeline_config.attribute_descriptions.clear();
	pipeline_config.binding_descriptions.clear();

	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	pipeline_config	.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLessOrEqual;
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;
	pipeline_config.rasterization_info.depthBiasConstantFactor = 0.0f;
	pipeline_config.rasterization_info.depthBiasClamp = 0.0f;
	pipeline_config.rasterization_info.depthBiasSlopeFactor = -1.0f;

	//enable additve blending
	pipeline_config.color_blend_attachment.blendEnable = VK_TRUE;
	pipeline_config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	pipeline_config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne; //
	pipeline_config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create pipeline");

}


// Renders billboard quads for each point light
void LightSystem::render(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 2> sets{*frame_info.global_descriptor_set, *frame_info.texture_descriptor_set};
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		sets,
		{}
	);

	auto& registry = *frame_info.registry;
	auto& pl_pool = registry.pointLights();
	for (uint32_t i = 0; i < pl_pool.size(); i++) {
		uint32_t entity_idx = pl_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity)) continue;
		auto* transform = registry.getComponent<TransformComponent>(entity);
		if (!transform) continue;
		PointLightComponent& pl = pl_pool.data()[i];
		SimplePushConstantData push{};
		push.position = glm::vec4{transform->getTranslation(), 1.0f};
		push.scale = transform->getScale().x;
		push.color = glm::vec4{pl.color, pl.intensity};
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.command_buffer.draw(6, 1, 0, 0);
	}
}

// Update UBO with all light data (point lights + directional lights + shadows)
void LightSystem::updateUniformBuffer(VeFrameInfo& frame_info, UniformBufferObject& ubo) {
	uint32_t num_lights = 0;
	uint32_t num_shadow_lights = 0;

	// Pre-multiply ambient light color by intensity
	ubo.ambient_light_color.x *= ubo.ambient_light_color.w;
	ubo.ambient_light_color.y *= ubo.ambient_light_color.w;
	ubo.ambient_light_color.z *= ubo.ambient_light_color.w;

	auto& registry = *frame_info.registry;
	auto& pl_pool = registry.pointLights();
	for (uint32_t i = 0; i < pl_pool.size(); i++) {
		uint32_t entity_idx = pl_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity))
			continue;
		auto* transform = registry.getComponent<TransformComponent>(entity);
		if (!transform)
			continue;
		assert(num_lights < MAX_LIGHTS && "Number of point lights exceeds MAX_LIGHTS");

		PointLightComponent& pl = pl_pool.data()[i];
		glm::vec3 color = pl.color;

		ubo.point_lights[num_lights].position = glm::vec4{transform->getTranslation(), pl.range};
		ubo.point_lights[num_lights].color.x = color.x * pl.intensity;
		ubo.point_lights[num_lights].color.y = color.y * pl.intensity;
		ubo.point_lights[num_lights].color.z = color.z * pl.intensity;
		ubo.point_lights[num_lights].color.w = pl.intensity;

		if (pl.casts_shadow && num_shadow_lights < MAX_SHADOW_LIGHTS) {
			glm::vec3 light_pos = transform->getTranslation();
			glm::vec3 scene_center = glm::vec3(0.0f, 0.0f, 0.0f);
			glm::vec3 view_up = glm::vec3(0.0f, 1.0f, 0.0f);
			glm::mat4 light_view = glm::lookAt(light_pos, scene_center, view_up);
			float near_plane = 1.0f;
			float far_plane = 400.0f;
			glm::mat4 light_proj = glm::perspective(glm::radians(100.0f), 1.0f, near_plane, far_plane);

			ubo.shadow_lights[num_shadow_lights].light_index_padding = glm::vec4(
				static_cast<float>(num_lights), 0.0f, 0.0f, 0.0f); // y=0 marks point light
			ubo.shadow_lights[num_shadow_lights].light_view = light_view;
			ubo.shadow_lights[num_shadow_lights].light_proj = light_proj;
			num_shadow_lights++;
		}

		num_lights++;
	}

	ubo.num_lights = num_lights;

	// Directional lights
	uint32_t num_dir_lights = 0;
	auto& dl_pool = registry.directionalLights();
	for (uint32_t i = 0; i < dl_pool.size(); i++) {
		uint32_t entity_idx = dl_pool.entityAt(i);
		Entity entity = registry.entityFromIndex(entity_idx);
		if (!registry.isActive(entity))
			continue;
		assert(num_dir_lights < MAX_DIR_LIGHTS && "Number of directional lights exceeds MAX_DIR_LIGHTS");

		DirectionalLightComponent& dl = dl_pool.data()[i];
		glm::vec3 dir = glm::normalize(dl.direction);

		ubo.dir_lights[num_dir_lights].direction = glm::vec4{dir, 0.0f};
		ubo.dir_lights[num_dir_lights].color.x = dl.color.x * dl.intensity;
		ubo.dir_lights[num_dir_lights].color.y = dl.color.y * dl.intensity;
		ubo.dir_lights[num_dir_lights].color.z = dl.color.z * dl.intensity;
		ubo.dir_lights[num_dir_lights].color.w = dl.intensity;

		if (dl.casts_shadow && num_shadow_lights < MAX_SHADOW_LIGHTS) {
			// Camera-frustum-fitted orthographic shadow projection
			const auto& cam = frame_info.camera;
			float shadow_far = std::min(cam.getFar(), DIR_SHADOW_MAX_DISTANCE);
			float tan_half_fov = std::tan(cam.getFovY() * 0.5f);
			float nh = cam.getNear() * tan_half_fov;
			float nw = nh * cam.getAspect();
			float fh = shadow_far * tan_half_fov;
			float fw = fh * cam.getAspect();

			glm::vec3 cam_fwd = cam.getForward();
			glm::vec3 cam_right = cam.getRight();
			glm::vec3 cam_up = cam.getUp();
			glm::vec3 nc = cam.getPosition() + cam_fwd * cam.getNear();
			glm::vec3 fc = cam.getPosition() + cam_fwd * shadow_far;

			// 8 frustum corners in world space
			glm::vec3 corners[8] = {
				nc - cam_up * nh - cam_right * nw, // near bottom-left
				nc - cam_up * nh + cam_right * nw, // near bottom-right
				nc + cam_up * nh + cam_right * nw, // near top-right
				nc + cam_up * nh - cam_right * nw, // near top-left
				fc - cam_up * fh - cam_right * fw, // far bottom-left
				fc - cam_up * fh + cam_right * fw, // far bottom-right
				fc + cam_up * fh + cam_right * fw, // far top-right
				fc + cam_up * fh - cam_right * fw, // far top-left
			};

			// Frustum center for light view target
			glm::vec3 center{0.0f};
			for (const auto& c : corners)
				center += c;
			center /= 8.0f;

			// Light view matrix
			glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.99f
				? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			glm::mat4 light_view = glm::lookAt(center - dir, center, up);

			// Transform corners to light space and compute AABB
			glm::vec3 ls_min{std::numeric_limits<float>::max()};
			glm::vec3 ls_max{std::numeric_limits<float>::lowest()};
			for (const auto& c : corners) {
				glm::vec3 ls = glm::vec3(light_view * glm::vec4(c, 1.0f));
				ls_min = glm::min(ls_min, ls);
				ls_max = glm::max(ls_max, ls);
			}

			// Extend near plane backward to catch shadow casters behind the camera frustum
			constexpr float z_margin = 150.0f;
			glm::mat4 light_proj = glm::ortho(
				ls_min.x, ls_max.x,
				ls_min.y, ls_max.y,
				-ls_max.z - z_margin, -ls_min.z);

			ubo.shadow_lights[num_shadow_lights].light_index_padding = glm::vec4(
				static_cast<float>(num_dir_lights), 1.0f, 0.0f, 0.0f); // y=1 marks directional
			ubo.shadow_lights[num_shadow_lights].light_view = light_view;
			ubo.shadow_lights[num_shadow_lights].light_proj = light_proj;
			num_shadow_lights++;
		}

		num_dir_lights++;
	}

	ubo.num_dir_lights = num_dir_lights;
	ubo.num_shadow_lights = num_shadow_lights;
}
} // namespace ve
