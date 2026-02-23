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

#include <cmath>

namespace ve {

// Bias matrix: converts NDC [-1,1]^2 to texture coordinates [0,1]^2
static const glm::mat4 s_bias_matrix(
	0.5f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.5f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	0.5f, 0.5f, 0.0f, 1.0f
);

// Practical split scheme: blend between logarithmic and uniform distribution
static void computeCascadeSplits(float near_z, float far_z, float lambda,
                                  uint32_t count, float* out_splits) {
	for (uint32_t i = 0; i < count; i++) {
		float p = static_cast<float>(i + 1) / static_cast<float>(count);
		float log_split = near_z * std::pow(far_z / near_z, p);
		float lin_split = near_z + (far_z - near_z) * p;
		out_splits[i] = lambda * log_split + (1.0f - lambda) * lin_split;
	}
}

// Snap ortho projection to texel grid to eliminate sub-texel jitter
static void applyTexelSnapping(glm::mat4& light_proj, const glm::mat4& light_view,
                                uint32_t resolution) {
	glm::mat4 shadow_matrix = light_proj * light_view;
	glm::vec4 shadow_origin = shadow_matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	shadow_origin *= static_cast<float>(resolution) / 2.0f;

	glm::vec2 rounded(std::round(shadow_origin.x), std::round(shadow_origin.y));
	glm::vec2 offset = rounded - glm::vec2(shadow_origin.x, shadow_origin.y);
	offset *= 2.0f / static_cast<float>(resolution);

	light_proj[3][0] += offset.x;
	light_proj[3][1] += offset.y;
}

//TODO put scale in pos.w
struct SimplePushConstantData {
	glm::vec4 position;
	glm::vec4 color;
	float scale;
	uint32_t billboard_type; // 0 = point light, 1 = moon, 2 = sun
	uint32_t padding[2];
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
	for (auto [entity, pl, tc] : registry.view<PointLightComponent, TransformComponent>()) {
		SimplePushConstantData push{};
		push.position = glm::vec4{tc.getTranslation(), 1.0f};
		push.scale = tc.getScale().x;
		push.color = glm::vec4{pl.getColor(), pl.getIntensity()};
		push.billboard_type = 0;
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.command_buffer.draw(6, 1, 0, 0);
	}

	// Celestial billboards for directional lights (sun/moon)
	glm::vec3 cam_pos = frame_info.camera.getPosition();
	for (auto [entity, dl] : registry.view<DirectionalLightComponent>()) {
		glm::vec3 celestial_pos = cam_pos - glm::normalize(dl.direction) * CELESTIAL_DISTANCE;

		SimplePushConstantData push{};
		push.position = glm::vec4{celestial_pos, 1.0f};
		push.color = glm::vec4{dl.color, dl.intensity * CELESTIAL_INTENSITY_BOOST};
		push.scale = CELESTIAL_SCALE;
		push.billboard_type = (dl.celestial_type == CelestialType::Sun) ? 2 : 1;
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
	for (auto [entity, pl, tc] : registry.view<PointLightComponent, TransformComponent>()) {
		assert(num_lights < MAX_LIGHTS && "Number of point lights exceeds MAX_LIGHTS");

		glm::vec3 color = pl.getColor();
		float intensity = pl.getIntensity();

		ubo.point_lights[num_lights].position = glm::vec4{tc.getTranslation(), pl.getRange()};
		ubo.point_lights[num_lights].color.x = color.x * intensity;
		ubo.point_lights[num_lights].color.y = color.y * intensity;
		ubo.point_lights[num_lights].color.z = color.z * intensity;
		ubo.point_lights[num_lights].color.w = intensity;

		if (pl.getCastsShadow() && num_shadow_lights < MAX_POINT_SHADOW_LIGHTS) {
			glm::vec3 light_pos = tc.getTranslation();
			glm::vec3 scene_center = glm::vec3(0.0f, 0.0f, 0.0f);
			glm::vec3 view_up = glm::vec3(0.0f, 1.0f, 0.0f);
			glm::mat4 light_view = glm::lookAt(light_pos, scene_center, view_up);
			float near_plane = 1.0f;
			float far_plane = 400.0f;
			glm::mat4 light_proj = glm::perspective(glm::radians(100.0f), 1.0f, near_plane, far_plane);

			ubo.shadow_lights[num_shadow_lights].light_index_padding = glm::vec4(
				static_cast<float>(num_lights), 0.0f,
				static_cast<float>(NUM_CSM_CASCADES + num_shadow_lights), // z = actual array layer
				0.0f);
			ubo.shadow_lights[num_shadow_lights].light_view = light_view;
			ubo.shadow_lights[num_shadow_lights].light_proj = light_proj;
			num_shadow_lights++;
		}

		num_lights++;
	}

	ubo.num_lights = num_lights;

	// Directional lights (first shadow-casting directional gets CSM)
	uint32_t num_dir_lights = 0;
	bool csm_assigned = false;
	for (auto [entity, dl] : registry.view<DirectionalLightComponent>()) {
		assert(num_dir_lights < MAX_DIR_LIGHTS && "Number of directional lights exceeds MAX_DIR_LIGHTS");

		glm::vec3 dir = glm::normalize(dl.direction);

		ubo.dir_lights[num_dir_lights].direction = glm::vec4{dir, 0.0f};
		ubo.dir_lights[num_dir_lights].color.x = dl.color.x * dl.intensity;
		ubo.dir_lights[num_dir_lights].color.y = dl.color.y * dl.intensity;
		ubo.dir_lights[num_dir_lights].color.z = dl.color.z * dl.intensity;
		ubo.dir_lights[num_dir_lights].color.w = dl.intensity;

		// CSM for the first shadow-casting directional light
		if (dl.casts_shadow && !csm_assigned) {
			const auto& cam = frame_info.camera;
			float shadow_near = std::max(cam.getNear(), 0.5f); // CSM near floor: avoid wasting cascade 0 on tiny near plane
			float shadow_far = std::min(cam.getFar(), DIR_SHADOW_MAX_DISTANCE);
			float tan_half_fov = std::tan(cam.getFovY() * 0.5f);

			glm::vec3 cam_fwd = cam.getForward();
			glm::vec3 cam_right = cam.getRight();
			glm::vec3 cam_up = cam.getUp();

			// Compute cascade split distances
			float splits[NUM_CSM_CASCADES];
			computeCascadeSplits(shadow_near, shadow_far, CSM_SPLIT_LAMBDA,
			                     NUM_CSM_CASCADES, splits);

			// Up vector for light view (avoid gimbal lock)
			glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.99f
				? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

			constexpr float z_margin = 150.0f;

			for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
				float near_split = (cascade == 0) ? shadow_near : splits[cascade - 1];
				float far_split = splits[cascade];

				// Frustum slice corners
				float nh = near_split * tan_half_fov;
				float nw = nh * cam.getAspect();
				float fh = far_split * tan_half_fov;
				float fw = fh * cam.getAspect();
				glm::vec3 nc = cam.getPosition() + cam_fwd * near_split;
				glm::vec3 fc = cam.getPosition() + cam_fwd * far_split;

				glm::vec3 corners[8] = {
					nc - cam_up * nh - cam_right * nw,
					nc - cam_up * nh + cam_right * nw,
					nc + cam_up * nh + cam_right * nw,
					nc + cam_up * nh - cam_right * nw,
					fc - cam_up * fh - cam_right * fw,
					fc - cam_up * fh + cam_right * fw,
					fc + cam_up * fh + cam_right * fw,
					fc + cam_up * fh - cam_right * fw,
				};

				// Bounding sphere for rotation-invariant ortho bounds
				glm::vec3 center{0.0f};
				for (const auto& c : corners) center += c;
				center /= 8.0f;

				float radius = 0.0f;
				for (const auto& c : corners)
					radius = std::max(radius, glm::length(c - center));

				// Round radius up to texel boundary for stable sizing
				float texels_per_unit = static_cast<float>(CSM_SHADOW_MAP_RESOLUTION) / (2.0f * radius);
				radius = std::ceil(radius * texels_per_unit) / texels_per_unit;

				// Light view: position light behind the sphere along light direction
				glm::mat4 light_view = glm::lookAt(center - dir * (radius + z_margin), center, up);

				// Ortho projection from sphere (rotation-invariant)
				glm::mat4 light_proj = glm::ortho(
					-radius, radius, -radius, radius,
					0.0f, 2.0f * radius + z_margin);

				// Texel snapping: eliminates sub-texel jitter → no more flickering
				applyTexelSnapping(light_proj, light_view, CSM_SHADOW_MAP_RESOLUTION);

				// Store in UBO (bias * proj * view for shader sampling)
				ubo.csm_shadow_matrices[cascade] = s_bias_matrix * light_proj * light_view;
				ubo.csm_split_distances[static_cast<int>(cascade)] = far_split;

				// Store raw view/proj for shadow render pass
				frame_info.csm_data.light_view[cascade] = light_view;
				frame_info.csm_data.light_proj[cascade] = light_proj;
			}
			ubo.csm_cascade_count = NUM_CSM_CASCADES;
			ubo.csm_base_layer = 0;
			ubo.csm_shadow_map_size = static_cast<float>(CSM_SHADOW_MAP_RESOLUTION);
			ubo.csm_dir_light_index = num_dir_lights;
			frame_info.csm_data.active_cascade_count = NUM_CSM_CASCADES;
			csm_assigned = true;
		}

		num_dir_lights++;
	}

	ubo.num_dir_lights = num_dir_lights;
	ubo.num_shadow_lights = num_shadow_lights;
}
} // namespace ve
