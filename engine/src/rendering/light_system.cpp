#include "pch.hpp"
#include "rendering/light_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

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

LightSystem::LightSystem(VeDevice& device,
									VeDescriptorPool& descriptor_pool,
									const vk::raii::DescriptorSetLayout& global_set_layout,
									ResourceHandle<VeTexture> particle_texture,
									vk::Format color_format,
									vk::SampleCountFlagBits sample_count,
									std::filesystem::path shader_path,
									EventBus& event_bus)
									: m_ve_device(device), m_shader_path(shader_path),
									  m_particle_handle(std::move(particle_texture)) {

	event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		recreatePipeline(e.offscreen_format, e.sample_count);
	});

	m_billboard_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();

	createPipelineLayout(global_set_layout, m_billboard_set_layout->getDescriptorSetLayout());
	createPipeline(color_format, sample_count);
	createBillboardDescriptorSet(descriptor_pool);
}

void LightSystem::createBillboardDescriptorSet(VeDescriptorPool& descriptor_pool) {
	auto image_info = m_particle_handle.get()->getDescriptorInfo();
	VeDescriptorWriter(*m_billboard_set_layout, descriptor_pool)
		.writeImage(0, &image_info)
		.build(m_billboard_descriptor_set);
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
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eGreaterOrEqual;
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;
	pipeline_config.rasterization_info.depthBiasConstantFactor = 0.0f;
	pipeline_config.rasterization_info.depthBiasClamp = 0.0f;
	pipeline_config.rasterization_info.depthBiasSlopeFactor = 1.0f;

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
	frame_info.cmd().bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 2> sets{*frame_info.global_descriptor_set, *m_billboard_descriptor_set};
	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		sets,
		{}
	);

	auto& registry = *frame_info.registry;
	for (auto [entity, pl, tc] : registry.view<PointLightComponent, TransformComponent>()) {
		SimplePushConstantData push{};
		glm::vec3 world_pos = glm::vec3(registry.getWorldTransform(entity)[3]);
		push.position = glm::vec4{world_pos, 1.0f};
		push.scale = tc.getScale().x;
		push.color = glm::vec4{pl.getColor(), pl.getIntensity()};
		push.billboard_type = 0;
		frame_info.cmd().pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.cmd().draw(6, 1, 0, 0);
	}

	// Celestial billboards for directional lights (sun/moon)
	glm::vec3 cam_pos = frame_info.camera_view.position;
	for (auto [entity, dl] : registry.view<DirectionalLightComponent>()) {
		glm::vec3 celestial_pos = cam_pos - glm::normalize(dl.getDirection()) * CELESTIAL_DISTANCE;

		SimplePushConstantData push{};
		push.position = glm::vec4{celestial_pos, 1.0f};
		push.color = glm::vec4{dl.getColor(), dl.getIntensity() * CELESTIAL_INTENSITY_BOOST};
		push.scale = CELESTIAL_SCALE;
		push.billboard_type = (dl.getCelestialType() == CelestialType::Sun) ? 2 : 1;
		frame_info.cmd().pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.cmd().draw(6, 1, 0, 0);
	}

	// Spot light billboards
	for (auto [entity, sl, tc] : registry.view<SpotLightComponent, TransformComponent>()) {
		SimplePushConstantData push{
			.position = glm::vec4{glm::vec3(registry.getWorldTransform(entity)[3]), 1.0f},
			.color = glm::vec4{sl.getColor(), sl.getIntensity()},
			.scale = tc.getScale().x,
			.billboard_type = 3,
		};
		frame_info.cmd().pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		frame_info.cmd().draw(6, 1, 0, 0);
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

		glm::vec3 world_pos = glm::vec3(registry.getWorldTransform(entity)[3]);
		ubo.point_lights[num_lights].position = glm::vec4{world_pos, pl.getRange()};
		ubo.point_lights[num_lights].color.x = color.x * intensity;
		ubo.point_lights[num_lights].color.y = color.y * intensity;
		ubo.point_lights[num_lights].color.z = color.z * intensity;
		ubo.point_lights[num_lights].color.w = intensity;

		if (pl.getCastsShadow() && num_shadow_lights < MAX_POINT_SHADOW_LIGHTS) {
			glm::vec3 light_pos = world_pos;
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

		glm::vec3 dir = glm::normalize(dl.getDirection());
		glm::vec3 dl_color = dl.getColor();
		float dl_intensity = dl.getIntensity();

		ubo.dir_lights[num_dir_lights].direction = glm::vec4{dir, 0.0f};
		ubo.dir_lights[num_dir_lights].color.x = dl_color.x * dl_intensity;
		ubo.dir_lights[num_dir_lights].color.y = dl_color.y * dl_intensity;
		ubo.dir_lights[num_dir_lights].color.z = dl_color.z * dl_intensity;
		ubo.dir_lights[num_dir_lights].color.w = dl_intensity;

		// CSM for the first shadow-casting directional light
		if (dl.getCastsShadow() && !csm_assigned) {
			const CameraView& cv = frame_info.camera_view;
			float shadow_near = std::max(cv.z_near, 0.5f); // CSM near floor: avoid wasting cascade 0 on tiny near plane
			float shadow_far = std::min(cv.z_far, DIR_SHADOW_MAX_DISTANCE);
			float tan_half_fov = std::tan(cv.fov_y_radians * 0.5f);

			glm::vec3 cam_fwd = cv.forward;
			glm::vec3 cam_right = cv.right;
			glm::vec3 cam_up = cv.up;

			// Compute cascade split distances
			float splits[NUM_CSM_CASCADES];
			computeCascadeSplits(shadow_near, shadow_far, CSM_SPLIT_LAMBDA,
			                     NUM_CSM_CASCADES, splits);

			// Up vector for light view (avoid gimbal lock)
			glm::vec3 up = std::abs(glm::dot(dir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.99f
				? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

			constexpr float z_margin = CSM_Z_MARGIN;

			for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
				float near_split = (cascade == 0) ? shadow_near : splits[cascade - 1];
				float far_split = splits[cascade];

				// Frustum slice corners
				float nh = near_split * tan_half_fov;
				float nw = nh * cv.aspect;
				float fh = far_split * tan_half_fov;
				float fw = fh * cv.aspect;
				glm::vec3 nc = cv.position + cam_fwd * near_split;
				glm::vec3 fc = cv.position + cam_fwd * far_split;

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

				// Round radius up to texel boundary for stable sizing (per-cascade resolution)
				uint32_t cascade_res_u = frame_info.csm_cascade_resolutions
					? frame_info.csm_cascade_resolutions[cascade]
					: CSM_CASCADE_RESOLUTIONS[cascade];
				float cascade_res = static_cast<float>(cascade_res_u);
				float texels_per_unit = cascade_res / (2.0f * radius);
				radius = std::ceil(radius * texels_per_unit) / texels_per_unit;

				// Cascade geometry for scroll tracking
				frame_info.csm_data.center[cascade] = center;
				frame_info.csm_data.radius[cascade] = radius;

				// Snap the center's component along the light direction to a coarse grid.
				// This keeps depth values stable between snap events, enabling shadow map
				// caching.
				float depth_range = 2.0f * radius + z_margin;
				float z_snap = depth_range * 0.5f;
				float center_along_light = glm::dot(center, dir);

				// Z-snap with hysteresis
				auto& zs = m_cascade_z_state[cascade];
				float naive_snap = std::floor(center_along_light / z_snap) * z_snap;
				float snapped_z;
				if (!zs.valid || zs.z_snap != z_snap) {
					snapped_z = naive_snap;
				} else {
					float drift = center_along_light - zs.snapped_z;
					if (drift > z_snap * 1.25f || drift < -z_snap * 0.25f)
						snapped_z = naive_snap;
					else
						snapped_z = zs.snapped_z;
				}
				bool z_snap_changed = (snapped_z != zs.snapped_z) || !zs.valid;
				zs.snapped_z = snapped_z;  // must be after z_snap_changed check
				zs.z_snap = z_snap;
				zs.valid = true;
				glm::vec3 light_right = glm::normalize(glm::cross(dir, up));
				glm::vec3 light_up = glm::cross(light_right, dir);
				glm::vec3 snapped_center = light_right * glm::dot(center, light_right)
					+ light_up * glm::dot(center, light_up)
					+ dir * snapped_z;

				glm::vec3 eye = snapped_center - dir * (radius + z_margin);
				if (!z_snap_changed) {
					float cached_depth = glm::dot(zs.cached_eye, dir);
					float cur_depth = glm::dot(eye, dir);
					eye += dir * (cached_depth - cur_depth);
				}
				zs.cached_eye = eye;
				glm::mat4 light_view = glm::lookAt(eye, eye + dir, up);

				// Widen depth range to cover the full cascade sphere at maximum
				// Z-snap drift (up to z_snap * 1.25 from hysteresis)
				float total_depth_range = depth_range + z_snap * 2.5f;
				glm::mat4 light_proj = glm::ortho(
					-radius, radius, -radius, radius,
					0.0f, total_depth_range);

				// Texel snapping: eliminates sub-texel jitter
				applyTexelSnapping(light_proj, light_view, cascade_res_u);

				// Atlas bias matrix: maps clip [-1,1] to this cascade's UV region in the atlas
				glm::mat4 atlas_bias = s_bias_matrix;
				if (frame_info.shadow_atlas_regions && frame_info.shadow_atlas_width > 0) {
					auto& r = frame_info.shadow_atlas_regions[cascade];
					float aw = static_cast<float>(frame_info.shadow_atlas_width);
					float ah = static_cast<float>(frame_info.shadow_atlas_height);
					float sx = static_cast<float>(r.resolution) / aw;
					float sy = static_cast<float>(r.resolution) / ah;
					float ox = static_cast<float>(r.x) / aw;
					float oy = static_cast<float>(r.y) / ah;
					// Scale from [0,1] to region UV, then offset
					atlas_bias = glm::mat4(
						sx * 0.5f, 0.0f,      0.0f, 0.0f,
						0.0f,      sy * 0.5f,  0.0f, 0.0f,
						0.0f,      0.0f,       1.0f, 0.0f,
						ox + sx * 0.5f, oy + sy * 0.5f, 0.0f, 1.0f
					);
				}

				ubo.csm_shadow_matrices[cascade] = atlas_bias * light_proj * light_view;
				ubo.csm_split_distances[static_cast<int>(cascade)] = far_split;

				// Store raw view/proj for shadow render pass
				frame_info.csm_data.light_view[cascade] = light_view;
				frame_info.csm_data.light_proj[cascade] = light_proj;
			}
			ubo.csm_cascade_count = NUM_CSM_CASCADES;
			ubo.csm_base_layer = 0;
			ubo.csm_shadow_map_size = static_cast<float>(frame_info.shadow_atlas_width);
			ubo.csm_dir_light_index = num_dir_lights;
			frame_info.csm_data.active_cascade_count = NUM_CSM_CASCADES;
			csm_assigned = true;
		}

		num_dir_lights++;
	}

	ubo.num_dir_lights = num_dir_lights;

	// Spot lights
	uint32_t num_spot_lights = 0;
	for (auto [entity, sl, tc] : registry.view<SpotLightComponent, TransformComponent>()) {
		assert(num_spot_lights < MAX_SPOT_LIGHTS && "Number of spot lights exceeds MAX_SPOT_LIGHTS");

		glm::vec3 color = sl.getColor();
		float intensity = sl.getIntensity();
		glm::vec3 world_pos = glm::vec3(registry.getWorldTransform(entity)[3]);
		glm::vec3 world_dir = glm::normalize(glm::mat3(registry.getWorldTransform(entity)) * sl.getDirection());

		ubo.spot_lights[num_spot_lights].position = glm::vec4{world_pos, sl.getEffectiveRange()};
		ubo.spot_lights[num_spot_lights].direction = glm::vec4{world_dir, std::cos(sl.getOuterConeAngle())};
		ubo.spot_lights[num_spot_lights].color.x = color.x * intensity;
		ubo.spot_lights[num_spot_lights].color.y = color.y * intensity;
		ubo.spot_lights[num_spot_lights].color.z = color.z * intensity;
		ubo.spot_lights[num_spot_lights].color.w = std::cos(sl.getInnerConeAngle());

		// Spot light shadow: single perspective projection along cone direction
		if (sl.getCastsShadow() && num_shadow_lights < MAX_SHADOW_LIGHTS) {
			float fov = sl.getOuterConeAngle() * 2.0f;
			fov = std::min(fov, glm::radians(170.0f));
			float near_plane = 0.1f;
			float far_plane = sl.getEffectiveRange() > 0.0f ? sl.getEffectiveRange() : 100.0f;

			glm::vec3 spot_up = std::abs(glm::dot(world_dir, glm::vec3(0, 1, 0))) > 0.99f
				? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			glm::mat4 light_view = glm::lookAt(world_pos, world_pos + world_dir, spot_up);
			glm::mat4 light_proj = glm::perspective(fov, 1.0f, near_plane, far_plane);

			ubo.shadow_lights[num_shadow_lights].light_view = light_view;
			ubo.shadow_lights[num_shadow_lights].light_proj = light_proj;
			// Atlas bias matrix for this shadow light's region
			glm::mat4 spot_atlas_bias = s_bias_matrix;
			if (frame_info.shadow_atlas_regions && frame_info.shadow_atlas_width > 0) {
				uint32_t slot = NUM_CSM_CASCADES + num_shadow_lights;
				auto& r = frame_info.shadow_atlas_regions[slot];
				float aw = static_cast<float>(frame_info.shadow_atlas_width);
				float ah = static_cast<float>(frame_info.shadow_atlas_height);
				float sx = static_cast<float>(r.resolution) / aw;
				float sy = static_cast<float>(r.resolution) / ah;
				float ox = static_cast<float>(r.x) / aw;
				float oy = static_cast<float>(r.y) / ah;
				spot_atlas_bias = glm::mat4(
					sx * 0.5f, 0.0f,      0.0f, 0.0f,
					0.0f,      sy * 0.5f,  0.0f, 0.0f,
					0.0f,      0.0f,       1.0f, 0.0f,
					ox + sx * 0.5f, oy + sy * 0.5f, 0.0f, 1.0f
				);
			}
			ubo.shadow_lights[num_shadow_lights].shadow_matrix = spot_atlas_bias * light_proj * light_view;
			ubo.shadow_lights[num_shadow_lights].light_index_padding = glm::vec4(
				static_cast<float>(MAX_LIGHTS + num_spot_lights), 2.0f,  // type 2 = spot
				static_cast<float>(NUM_CSM_CASCADES + num_shadow_lights),
				0.0f);
			// Atlas bounds for XY clamping
			if (frame_info.shadow_atlas_regions && frame_info.shadow_atlas_width > 0) {
				uint32_t slot = NUM_CSM_CASCADES + num_shadow_lights;
				auto& r = frame_info.shadow_atlas_regions[slot];
				float aw = static_cast<float>(frame_info.shadow_atlas_width);
				float ah = static_cast<float>(frame_info.shadow_atlas_height);
				ubo.shadow_lights[num_shadow_lights].atlas_bounds = glm::vec4(
					static_cast<float>(r.x) / aw, static_cast<float>(r.y) / ah,
					static_cast<float>(r.x + r.resolution) / aw,
					static_cast<float>(r.y + r.resolution) / ah);
			}
			num_shadow_lights++;
		}

		num_spot_lights++;
	}
	ubo.num_spot_lights = num_spot_lights;
	ubo.num_shadow_lights = num_shadow_lights;
}
} // namespace ve
