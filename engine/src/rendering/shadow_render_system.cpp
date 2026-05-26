#include "pch.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/culling/gpu_culling_system.hpp"
#include "rendering/culling/meshlet_culling_system.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/skinning_pre_pass.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_image.hpp"
#include "resources/ve_texture.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_frustum.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

struct ShadowPushConstantData {
	alignas(4) uint32_t instance_offset; // SV_InstanceID is 0-based in Slang
};
static_assert(sizeof(ShadowPushConstantData) == 4, "Shadow push constants must be 4 bytes");


ShadowRenderSystem::ShadowRenderSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	std::filesystem::path shader_path,
	EventBus& event_bus)
	: m_ve_device(device), m_descriptor_pool(descriptor_pool), m_shader_path(shader_path) {

	event_bus.subscribe<DepthBiasChangedEvent>([this](const DepthBiasChangedEvent&) {
		forceShadowRerender();
	});
	event_bus.subscribe<CullingBackendChangedEvent>([this](const CullingBackendChangedEvent&) {
		forceShadowRerender();
	});
	event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		subscribeToRegistry(*e.registry);
	});
	event_bus.subscribe<AssetLoadCompleteEvent>([this](const AssetLoadCompleteEvent&) {
		invalidateShadowDrawables();
	});
	event_bus.subscribe<ShadowAtlasResolutionChangedEvent>(
		[this](const ShadowAtlasResolutionChangedEvent& e) {
			resizeShadowAtlas(e.preset, e.pool);
		});

	m_shadow_ubos.resize(MAX_FRAMES_IN_FLIGHT);
	m_shadow_global_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_views.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_projs.resize(MAX_FRAMES_IN_FLIGHT);

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_ubos[i].resize(MAX_SHADOW_LAYERS);
		m_shadow_global_descriptor_sets[i].reserve(MAX_SHADOW_LAYERS);
	}

	computeAtlasLayout();
	createShadowResources();

	m_shadow_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.build();

	// eCompute needed so the shadow mask compute shader can read shadow maps
	m_shadow_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute)       // comparison sampler
		.addBinding(1, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute)   // shadow atlas (2D)
		.addBinding(2, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute)       // raw sampler (PCSS)
		.build();

	createShadowUBOs();
	createShadowInstanceBuffers();
	createShadowPassDescriptorSets(descriptor_pool);
	createShadowTextureDescriptorSets(descriptor_pool);
	createPipelineLayout(material_set_layout);
	createPipeline(m_shadow_depth_format);
}

ShadowRenderSystem::~ShadowRenderSystem() {
}

void ShadowRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& material_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex,
		.offset = 0,
		.size = sizeof(ShadowPushConstantData)
	};
	vk::DescriptorSetLayout layouts[2] = {*m_shadow_global_set_layout->getDescriptorSetLayout(), *material_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 2,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void ShadowRenderSystem::createPipeline(vk::Format depth_format) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);

	pipeline_config.color_format = vk::Format::eUndefined;
	pipeline_config.depth_format = depth_format;
	pipeline_config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsShadow();
	pipeline_config.binding_descriptions = VeMesh::Vertex::getShadowBindingDescriptions();

	pipeline_config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eFront;
	pipeline_config.rasterization_info.depthClampEnable = VK_TRUE;
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;

	pipeline_config.dynamic_state_enables.push_back(vk::DynamicState::eDepthBias);
	pipeline_config.dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(pipeline_config.dynamic_state_enables.size());
	pipeline_config.dynamic_state_info.pDynamicStates = pipeline_config.dynamic_state_enables.data();

	pipeline_config.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLess;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create shadow pipeline");
}


void ShadowRenderSystem::createShadowUBOs() {
	vk::DeviceSize buffer_size = sizeof(ShadowPassUBO);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		for (size_t layer = 0; layer < MAX_SHADOW_LAYERS; layer++) {
			m_shadow_ubos[frame][layer] = std::make_unique<VeBuffer>(
				m_ve_device,
				buffer_size,
				1,
				vk::BufferUsageFlagBits::eUniformBuffer,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
				m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
			);
			m_shadow_ubos[frame][layer]->map();
		}
	}
}

void ShadowRenderSystem::createShadowInstanceBuffers() {
	m_shadow_instance_buffers.clear();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_instance_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(InstanceData),
			m_shadow_instance_capacity,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		));
		m_shadow_instance_buffers[i]->map();
	}
}

void ShadowRenderSystem::createShadowPassDescriptorSets(
	VeDescriptorPool& descriptor_pool) {
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_shadow_global_descriptor_sets[frame].clear();

		auto instance_info = m_shadow_instance_buffers[frame]->getDescriptorInfo();

		for (size_t layer = 0; layer < MAX_SHADOW_LAYERS; layer++) {
			vk::DescriptorBufferInfo buffer_info{
				.buffer = m_shadow_ubos[frame][layer]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};

			vk::raii::DescriptorSet descriptor_set{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, descriptor_pool)
				.writeBuffer(0, &buffer_info)
				.writeBuffer(1, &instance_info)
				.build(descriptor_set);

			m_shadow_global_descriptor_sets[frame].push_back(std::move(descriptor_set));
		}
	}
}


void ShadowRenderSystem::computeAtlasLayout() {
	// Pack cascade 0 top-left, remaining cascades stacked right, lights fill remaining space
	uint32_t c0_res = m_csm_cascade_resolutions[0];
	m_atlas_regions[0] = {0, 0, c0_res};

	uint32_t col1_x = c0_res;
	uint32_t col1_width = 0;
	uint32_t stack_y = 0;
	for (uint32_t c = 1; c < NUM_CSM_CASCADES; c++) {
		uint32_t res = m_csm_cascade_resolutions[c];
		m_atlas_regions[c] = {col1_x, stack_y, res};
		col1_width = std::max(col1_width, res);
		stack_y += res;
	}

	constexpr uint32_t total_lights = MAX_POINT_SHADOW_LIGHTS + MAX_SPOT_SHADOW_LIGHTS;
	auto lightRes = [this](uint32_t i) -> uint32_t {
		return (i < MAX_POINT_SHADOW_LIGHTS) ? m_point_shadow_resolution : m_spot_shadow_resolution;
	};
	uint32_t max_light_res = std::max(m_point_shadow_resolution, m_spot_shadow_resolution);

	uint32_t placed = 0;
	if (col1_width >= max_light_res) {
		uint32_t per_row = col1_width / max_light_res;
		uint32_t y = stack_y;
		while (placed < total_lights && y + max_light_res <= c0_res) {
			for (uint32_t col = 0; col < per_row && placed < total_lights; col++, placed++)
				m_atlas_regions[NUM_CSM_CASCADES + placed] = {col1_x + col * max_light_res, y, lightRes(placed)};
			y += max_light_res;
		}
		stack_y = y;
	}

	uint32_t top_width = c0_res + col1_width;
	uint32_t top_height = std::max(c0_res, stack_y);

	uint32_t overflow_x = 0;
	uint32_t overflow_height = 0;
	for (; placed < total_lights; placed++) {
		uint32_t res = lightRes(placed);
		m_atlas_regions[NUM_CSM_CASCADES + placed] = {overflow_x, top_height, res};
		overflow_x += res;
		overflow_height = std::max(overflow_height, res);
	}

	m_atlas_width = std::max(top_width, overflow_x);
	m_atlas_height = top_height + overflow_height;

	VE_LOGI("Shadow atlas: " << m_atlas_width << "x" << m_atlas_height
		<< " (" << NUM_CSM_CASCADES << " CSM cascades, "
		<< MAX_POINT_SHADOW_LIGHTS << " point, " << MAX_SPOT_SHADOW_LIGHTS << " spot)");
	for (uint32_t i = 0; i < MAX_SHADOW_LAYERS; i++) {
		auto& r = m_atlas_regions[i];
		VE_LOGD("  Region " << i << ": (" << r.x << ", " << r.y << ") "
			<< r.resolution << "x" << r.resolution);
	}
}

void ShadowRenderSystem::createShadowResources() {
	VE_LOGD("Shadow system: Creating shadow atlas " << m_atlas_width << "x" << m_atlas_height);

	m_shadow_depth_format = m_ve_device.findSupportedFormat(
		{vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
		vk::ImageTiling::eOptimal,
		vk::FormatFeatureFlagBits::eDepthStencilAttachment | vk::FormatFeatureFlagBits::eSampledImage
	);

	// Transfer usage needed for CSM scroll copy operations
	m_shadow_atlas = std::make_unique<VeImage>(
		m_ve_device,
		m_atlas_width,
		m_atlas_height,
		vk::SampleCountFlagBits::e1,
		m_shadow_depth_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled
			| vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eDepth,
		false,  // not cubemap
		1
	);

	m_shadow_atlas->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		vk::AccessFlags2{},
		vk::AccessFlagBits2::eDepthStencilAttachmentRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);
	m_shadow_atlas->setDebugName("Shadow Atlas");

	for (uint32_t c = 0; c < NUM_CSM_CASCADES; c++) {
		uint32_t res = m_csm_cascade_resolutions[c];
		m_cascade_cache[c] = std::make_unique<VeImage>(
			m_ve_device, res, res,
			vk::SampleCountFlagBits::e1, m_shadow_depth_format,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst
				| vk::ImageUsageFlagBits::eDepthStencilAttachment,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eDepth, false, 1);
		m_cascade_cache[c]->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eTransferSrcOptimal,
			vk::AccessFlags2{},
			vk::AccessFlagBits2::eTransferRead,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eTransfer);
		m_cascade_cache[c]->setDebugName(("CSM Cascade Cache " + std::to_string(c)).c_str());
	}

	m_shadow_sampler = VeTexture::createDepthCompareSampler(m_ve_device);
	m_shadow_raw_sampler = VeTexture::createShadowRawSampler(m_ve_device);
}

void ShadowRenderSystem::createShadowTextureDescriptorSets(VeDescriptorPool& descriptor_pool) {
	m_shadow_descriptor_sets.clear();
	m_shadow_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);

	vk::DescriptorImageInfo cmp_sampler_info{
		.sampler = *m_shadow_sampler,
		.imageView = nullptr,
		.imageLayout = vk::ImageLayout::eUndefined
	};

	vk::DescriptorImageInfo image_info{
		.sampler = nullptr,
		.imageView = *m_shadow_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal
	};

	vk::DescriptorImageInfo raw_sampler_info{
		.sampler = *m_shadow_raw_sampler,
		.imageView = nullptr,
		.imageLayout = vk::ImageLayout::eUndefined
	};

	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_shadow_set_layout, descriptor_pool)
			.writeImage(0, &cmp_sampler_info)
			.writeImage(1, &image_info)
			.writeImage(2, &raw_sampler_info)
			.build(set);
		m_shadow_descriptor_sets.push_back(std::move(set));
	}
}

void ShadowRenderSystem::resizeShadowAtlas(ShadowResolutionPreset preset, VeDescriptorPool& descriptor_pool) {
	const auto& values = getShadowResolutionPreset(preset);
	bool unchanged =
		m_resolution_preset == preset
		&& m_csm_cascade_resolutions[0] == values.csm[0]
		&& m_csm_cascade_resolutions[1] == values.csm[1]
		&& m_csm_cascade_resolutions[2] == values.csm[2]
		&& m_point_shadow_resolution == values.point
		&& m_spot_shadow_resolution == values.spot;
	if (unchanged)
		return;

	m_resolution_preset = preset;
	for (uint32_t c = 0; c < NUM_CSM_CASCADES; c++)
		m_csm_cascade_resolutions[c] = values.csm[c];
	m_point_shadow_resolution = values.point;
	m_spot_shadow_resolution = values.spot;

	m_shadow_atlas.reset();
	for (auto& img : m_cascade_cache)
		img.reset();

	computeAtlasLayout();
	createShadowResources();
	createShadowTextureDescriptorSets(descriptor_pool);

	// Force a full re-render.
	for (auto& state : m_cascade_state) {
		state.valid = false;
		state.dirty = true;
		state.incremental = false;
	}
	m_force_full_rerender = true;
}

void ShadowRenderSystem::updateUniformBuffer(uint32_t frame_index, UniformBufferObject& ubo,
                                              const CsmCascadeData& csm_data) {
	uint32_t total_layers = csm_data.active_cascade_count + ubo.num_shadow_lights;
	if (total_layers == 0) {
		m_light_views[frame_index].clear();
		m_light_projs[frame_index].clear();
		return;
	}

	m_light_views[frame_index].resize(total_layers);
	m_light_projs[frame_index].resize(total_layers);

	auto makeAtlasBias = [this](uint32_t layer) -> glm::mat4 {
		auto& r = m_atlas_regions[layer];
		float aw = static_cast<float>(m_atlas_width);
		float ah = static_cast<float>(m_atlas_height);
		float sx = static_cast<float>(r.resolution) / aw;
		float sy = static_cast<float>(r.resolution) / ah;
		float ox = static_cast<float>(r.x) / aw;
		float oy = static_cast<float>(r.y) / ah;
		return glm::mat4(
			sx * 0.5f, 0.0f,      0.0f, 0.0f,
			0.0f,      sy * 0.5f,  0.0f, 0.0f,
			0.0f,      0.0f,       1.0f, 0.0f,
			ox + sx * 0.5f, oy + sy * 0.5f, 0.0f, 1.0f
		);
	};

	if (csm_data.active_cascade_count > 0) {
		for (uint32_t cascade = 0; cascade < csm_data.active_cascade_count; cascade++) {
			m_light_views[frame_index][cascade] = csm_data.light_view[cascade];
			m_light_projs[frame_index][cascade] = csm_data.light_proj[cascade];

			ShadowPassUBO cascade_ubo{};
			cascade_ubo.view = csm_data.light_view[cascade];
			cascade_ubo.proj = csm_data.light_proj[cascade];
			cascade_ubo.projection_view = cascade_ubo.proj * cascade_ubo.view;

			auto& state = m_cascade_state[cascade];
			state.texel_shift = {0, 0};
			state.incremental = false;
			state.current_radius = csm_data.radius[cascade];
			bool csm_scrolling_enabled = true; // toggle for debugging

			if (m_force_full_rerender || !state.valid) {
				state.dirty = true;
				/*
				if (m_force_full_rerender)
					VE_LOGD("CSM " << cascade << ": FULL (force_full_rerender)");
				else
					VE_LOGD("CSM " << cascade << ": FULL (first frame)");
				*/
			} else {
				// Texel shift from snapped projection matrices: prev and current VP
				// both map the origin to texel-snapped clip space.
				glm::mat4 prev_pv = state.prev_proj * state.prev_view;
				glm::mat4 cur_pv = cascade_ubo.proj * cascade_ubo.view;

				float half_res = static_cast<float>(m_csm_cascade_resolutions[cascade]) * 0.5f;
				glm::vec4 prev_origin = prev_pv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
				glm::vec4 cur_origin = cur_pv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

				glm::vec2 delta_texels = glm::vec2(cur_origin - prev_origin) * half_res;
				glm::ivec2 shift{
					static_cast<int>(std::round(delta_texels.x)),
					static_cast<int>(std::round(delta_texels.y))
				};

				// Bit-identical VP means cascade content is still valid.
				// shift==0 is not sufficient (sub-texel snapping can differ).
				bool matrices_identical =
					cascade_ubo.view == state.prev_view
					&& cascade_ubo.proj == state.prev_proj;

				if (matrices_identical) {
					state.dirty = false;
				} else if (csm_data.radius[cascade] != state.prev_radius) {
					// Ortho projection scale changed, can't scroll
					state.dirty = true;
				} else {
					// Detect depth-axis eye movement (Z-snap). When the eye jumps
					// along the light direction, all cached depths become invalid.
					// Threshold is half a texel in world units.
					glm::vec3 prev_eye = -glm::transpose(glm::mat3(state.prev_view))
						* glm::vec3(state.prev_view[3]);
					glm::vec3 cur_eye = -glm::transpose(glm::mat3(cascade_ubo.view))
						* glm::vec3(cascade_ubo.view[3]);
					glm::vec3 light_fwd = -glm::vec3(
						cascade_ubo.view[0][2], cascade_ubo.view[1][2],
						cascade_ubo.view[2][2]);
					float depth_shift = glm::dot(cur_eye - prev_eye, light_fwd);
					float texel_world = (2.0f * csm_data.radius[cascade])
						/ static_cast<float>(m_csm_cascade_resolutions[cascade]);
					float depth_threshold = texel_world * 0.5f;

					if (std::abs(depth_shift) > depth_threshold) {
						state.dirty = true;
						VE_LOGD("CSM " << cascade << ": FULL (Z-snap depth shift: "
							<< depth_shift << ")");
					} else if (std::abs(shift.x) > CSM_SCROLL_THRESHOLD
							|| std::abs(shift.y) > CSM_SCROLL_THRESHOLD) {
						state.dirty = true;
						VE_LOGD("CSM " << cascade << ": FULL (shift too large: "
							<< shift.x << "," << shift.y << ")");
					} else if (csm_scrolling_enabled) {
						state.dirty = true;
						state.incremental = true;
						state.texel_shift = shift;
						//VE_LOGD("CSM " << cascade << ": INCREMENTAL shift=("
						//	<< shift.x << "," << shift.y << ")");
					} else {
						state.dirty = true;
					}
				}
			}
			state.prev_radius = csm_data.radius[cascade];
			state.prev_view = cascade_ubo.view;
			state.prev_proj = cascade_ubo.proj;
			state.valid = true;

			m_shadow_ubos[frame_index][cascade]->writeToBuffer(&cascade_ubo);

			if (!m_csm_cascade_ubos.empty() && cascade < m_csm_cascade_ubos[frame_index].size())
				m_csm_cascade_ubos[frame_index][cascade]->writeToBuffer(&cascade_ubo);
		}
		m_force_full_rerender = false;
	}

	for (uint32_t shadow_idx = 0; shadow_idx < ubo.num_shadow_lights && shadow_idx < MAX_SHADOW_LIGHTS; shadow_idx++) {
		uint32_t layer = NUM_CSM_CASCADES + shadow_idx;
		m_light_views[frame_index][csm_data.active_cascade_count + shadow_idx] = ubo.shadow_lights[shadow_idx].light_view;
		m_light_projs[frame_index][csm_data.active_cascade_count + shadow_idx] = ubo.shadow_lights[shadow_idx].light_proj;

		ShadowPassUBO shadow_ubo{};
		shadow_ubo.view = ubo.shadow_lights[shadow_idx].light_view;
		shadow_ubo.proj = ubo.shadow_lights[shadow_idx].light_proj;
		shadow_ubo.projection_view = shadow_ubo.proj * shadow_ubo.view;

		ubo.shadow_lights[shadow_idx].shadow_matrix = makeAtlasBias(layer) * shadow_ubo.projection_view;

		auto& r = m_atlas_regions[layer];
		float aw = static_cast<float>(m_atlas_width);
		float ah = static_cast<float>(m_atlas_height);
		ubo.shadow_lights[shadow_idx].atlas_bounds = glm::vec4(
			static_cast<float>(r.x) / aw,
			static_cast<float>(r.y) / ah,
			static_cast<float>(r.x + r.resolution) / aw,
			static_cast<float>(r.y + r.resolution) / ah);

		m_shadow_ubos[frame_index][layer]->writeToBuffer(&shadow_ubo);
	}
}

void ShadowRenderSystem::invalidateShadowDrawables() {
	m_static_drawables_dirty = true;
	m_dynamic_drawables_dirty = true;
	m_force_full_rerender = true;
}

void ShadowRenderSystem::forceShadowRerender() {
	m_force_full_rerender = true;
}

void ShadowRenderSystem::retireBuffer(std::unique_ptr<VeBuffer> buffer) {
	if (buffer)
		m_retired_buffers.push_back({std::move(buffer), MAX_FRAMES_IN_FLIGHT});
}

void ShadowRenderSystem::flushRetiredBuffers() {
	for (auto it = m_retired_buffers.begin(); it != m_retired_buffers.end();) {
		if (--it->frames_remaining == 0)
			it = m_retired_buffers.erase(it);
		else
			++it;
	}
}

void ShadowRenderSystem::transitionAtlasPostRender(vk::raii::CommandBuffer& cmd) {
	vk::ImageMemoryBarrier2 barrier{
		.sType = vk::StructureType::eImageMemoryBarrier2,
		.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
		.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader
			| vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_shadow_atlas->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
	};
	cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

void ShadowRenderSystem::transitionAtlasForRendering(
	vk::raii::CommandBuffer& cmd, uint32_t csm_count) {

	bool any_incremental = false;
	for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES; c++)
		if (m_cascade_state[c].dirty && m_cascade_state[c].incremental)
			any_incremental = true;

	if (any_incremental) {
		// Copy from the existing static cache (populated by
		// the previous frame's snapshot) directly to the atlas. The cache is already in
		// eTransferSrcOptimal and contains static-only content
		vk::ImageMemoryBarrier2 atlas_to_dst{
			.sType = vk::StructureType::eImageMemoryBarrier2,
			.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader
				| vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eTransferDstOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_shadow_atlas->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
		};
		cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_to_dst});

		for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES; c++)
			if (m_cascade_state[c].dirty && m_cascade_state[c].incremental)
				copyPreservedRegion(cmd, c);

		vk::ImageMemoryBarrier2 to_depth{
			.sType = vk::StructureType::eImageMemoryBarrier2,
			.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
			.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
				| vk::PipelineStageFlagBits2::eLateFragmentTests,
			.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
				| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
			.oldLayout = vk::ImageLayout::eTransferDstOptimal,
			.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_shadow_atlas->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
		};
		cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_depth});
	} else {
		vk::ImageMemoryBarrier2 pre_barrier{
			.sType = vk::StructureType::eImageMemoryBarrier2,
			.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader
				| vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
				| vk::PipelineStageFlagBits2::eLateFragmentTests,
			.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
				| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
			.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = m_shadow_atlas->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
		};
		cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &pre_barrier});
	}
}

void ShadowRenderSystem::beginShadowRegionRender(
	vk::raii::CommandBuffer& cmd,
	const FrameAtlasRegion& region,
	vk::AttachmentLoadOp load_op,
	const StripRegion* strip_clear) {

	vk::Extent2D extent{region.resolution, region.resolution};
	vk::Rect2D full_area{{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent};

	vk::RenderingAttachmentInfo depth_attachment{
		.imageView = *m_shadow_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.loadOp = load_op,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
	};
	vk::RenderingInfo rendering_info{
		.renderArea = full_area,
		.layerCount = 1,
		.colorAttachmentCount = 0,
		.pColorAttachments = nullptr,
		.pDepthAttachment = &depth_attachment
	};
	cmd.beginRendering(rendering_info);

	cmd.setViewport(0, vk::Viewport{
		static_cast<float>(region.x), static_cast<float>(region.y),
		static_cast<float>(region.resolution), static_cast<float>(region.resolution),
		0.0f, 1.0f});

	if (strip_clear) {
		vk::ClearAttachment clear_depth{
			.aspectMask = vk::ImageAspectFlagBits::eDepth,
			.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}}
		};
		vk::ClearRect clear_rect{
			.rect = {{static_cast<int32_t>(strip_clear->x), static_cast<int32_t>(strip_clear->y)},
			         {strip_clear->width, strip_clear->height}},
			.baseArrayLayer = 0, .layerCount = 1
		};
		cmd.clearAttachments(clear_depth, clear_rect);
		cmd.setScissor(0, vk::Rect2D{
			{static_cast<int32_t>(strip_clear->x), static_cast<int32_t>(strip_clear->y)},
			{strip_clear->width, strip_clear->height}});
	} else {
		cmd.setScissor(0, full_area);
	}
}

void ShadowRenderSystem::subscribeToRegistry(Registry& registry) {
	m_registry = &registry;
	invalidateShadowDrawables();

	registry.events().subscribe<ComponentAddedEvent<MeshComponent>>(
		[this](const ComponentAddedEvent<MeshComponent>& event) {
			if (m_registry && isDynamicEntity(*m_registry, event.entity))
				m_dynamic_drawables_dirty = true;
			else {
				m_static_drawables_dirty = true;
				m_force_full_rerender = true;
			}
		});
	registry.events().subscribe<ComponentRemovedEvent<MeshComponent>>(
		[this](const ComponentRemovedEvent<MeshComponent>& event) {
			if (m_registry && isDynamicEntity(*m_registry, event.entity))
				m_dynamic_drawables_dirty = true;
			else {
				m_static_drawables_dirty = true;
				m_force_full_rerender = true;
			}
		});
	registry.events().subscribe<ComponentAddedEvent<RigidbodyComponent>>(
		[this](const ComponentAddedEvent<RigidbodyComponent>& event) {
			if (event.component.getMotionType() == PhysicsMotionType::Dynamic) {
				// Entity moved from static to dynamic
				m_static_drawables_dirty = true;
				m_dynamic_drawables_dirty = true;
				m_force_full_rerender = true;
			}
		});
	registry.events().subscribe<ComponentRemovedEvent<RigidbodyComponent>>(
		[this](const ComponentRemovedEvent<RigidbodyComponent>&) {
			// Entity becomes static
			m_static_drawables_dirty = true;
			m_dynamic_drawables_dirty = true;
			m_force_full_rerender = true;
		});
	registry.events().subscribe<AnimationStateChangedEvent>(
		[this](const AnimationStateChangedEvent&) {
			m_static_drawables_dirty = true;
			m_dynamic_drawables_dirty = true;
			m_force_full_rerender = true;
		});
	registry.events().subscribe<ComponentRemovedEvent<AnimatorComponent>>(
		[this](const ComponentRemovedEvent<AnimatorComponent>&) {
			m_static_drawables_dirty = true;
			m_dynamic_drawables_dirty = true;
			m_force_full_rerender = true;
		});
	registry.events().subscribe<EntityDestroyedEvent>(
		[this](const EntityDestroyedEvent&) {
			m_static_drawables_dirty = true;
			m_dynamic_drawables_dirty = true;
		});
	registry.events().subscribe<MeshDataChangedEvent>(
		[this](const MeshDataChangedEvent&) {
			invalidateShadowDrawables();
		});
	registry.events().subscribe<TransformInvalidatedEvent>(
		[this](const TransformInvalidatedEvent& event) {
			// Static shadow-casting object moved in editor: force static cache rerender
			if (!m_registry || isDynamicEntity(*m_registry, event.entity))
				return;
			const auto* mesh = m_registry->getComponent<MeshComponent>(event.entity);
			if (mesh && mesh->has_shadow)
				m_force_full_rerender = true;
		});
	registry.events().subscribe<RigidbodyChangedEvent>(
		[this](const RigidbodyChangedEvent&) {
			m_static_drawables_dirty = true;
			m_dynamic_drawables_dirty = true;
			m_force_full_rerender = true;
		});
	registry.events().subscribe<LightDataChangedEvent>(
		[this](const LightDataChangedEvent&) {
			m_force_full_rerender = true;
		});
}

std::vector<ShadowRenderSystem::StripRegion> ShadowRenderSystem::computeStripRegions(uint32_t cascade) const {
	std::vector<StripRegion> strips;
	auto& state = m_cascade_state[cascade];
	auto& region = m_atlas_regions[cascade];
	uint32_t N = region.resolution;
	int32_t dx = state.texel_shift.x;
	int32_t dy = state.texel_shift.y;
	// Expand strips by a small margin to eliminate depth precision artifacts at boundaries
	constexpr uint32_t STRIP_MARGIN = 2;
	uint32_t adx = std::min(N, static_cast<uint32_t>(std::abs(dx)) + STRIP_MARGIN);
	uint32_t ady = std::min(N, static_cast<uint32_t>(std::abs(dy)) + STRIP_MARGIN);

	// Horizontal strip (full width, ady height at the exposed edge)
	if (ady > 0) {
		uint32_t strip_y = (dy > 0)
			? region.y
			: region.y + N - ady;
		strips.push_back({region.x, strip_y, N, ady});
	}

	// Vertical strip (adx width, remaining height)
	if (adx > 0) {
		uint32_t strip_x = (dx > 0)
			? region.x
			: region.x + N - adx;
		uint32_t strip_h = N - ady;
		uint32_t strip_y = (dy > 0)
			? region.y + ady
			: region.y;
		strips.push_back({strip_x, strip_y, adx, strip_h});
	}

	return strips;
}

glm::mat4 ShadowRenderSystem::computeStripFrustumVP(
	uint32_t cascade, const StripRegion& strip, uint32_t frame) const {
	auto& region = m_atlas_regions[cascade];
	uint32_t N = region.resolution;
	float radius = m_cascade_state[cascade].current_radius;
	float wpt = (2.0f * radius) / static_cast<float>(N);

	// Convert strip atlas coordinates to light-space ortho bounds
	// Atlas pixel 0 in cascade = light-space coordinate -radius
	float x_min = -radius + static_cast<float>(strip.x - region.x) * wpt;
	float x_max = x_min + static_cast<float>(strip.width) * wpt;
	float y_min = -radius + static_cast<float>(strip.y - region.y) * wpt;
	float y_max = y_min + static_cast<float>(strip.height) * wpt;

	// Extract far plane from the cascade projection to match depth range.
	float far_plane = -1.0f / m_light_projs[frame][cascade][2][2];
	glm::mat4 strip_proj = glm::ortho(x_min, x_max, y_min, y_max, 0.0f, far_plane);
	return strip_proj * m_light_views[frame][cascade];
}

void ShadowRenderSystem::copyPreservedRegion(vk::raii::CommandBuffer& cmd, uint32_t cascade) {
	auto& state = m_cascade_state[cascade];
	auto& region = m_atlas_regions[cascade];
	uint32_t N = region.resolution;
	int32_t dx = state.texel_shift.x;
	int32_t dy = state.texel_shift.y;

	uint32_t preserved_w = N - static_cast<uint32_t>(std::abs(dx));
	uint32_t preserved_h = N - static_cast<uint32_t>(std::abs(dy));

	if (preserved_w == 0 || preserved_h == 0)
		return;

	// Source: skip the edge that scrolled off. Destination: paste offset by the shift.
	// If dx > 0, content shifted right: old texel 0 maps to new texel dx, so dst gets +dx offset.
	// If dx < 0, content shifted left: old texel |dx| maps to new texel 0, so src gets +|dx| offset.
	vk::Offset3D src_offset{std::max(-dx, 0), std::max(-dy, 0), 0};
	vk::Offset3D dst_offset{
		static_cast<int32_t>(region.x) + std::max(dx, 0),
		static_cast<int32_t>(region.y) + std::max(dy, 0),
		0
	};

	vk::ImageCopy copy_region{
		.srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
		.srcOffset = src_offset,
		.dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
		.dstOffset = dst_offset,
		.extent = {preserved_w, preserved_h, 1}
	};
	cmd.copyImage(
		m_cascade_cache[cascade]->getImage(), vk::ImageLayout::eTransferSrcOptimal,
		m_shadow_atlas->getImage(), vk::ImageLayout::eTransferDstOptimal,
		copy_region);
}

void ShadowRenderSystem::updateCascadeCache(vk::raii::CommandBuffer& cmd, uint32_t cascade) {
	auto& region = m_atlas_regions[cascade];

	vk::ImageCopy copy_region{
		.srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
		.srcOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y), 0},
		.dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
		.dstOffset = {0, 0, 0},
		.extent = {region.resolution, region.resolution, 1}
	};
	cmd.copyImage(
		m_shadow_atlas->getImage(), vk::ImageLayout::eTransferSrcOptimal,
		m_cascade_cache[cascade]->getImage(), vk::ImageLayout::eTransferDstOptimal,
		copy_region);
}

bool ShadowRenderSystem::isDynamicEntity(const Registry& registry, Entity entity) {
	return GpuSceneManager::isDynamicEntity(registry, entity);
}

void ShadowRenderSystem::copyStaticCacheToAtlas(vk::raii::CommandBuffer& cmd, uint32_t cascade) {
	auto& region = m_atlas_regions[cascade];

	vk::ImageCopy copy_region{
		.srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
		.srcOffset = {0, 0, 0},
		.dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
		.dstOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y), 0},
		.extent = {region.resolution, region.resolution, 1}
	};
	cmd.copyImage(
		m_cascade_cache[cascade]->getImage(), vk::ImageLayout::eTransferSrcOptimal,
		m_shadow_atlas->getImage(), vk::ImageLayout::eTransferDstOptimal,
		copy_region);
}

void ShadowRenderSystem::snapshotAtlasToStaticCache(vk::raii::CommandBuffer& cmd, uint32_t cascade) {
	updateCascadeCache(cmd, cascade);
}

void ShadowRenderSystem::growShadowInstanceBuffers(uint32_t new_capacity) {
	VE_LOGI("Shadow instance buffer growing: " << m_shadow_instance_capacity << " -> " << new_capacity);
	// createShadowPassDescriptorSets() below frees the existing per-frame shadow
	// global descriptor sets so wait for idle.
	m_ve_device.getDevice().waitIdle();
	m_shadow_instance_capacity = new_capacity;
	for (auto& buf : m_shadow_instance_buffers)
		retireBuffer(std::move(buf));
	createShadowInstanceBuffers();
	createShadowPassDescriptorSets(m_descriptor_pool);
}

void ShadowRenderSystem::renderShadowMaps(VeFrameInfo& frame_info, PbrMegaBuffer& mega_buffer) {
	flushRetiredBuffers();
	const auto& light_views = m_light_views[frame_info.current_frame];

	if (light_views.empty()) return;
	assert(light_views.size() <= MAX_SHADOW_LAYERS && "Active shadow layers exceed MAX_SHADOW_LAYERS");

	bool any_list_dirty = m_static_drawables_dirty || m_dynamic_drawables_dirty;
	if (any_list_dirty) {
		m_static_shadow_drawables.clear();
		m_dynamic_shadow_drawables.clear();
		auto& registry = *frame_info.registry;
		auto view = registry.view<MeshComponent, TransformComponent>();

		for (auto [entity, mesh, tc] : view) {
			if (!mesh.getMesh() || !mesh.has_shadow)
				continue;
			// Skinned meshes go through the per-instance skinned shadow path, which
			// reads SkinningPrePass output buffers instead of the mega buffer.
			if (registry.hasComponent<SkinComponent>(entity))
				continue;
			uint32_t shadow_lod = std::min(std::max(1u, mesh.cached_lod + 1),
			                               mesh.getMesh()->getLodCount() - 1);
			ShadowDrawable drawable{entity, &mesh, shadow_lod};

			if (isDynamicEntity(registry, entity))
				m_dynamic_shadow_drawables.push_back(drawable);
			else
				m_static_shadow_drawables.push_back(drawable);
		}

		auto sortDrawables = [](std::vector<ShadowDrawable>& list) {
			std::sort(list.begin(), list.end(),
				[](const ShadowDrawable& a, const ShadowDrawable& b) {
					VeMesh* ma = a.mesh->getMesh();
					VeMesh* mb = b.mesh->getMesh();
					if (ma != mb)
						return ma < mb;
					return a.lod_level < b.lod_level;
				});
		};
		sortDrawables(m_static_shadow_drawables);
		sortDrawables(m_dynamic_shadow_drawables);

		m_static_drawables_dirty = false;
		m_dynamic_drawables_dirty = false;
	}

	auto& registry = *frame_info.registry;
	uint32_t csm_count = frame_info.csm_data.active_cascade_count;
	size_t all_drawables_size = m_static_shadow_drawables.size() + m_dynamic_shadow_drawables.size();

	// Worst case: CSM static + CSM dynamic + point lights + strip instances
	uint32_t max_needed = static_cast<uint32_t>(all_drawables_size) * (3 + NUM_CSM_CASCADES * 2);
	if (max_needed > m_shadow_instance_capacity)
		growShadowInstanceBuffers(std::max(max_needed, m_shadow_instance_capacity * 2));

	auto* shadow_instance_data = static_cast<InstanceData*>(
		m_shadow_instance_buffers[frame_info.current_frame]->getMappedMemory());
	uint32_t shadow_instance_count = 0;

	// Helper: populate instance groups from a drawable list, frustum-culled
	auto populateInstanceGroups = [&](const std::vector<ShadowDrawable>& drawables,
	                                  std::vector<ShadowInstanceGroup>& groups,
	                                  const FrustumPlane* planes) {
		for (auto& d : drawables) {
			if (planes) {
				const VeMesh::AABB& aabb = d.mesh->getWorldAABB();
				if (!isAABBInFrustum(aabb, planes))
					continue;
			}
			if (shadow_instance_count >= m_shadow_instance_capacity) {
				VE_LOGW("Shadow instance buffer full");
				break;
			}
			uint32_t idx = shadow_instance_count++;
			shadow_instance_data[idx].transform = registry.getWorldTransform(d.entity);
			shadow_instance_data[idx].normal_transform[0] = glm::vec4(0.0f);
			shadow_instance_data[idx].normal_transform[1] = glm::vec4(0.0f);
			shadow_instance_data[idx].normal_transform[2] = glm::vec4(0.0f);

			VeMesh* mesh_ptr = d.mesh->getMesh();
			if (!groups.empty()
				&& groups.back().mesh == mesh_ptr
				&& groups.back().lod_level == d.lod_level) {
				groups.back().instance_count++;
			} else {
				groups.push_back({mesh_ptr, d.lod_level, idx, 1});
			}
		}
	};

	{
	ZoneScopedN("CPU populate instance groups");
	// CSM instance groups: static and dynamic, frustum-culled against outer cascade
	m_static_csm_instance_groups.clear();
	m_dynamic_csm_instance_groups.clear();
	if (csm_count > 0) {
		glm::mat4 outer_vp = m_light_projs[frame_info.current_frame][csm_count - 1]
		                    * m_light_views[frame_info.current_frame][csm_count - 1];
		FrustumPlane planes[6];
		extractFrustumPlanes(outer_vp, planes);

		populateInstanceGroups(m_static_shadow_drawables, m_static_csm_instance_groups, planes);
		populateInstanceGroups(m_dynamic_shadow_drawables, m_dynamic_csm_instance_groups, planes);
	}

	// Point/spot light instance groups: all drawables combined, no frustum cull
	m_shadow_instance_groups.clear();
	populateInstanceGroups(m_static_shadow_drawables, m_shadow_instance_groups, nullptr);
	populateInstanceGroups(m_dynamic_shadow_drawables, m_shadow_instance_groups, nullptr);
	}

	// Skinned shadow casters: write one shadow InstanceData slot per entity. Cull
	// against the outer CSM cascade
	m_skinned_shadow_drawables.clear();
	FrustumPlane skin_cull_planes[6];
	bool have_skin_cull_planes = false;
	if (csm_count > 0) {
		glm::mat4 outer_vp = m_light_projs[frame_info.current_frame][csm_count - 1]
		                   * m_light_views[frame_info.current_frame][csm_count - 1];
		extractFrustumPlanes(outer_vp, skin_cull_planes);
		have_skin_cull_planes = true;
	}
	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>()) {
		if (!mc.has_shadow || !mc.hasMesh() || !mc.hasMaterial())
			continue;
		if (have_skin_cull_planes && !isAABBInFrustum(mc.getWorldAABB(), skin_cull_planes))
			continue;
		if (shadow_instance_count >= m_shadow_instance_capacity) {
			VE_LOGW("Shadow instance buffer full (skinned)");
			break;
		}
		uint32_t idx = shadow_instance_count++;
		shadow_instance_data[idx].transform = registry.getWorldTransform(entity);
		shadow_instance_data[idx].normal_transform[0] = glm::vec4(0.0f);
		shadow_instance_data[idx].normal_transform[1] = glm::vec4(0.0f);
		shadow_instance_data[idx].normal_transform[2] = glm::vec4(0.0f);
		m_skinned_shadow_drawables.push_back({entity, mc.getMesh(), idx});
	}

	auto& command_buffer = frame_info.cmd();

	{
		TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: atlas transition");
		transitionAtlasForRendering(command_buffer, csm_count);
	}

	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	command_buffer.setDepthBias(frame_info.depth_bias_constant, frame_info.depth_bias_clamp, frame_info.depth_bias_slope);

	if (mega_buffer.isValid())
		mega_buffer.bindShadow(command_buffer);

	bool has_dynamics = !m_dynamic_csm_instance_groups.empty();
	bool has_skinned = !m_skinned_shadow_drawables.empty();

	if (csm_count > 0 && (!m_static_csm_instance_groups.empty() || has_dynamics || has_skinned)) {
		ZoneScopedN("CSM cascade loop");
		TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: CSM cascade loop");
		for (uint32_t c = 0; c < csm_count; c++) {
			auto& region = m_atlas_regions[c];
			auto& state = m_cascade_state[c];

			if (c < NUM_CSM_CASCADES && state.dirty) {
				// Static cache needs updating (scroll or static scene change)
				if (state.incremental) {
					TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: strips");
					vk::MemoryBarrier2 pre_strip{
						.sType = vk::StructureType::eMemoryBarrier2,
						.srcStageMask = vk::PipelineStageFlagBits2::eTransfer
							| vk::PipelineStageFlagBits2::eEarlyFragmentTests
							| vk::PipelineStageFlagBits2::eLateFragmentTests,
						.srcAccessMask = vk::AccessFlagBits2::eTransferWrite
							| vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
						.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
							| vk::PipelineStageFlagBits2::eLateFragmentTests,
						.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead
							| vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
					};
					command_buffer.pipelineBarrier2({
						.sType = vk::StructureType::eDependencyInfo,
						.memoryBarrierCount = 1,
						.pMemoryBarriers = &pre_strip,
					});

					auto strips = computeStripRegions(c);

					for (auto& strip : strips) {
						glm::mat4 strip_vp = computeStripFrustumVP(c, strip, frame_info.current_frame);
						FrustumPlane strip_planes[6];
						extractFrustumPlanes(strip_vp, strip_planes);

						// Only render static objects into strips
						std::vector<ShadowInstanceGroup> strip_groups;
						for (auto& d : m_static_shadow_drawables) {
							const VeMesh::AABB& aabb = d.mesh->getWorldAABB();
							if (!isAABBInFrustum(aabb, strip_planes))
								continue;
							if (shadow_instance_count >= m_shadow_instance_capacity)
								break;
							uint32_t idx = shadow_instance_count++;
							shadow_instance_data[idx].transform = registry.getWorldTransform(d.entity);
							shadow_instance_data[idx].normal_transform[0] = glm::vec4(0.0f);
							shadow_instance_data[idx].normal_transform[1] = glm::vec4(0.0f);
							shadow_instance_data[idx].normal_transform[2] = glm::vec4(0.0f);

							VeMesh* mesh_ptr = d.mesh->getMesh();
							if (!strip_groups.empty()
								&& strip_groups.back().mesh == mesh_ptr
								&& strip_groups.back().lod_level == d.lod_level) {
								strip_groups.back().instance_count++;
							} else {
								strip_groups.push_back({mesh_ptr, d.lod_level, idx, 1});
							}
						}

						beginShadowRegionRender(command_buffer, region, vk::AttachmentLoadOp::eLoad, &strip);
						if (!strip_groups.empty()) {
							command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
							command_buffer.setDepthBias(frame_info.depth_bias_constant, frame_info.depth_bias_clamp, frame_info.depth_bias_slope);
							renderShadowMap(frame_info, c, strip_groups, mega_buffer);
						}
						command_buffer.endRendering();
					}
				} else {
					// Full re-render with only static drawables
					TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: full static render");
					beginShadowRegionRender(command_buffer, region, vk::AttachmentLoadOp::eClear);
					renderShadowMap(frame_info, c, m_static_csm_instance_groups, mega_buffer);
					command_buffer.endRendering();
				}

				// Snapshot the clean static result into the cache
				{
					TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: snapshot to cache");
					vk::ImageMemoryBarrier2 to_transfer_src{
						.sType = vk::StructureType::eImageMemoryBarrier2,
						.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
						.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
						.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
						.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
						.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
						.newLayout = vk::ImageLayout::eTransferSrcOptimal,
						.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.image = m_shadow_atlas->getImage(),
						.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
					};
					vk::ImageMemoryBarrier2 cache_to_dst{
						.sType = vk::StructureType::eImageMemoryBarrier2,
						.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
						.srcAccessMask = vk::AccessFlagBits2::eTransferRead,
						.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
						.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
						.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
						.newLayout = vk::ImageLayout::eTransferDstOptimal,
						.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.image = m_cascade_cache[c]->getImage(),
						.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
					};
					vk::ImageMemoryBarrier2 barriers[] = {to_transfer_src, cache_to_dst};
					command_buffer.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = barriers});

					snapshotAtlasToStaticCache(command_buffer, c);

					// Transition cache back to transfer-src for future restores, atlas back to depth-attachment
					vk::ImageMemoryBarrier2 cache_back{
						.sType = vk::StructureType::eImageMemoryBarrier2,
						.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
						.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
						.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
						.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
						.oldLayout = vk::ImageLayout::eTransferDstOptimal,
						.newLayout = vk::ImageLayout::eTransferSrcOptimal,
						.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.image = m_cascade_cache[c]->getImage(),
						.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
					};
					vk::ImageMemoryBarrier2 atlas_back{
						.sType = vk::StructureType::eImageMemoryBarrier2,
						.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
						.srcAccessMask = vk::AccessFlagBits2::eTransferRead,
						.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
							| vk::PipelineStageFlagBits2::eLateFragmentTests,
						.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
							| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
						.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
						.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
						.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
						.image = m_shadow_atlas->getImage(),
						.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
					};
					vk::ImageMemoryBarrier2 post_barriers[] = {cache_back, atlas_back};
					command_buffer.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = post_barriers});
				}
			} else if (has_dynamics || has_skinned) {
				// Static cache is clean but we need it in the atlas for the dynamic/skinned overlay
				TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: cache restore");
				vk::ImageMemoryBarrier2 atlas_to_dst{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
						| vk::PipelineStageFlagBits2::eLateFragmentTests,
					.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
					.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
					.newLayout = vk::ImageLayout::eTransferDstOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_shadow_atlas->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				command_buffer.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_to_dst});

				copyStaticCacheToAtlas(command_buffer, c);

				vk::ImageMemoryBarrier2 atlas_back{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
					.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
						| vk::PipelineStageFlagBits2::eLateFragmentTests,
					.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
						| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
					.oldLayout = vk::ImageLayout::eTransferDstOptimal,
					.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_shadow_atlas->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				command_buffer.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_back});
			} else {
				// Static cache is clean AND nothing to overlay: skip cascade entirely
				continue;
			}

			// Render dynamic + skinned objects on top using the full cascade VP (loadOp = eLoad)
			if (has_dynamics || has_skinned) {
				TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: dynamic+skinned overlay");
				beginShadowRegionRender(command_buffer, region, vk::AttachmentLoadOp::eLoad);
				command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
				command_buffer.setDepthBias(frame_info.depth_bias_constant, frame_info.depth_bias_clamp, frame_info.depth_bias_slope);
				renderShadowMap(frame_info, c, m_dynamic_csm_instance_groups, mega_buffer, /*include_skinned=*/true);
				command_buffer.endRendering();
			}
		}
	}

	// Point/spot light shadows
	if (!m_shadow_instance_groups.empty() || has_skinned) {
		ZoneScopedN("Light shadows");
		TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: light shadows");
		for (size_t i = csm_count; i < light_views.size(); i++) {
			uint32_t layer = NUM_CSM_CASCADES + static_cast<uint32_t>(i - csm_count);
			assert(layer < MAX_SHADOW_LAYERS);
			auto& region = m_atlas_regions[layer];
			beginShadowRegionRender(command_buffer, region, vk::AttachmentLoadOp::eClear);
			renderShadowMap(frame_info, layer, m_shadow_instance_groups, mega_buffer, /*include_skinned=*/true);
			command_buffer.endRendering();
		}
	}

	{
		TracyVkZone(m_tracy_gfx_ctx, *command_buffer, "Shadow: post-render transition");
		transitionAtlasPostRender(command_buffer);
	}
}

void ShadowRenderSystem::renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index,
                                          const std::vector<ShadowInstanceGroup>& instance_groups,
                                          const PbrMegaBuffer& mega_buffer,
                                          bool include_skinned) const {

	const vk::raii::DescriptorSet& shadow_global_set = m_shadow_global_descriptor_sets[frame_info.current_frame][light_index];
	auto& cmd = frame_info.cmd();

	cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*shadow_global_set},
		{}
	);

	if (mega_buffer.isValid()) {
		mega_buffer.bindShadow(cmd);

		for (const auto& group : instance_groups) {
			const auto* entry = mega_buffer.getEntry(group.mesh);
			if (!entry)
				continue;
			uint32_t lod = std::min(group.lod_level, static_cast<uint32_t>(entry->lod_entries.size()) - 1);
			const auto& lod_entry = entry->lod_entries[lod];

			ShadowPushConstantData push{};
			push.instance_offset = group.first_instance;
			cmd.pushConstants(
				*m_pipeline_layout,
				vk::ShaderStageFlagBits::eVertex,
				0,
				vk::ArrayProxy<const uint8_t>(sizeof(ShadowPushConstantData), reinterpret_cast<const uint8_t*>(&push))
			);

			cmd.drawIndexed(
				lod_entry.index_count, group.instance_count,
				lod_entry.first_index, static_cast<int32_t>(entry->vertex_offset), 0);
		}
	}

	if (include_skinned && !m_skinned_shadow_drawables.empty() && frame_info.skinning_pre_pass) {
		mega_buffer.bindShadow(cmd);
		for (const auto& sd : m_skinned_shadow_drawables) {
			if (!sd.mesh)
				continue;
			const auto* entry = mega_buffer.getEntry(sd.mesh);
			if (!entry || entry->lod_entries.empty())
				continue;
			uint32_t vo = frame_info.skinning_pre_pass->getSkinnedVertexOffset(
				sd.entity, frame_info.current_frame, mega_buffer);
			if (vo == SkinningPrePass::INVALID_OFFSET)
				continue;

			ShadowPushConstantData push{};
			push.instance_offset = sd.instance_offset;
			cmd.pushConstants(
				*m_pipeline_layout,
				vk::ShaderStageFlagBits::eVertex,
				0,
				vk::ArrayProxy<const uint8_t>(sizeof(ShadowPushConstantData), reinterpret_cast<const uint8_t*>(&push))
			);
			const auto& lod = entry->lod_entries[0];
			cmd.drawIndexed(lod.index_count, 1, lod.first_index, static_cast<int32_t>(vo), 0);
		}
	}
}

void ShadowRenderSystem::ensureCascadeUbos() {
	if (!m_csm_cascade_ubos.empty())
		return;
	m_csm_cascade_ubos.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
			m_csm_cascade_ubos[frame].emplace_back(std::make_unique<VeBuffer>(
				m_ve_device, sizeof(ShadowPassUBO), 1,
				vk::BufferUsageFlagBits::eUniformBuffer,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
				m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
			));
			m_csm_cascade_ubos[frame].back()->map();
		}
	}
}

void ShadowRenderSystem::createShadowSlotDescriptorSets(
	std::function<VeBuffer&(uint32_t frame, uint32_t slot)> get_instance_buffer,
	std::vector<std::vector<vk::raii::DescriptorSet>>& out_cascade_sets,
	std::vector<std::vector<vk::raii::DescriptorSet>>& out_light_sets) {

	ensureCascadeUbos();

	out_cascade_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		out_cascade_sets[frame].clear();
		for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
			vk::DescriptorBufferInfo cascade_ubo_info{
				.buffer = m_csm_cascade_ubos[frame][cascade]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};
			auto instance_info = get_instance_buffer(
				static_cast<uint32_t>(frame), cascade).getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, m_descriptor_pool)
				.writeBuffer(0, &cascade_ubo_info)
				.writeBuffer(1, &instance_info)
				.build(ds);
			out_cascade_sets[frame].push_back(std::move(ds));
		}
	}

	out_light_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		out_light_sets[frame].clear();
		for (uint32_t light = 0; light < MAX_SHADOW_LIGHTS; light++) {
			uint32_t layer = NUM_CSM_CASCADES + light;

			vk::DescriptorBufferInfo buffer_info{
				.buffer = m_shadow_ubos[frame][layer]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};
			auto instance_info = get_instance_buffer(
				static_cast<uint32_t>(frame), layer).getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, m_descriptor_pool)
				.writeBuffer(0, &buffer_info)
				.writeBuffer(1, &instance_info)
				.build(ds);
			out_light_sets[frame].push_back(std::move(ds));
		}
	}
}

void ShadowRenderSystem::createGpuShadowDescriptorSets(GpuCullingSystem& gpu_cull_system) {
	createShadowSlotDescriptorSets(
		[&](uint32_t f, uint32_t s) -> VeBuffer& {
			return gpu_cull_system.getShadowInstanceBuffer(f, s);
		},
		m_gpu_cascade_descriptor_sets, m_gpu_shadow_descriptor_sets);
}

void ShadowRenderSystem::releaseGpuShadowDescriptorSets() {
	m_gpu_cascade_descriptor_sets.clear();
	m_gpu_shadow_descriptor_sets.clear();
}

void ShadowRenderSystem::renderShadowMapsGpuCulled(VeFrameInfo& frame_info,
                                                    GpuCullingSystem& gpu_cull_system,
                                                    PbrMegaBuffer& mega_buffer,
                                                    GpuSceneManager& scene_mgr) {
	const auto& light_views = m_light_views[frame_info.current_frame];
	if (light_views.empty())
		return;

	auto& cmd = frame_info.cmd();
	uint32_t frame = frame_info.current_frame;
	uint32_t csm_count = frame_info.csm_data.active_cascade_count;
	uint32_t num_shadow_lights = static_cast<uint32_t>(light_views.size()) - csm_count;

	const CameraView* cam_for_hiz = gpu_cull_system.isHizEnabled() ? &frame_info.camera_view : nullptr;

	// If an object changed between static and dynamic, force a full re-render
	if (scene_mgr.consumeDynamicClassificationChanged()) {
		for (uint32_t c = 0; c < NUM_CSM_CASCADES; c++) {
			m_cascade_state[c].dirty = true;
			m_cascade_state[c].incremental = false;
		}
	}

	// Classify cascades
	bool any_incremental = false;
	bool any_full_dirty = false;
	for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES; c++) {
		if (!m_cascade_state[c].dirty) continue;
		if (m_cascade_state[c].incremental) any_incremental = true;
		else any_full_dirty = true;
	}
	bool has_dynamics = scene_mgr.hasDynamicObjects();
	bool any_dirty = any_incremental || any_full_dirty || num_shadow_lights > 0
		|| has_dynamics;
	if (!any_dirty)
		return;

	// Batch cull dispatches for full-dirty cascades (static only) and point/spot
	// lights (all)
	std::array<GpuCullingSystem::ShadowCullRequest,
	           GpuCullingSystem::SHADOW_BUFFER_COUNT> shadow_requests{};
	uint32_t shadow_req_count = 0;

	for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES; c++) {
		if (!m_cascade_state[c].dirty || m_cascade_state[c].incremental)
			continue;
		glm::mat4 cascade_vp = m_light_projs[frame][c] * m_light_views[frame][c];
		shadow_requests[shadow_req_count++] = {
			.view_proj   = cascade_vp,
			.camera_view = cam_for_hiz,
			.slot        = c,
			.lod_bias    = static_cast<int32_t>(c) + 1,
			.shadow_mode = ShadowPassMode::STATIC_ONLY,
		};
	}
	for (uint32_t i = 0; i < num_shadow_lights; i++) {
		glm::mat4 light_vp = m_light_projs[frame][csm_count + i]
		                   * m_light_views[frame][csm_count + i];
		shadow_requests[shadow_req_count++] = {
			.view_proj   = light_vp,
			.camera_view = cam_for_hiz,
			.slot        = NUM_CSM_CASCADES + i,
			.lod_bias    = 2,
			.shadow_mode = ShadowPassMode::ALL_OBJECTS,
		};
	}
	if (shadow_req_count > 0) {
		ZoneScopedN("Batched cull");
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: batched cull");
		gpu_cull_system.dispatchShadowCulls(cmd,
			shadow_requests.data(), shadow_req_count, scene_mgr, frame);
		gpu_cull_system.flushShadowCullBarrier(cmd, scene_mgr, frame);
	}

	{
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: atlas transition");
		transitionAtlasForRendering(cmd, csm_count);
	}

	ShadowPushConstantData push{.instance_offset = 0};

	auto drawIndirect = [&](uint32_t slot) {
		auto& shadow_indirect = gpu_cull_system.getShadowIndirectBuffer(frame, slot);
		for (uint32_t bucket = 0; bucket < GpuCullingSystem::BUCKET_COUNT; bucket++) {
			uint32_t group_count = scene_mgr.getBucketGroupCount(bucket);
			if (group_count == 0) continue;
			auto offset = static_cast<vk::DeviceSize>(
				scene_mgr.getBucketGroupOffset(bucket)) * sizeof(VkDrawIndexedIndirectCommand);
			if (gpu_cull_system.compactionEnabled()) {
				cmd.drawIndexedIndirectCount(
					gpu_cull_system.getShadowCompactedIndirectBuffer(frame, slot).getBuffer(), offset,
					gpu_cull_system.getShadowCompactCountBuffer(frame, slot).getBuffer(),
					bucket * sizeof(uint32_t),
					group_count, sizeof(VkDrawIndexedIndirectCommand));
			} else {
				cmd.drawIndexedIndirect(shadow_indirect.getBuffer(), offset,
					group_count, sizeof(VkDrawIndexedIndirectCommand));
			}
		}
	};

	auto rebindGraphicsState = [&]() {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
		cmd.setDepthBias(frame_info.depth_bias_constant, frame_info.depth_bias_clamp, frame_info.depth_bias_slope);
		mega_buffer.bindShadow(cmd);
		cmd.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
			vk::ArrayProxy<const uint8_t>(sizeof(push), reinterpret_cast<const uint8_t*>(&push)));
	};

	// Per-cascade: render static, snapshot to cache, then dynamic overlay
	{
	ZoneScopedN("CSM cascade loop");
	TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: CSM cascade loop");
	for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES; c++) {
		auto& region = m_atlas_regions[c];

		if (m_cascade_state[c].dirty) {
			if (m_cascade_state[c].incremental) {
				// Per-strip cull + render (static only)
				ZoneScopedN("Cascade strips");
				TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: strips");
				auto strips = computeStripRegions(c);
				for (auto& strip : strips) {
					TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: strip cull+render");
					glm::mat4 strip_vp = computeStripFrustumVP(c, strip, frame);
					GpuCullingSystem::ShadowCullRequest strip_req{
						.view_proj   = strip_vp,
						.camera_view = cam_for_hiz,
						.slot        = c,
						.lod_bias    = static_cast<int32_t>(c) + 1,
						.shadow_mode = ShadowPassMode::STATIC_ONLY,
					};
					gpu_cull_system.dispatchShadowCulls(cmd, &strip_req, 1, scene_mgr, frame);
					gpu_cull_system.flushShadowCullBarrier(cmd, scene_mgr, frame);

					beginShadowRegionRender(cmd, region, vk::AttachmentLoadOp::eLoad, &strip);
					rebindGraphicsState();
					cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
						0, {*m_gpu_cascade_descriptor_sets[frame][c]}, {});
					drawIndirect(c);
					cmd.endRendering();
				}
			} else {
				// Full re-render (static only, already dispatched above)
				TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: full static render");
				beginShadowRegionRender(cmd, region, vk::AttachmentLoadOp::eClear);
				rebindGraphicsState();
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
					0, {*m_gpu_cascade_descriptor_sets[frame][c]}, {});
				drawIndirect(c);
				cmd.endRendering();
			}

			// Snapshot static result to cache
			{
				TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: snapshot to cache");
				vk::ImageMemoryBarrier2 to_src{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
					.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
					.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
					.newLayout = vk::ImageLayout::eTransferSrcOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_shadow_atlas->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 cache_dst{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferRead,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
					.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
					.newLayout = vk::ImageLayout::eTransferDstOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_cascade_cache[c]->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 snap_barriers[] = {to_src, cache_dst};
				cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = snap_barriers});
				snapshotAtlasToStaticCache(cmd, c);

				vk::ImageMemoryBarrier2 cache_back{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
					.oldLayout = vk::ImageLayout::eTransferDstOptimal,
					.newLayout = vk::ImageLayout::eTransferSrcOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_cascade_cache[c]->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 atlas_back{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferRead,
					.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
						| vk::PipelineStageFlagBits2::eLateFragmentTests,
					.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
						| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
					.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
					.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_shadow_atlas->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 post_barriers[] = {cache_back, atlas_back};
				cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = post_barriers});
			}
		} else if (has_dynamics) {
			// Restore static cache to atlas for dynamic overlay
			TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: cache restore");
			vk::ImageMemoryBarrier2 atlas_dst{
				.sType = vk::StructureType::eImageMemoryBarrier2,
				.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
					| vk::PipelineStageFlagBits2::eLateFragmentTests,
				.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
				.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
				.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.newLayout = vk::ImageLayout::eTransferDstOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_shadow_atlas->getImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
			};
			cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_dst});
			copyStaticCacheToAtlas(cmd, c);

			vk::ImageMemoryBarrier2 atlas_back{
				.sType = vk::StructureType::eImageMemoryBarrier2,
				.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
				.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
					| vk::PipelineStageFlagBits2::eLateFragmentTests,
				.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
					| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
				.oldLayout = vk::ImageLayout::eTransferDstOptimal,
				.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_shadow_atlas->getImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
			};
			cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_back});
		} else {
			continue;
		}

		// Dynamic overlay: cull + render dynamic objects on top using full cascade VP
		if (has_dynamics) {
			TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: dynamic overlay");
			glm::mat4 cascade_vp = m_light_projs[frame][c] * m_light_views[frame][c];
			GpuCullingSystem::ShadowCullRequest dyn_req{
				.view_proj   = cascade_vp,
				.camera_view = cam_for_hiz,
				.slot        = c,
				.lod_bias    = static_cast<int32_t>(c) + 1,
				.shadow_mode = ShadowPassMode::DYNAMIC_ONLY,
			};
			gpu_cull_system.dispatchShadowCulls(cmd, &dyn_req, 1, scene_mgr, frame);
			gpu_cull_system.flushShadowCullBarrier(cmd, scene_mgr, frame);

			beginShadowRegionRender(cmd, region, vk::AttachmentLoadOp::eLoad);
			rebindGraphicsState();
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
				0, {*m_gpu_cascade_descriptor_sets[frame][c]}, {});
			drawIndirect(c);
			cmd.endRendering();
		}
	}
	} // CSM cascade loop zone

	// Point/spot light shadows (already culled with ALL_OBJECTS)
	if (num_shadow_lights > 0) {
		ZoneScopedN("Light shadows");
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: light shadows");
		for (uint32_t i = 0; i < num_shadow_lights; i++) {
			uint32_t slot = NUM_CSM_CASCADES + i;
			beginShadowRegionRender(cmd, m_atlas_regions[slot], vk::AttachmentLoadOp::eClear);
			rebindGraphicsState();
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
				0, {*m_gpu_shadow_descriptor_sets[frame][i]}, {});
			drawIndirect(slot);
			cmd.endRendering();
		}
	}

	{
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: post-render transition");
		transitionAtlasPostRender(cmd);
	}
}

void ShadowRenderSystem::createMeshletShadowDescriptorSets(MeshletCullingSystem& meshlet_cull) {
	createShadowSlotDescriptorSets(
		[&](uint32_t f, uint32_t s) -> VeBuffer& {
			return meshlet_cull.getShadowInstanceBuffer(f, s);
		},
		m_meshlet_cascade_descriptor_sets, m_meshlet_shadow_descriptor_sets);
}

void ShadowRenderSystem::releaseMeshletShadowDescriptorSets() {
	m_meshlet_cascade_descriptor_sets.clear();
	m_meshlet_shadow_descriptor_sets.clear();
}

void ShadowRenderSystem::renderShadowMapsGpuCulledMeshlets(VeFrameInfo& frame_info,
                                                            MeshletCullingSystem& meshlet_cull,
                                                            PbrMegaBuffer& mega_buffer,
                                                            GpuSceneManager& scene_mgr) {
	const auto& light_views = m_light_views[frame_info.current_frame];
	if (light_views.empty())
		return;

	auto& cmd = frame_info.cmd();
	uint32_t frame = frame_info.current_frame;
	uint32_t csm_count = frame_info.csm_data.active_cascade_count;
	uint32_t num_shadow_lights = static_cast<uint32_t>(light_views.size()) - csm_count;

	if (scene_mgr.consumeDynamicClassificationChanged()) {
		for (uint32_t c = 0; c < NUM_CSM_CASCADES; c++) {
			m_cascade_state[c].dirty = true;
			m_cascade_state[c].incremental = false;
		}
	}

	bool has_dynamics = scene_mgr.hasDynamicObjects();
	bool any_dirty = num_shadow_lights > 0 || has_dynamics;
	for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES && !any_dirty; c++)
		any_dirty = m_cascade_state[c].dirty;
	if (!any_dirty)
		return;

	// Batch cull dispatches for full-dirty cascades (static only) and point/spot lights (all)
	std::vector<MeshletCullingSystem::ShadowCullRequest> requests;
	requests.reserve(csm_count + num_shadow_lights);
	for (uint32_t c = 0; c < csm_count; c++) {
		if (c < NUM_CSM_CASCADES && (!m_cascade_state[c].dirty || m_cascade_state[c].incremental))
			continue;
		const glm::mat4& lv = m_light_views[frame][c];
		glm::vec3 light_dir = -glm::vec3(lv[0][2], lv[1][2], lv[2][2]);
		requests.push_back({
			.view_proj    = m_light_projs[frame][c] * lv,
			.light_pos    = -light_dir * 100000.0f,
			.slot         = c,
			.lod_bias     = static_cast<int32_t>(c) + 1,
			.shadow_mode  = ShadowPassMode::STATIC_ONLY,
		});
	}
	for (uint32_t i = 0; i < num_shadow_lights; i++) {
		const glm::mat4& lv = m_light_views[frame][csm_count + i];
		requests.push_back({
			.view_proj    = m_light_projs[frame][csm_count + i] * lv,
			.light_pos    = -glm::transpose(glm::mat3(lv)) * glm::vec3(lv[3]),
			.slot         = NUM_CSM_CASCADES + i,
			.lod_bias     = 2,
			.shadow_mode  = ShadowPassMode::ALL_OBJECTS,
		});
	}

	if (!requests.empty()) {
		ZoneScopedN("Batched cull");
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: batched cull");
		meshlet_cull.dispatchShadowCulls(cmd, requests.data(),
			static_cast<uint32_t>(requests.size()), scene_mgr, frame);
	}

	{
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: atlas transition");
		transitionAtlasForRendering(cmd, csm_count);
	}

	ShadowPushConstantData push{.instance_offset = 0};

	auto rebindMeshletState = [&]() {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
		cmd.setDepthBias(frame_info.depth_bias_constant, frame_info.depth_bias_clamp, frame_info.depth_bias_slope);
		mega_buffer.bindShadowMeshletIbo(cmd);
		cmd.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
			vk::ArrayProxy<const uint8_t>(sizeof(push), reinterpret_cast<const uint8_t*>(&push)));
	};

	rebindMeshletState();

	auto drawMeshletIndirect = [&](uint32_t slot, bool is_dynamic = false) {
		auto& shadow_indirect = meshlet_cull.getShadowMeshletIndirectBuffer(frame, slot);
		auto& shadow_draw_counts = meshlet_cull.getShadowMeshletDrawCounts(frame, slot);
		const uint32_t* cpu_counts = meshlet_cull.getShadowCpuDrawCounts(slot, is_dynamic);
		for (uint32_t bucket = 0; bucket < MESHLET_SHADOW_BUCKET_COUNT; bucket++) {
			auto buf_offset = static_cast<vk::DeviceSize>(bucket) * MAX_MESHLET_SHADOW_DRAWS_PER_BUCKET
			                  * sizeof(VkDrawIndexedIndirectCommand);
			if (cpu_counts) {
				uint32_t count = std::min(cpu_counts[bucket], MAX_MESHLET_SHADOW_DRAWS_PER_BUCKET);
				cmd.drawIndexedIndirect(shadow_indirect.getBuffer(), buf_offset,
					count, sizeof(VkDrawIndexedIndirectCommand));
			} else {
				auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
				cmd.drawIndexedIndirectCount(
					shadow_indirect.getBuffer(), buf_offset,
					shadow_draw_counts.getBuffer(), count_offset,
					MAX_MESHLET_SHADOW_DRAWS_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
			}
		}
	};

	ZoneScopedN("CSM cascade loop");
	TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: CSM meshlet cascade loop");
	// Per-cascade: render static, snapshot to cache, then dynamic overlay
	for (uint32_t c = 0; c < csm_count && c < NUM_CSM_CASCADES; c++) {
		auto& region = m_atlas_regions[c];

		if (m_cascade_state[c].dirty) {
			if (m_cascade_state[c].incremental) {
				ZoneScopedN("Cascade strips");
				TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: strips");
				auto strips = computeStripRegions(c);
				const glm::mat4& lv = m_light_views[frame][c];
				glm::vec3 light_dir = -glm::vec3(lv[0][2], lv[1][2], lv[2][2]);

				for (auto& strip : strips) {
					TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: strip cull+render");
					glm::mat4 strip_vp = computeStripFrustumVP(c, strip, frame);
					MeshletCullingSystem::ShadowCullRequest strip_req{
						.view_proj    = strip_vp,
						.light_pos    = -light_dir * 100000.0f,
						.slot         = c,
						.lod_bias     = static_cast<int32_t>(c) + 1,
						.shadow_mode  = ShadowPassMode::STATIC_ONLY,
					};
					meshlet_cull.dispatchShadowCulls(cmd, &strip_req, 1, scene_mgr, frame, true);

					beginShadowRegionRender(cmd, region, vk::AttachmentLoadOp::eLoad, &strip);
					rebindMeshletState();
					cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
						0, {*m_meshlet_cascade_descriptor_sets[frame][c]}, {});
					drawMeshletIndirect(c);
					cmd.endRendering();
				}
			} else {
				TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: full static render");
				beginShadowRegionRender(cmd, region, vk::AttachmentLoadOp::eClear);
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
					0, {*m_meshlet_cascade_descriptor_sets[frame][c]}, {});
				drawMeshletIndirect(c);
				cmd.endRendering();
			}

			// Snapshot static result to cache
			{
				TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: snapshot to cache");
				vk::ImageMemoryBarrier2 to_src{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
					.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
					.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
					.newLayout = vk::ImageLayout::eTransferSrcOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_shadow_atlas->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 cache_dst{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferRead,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
					.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
					.newLayout = vk::ImageLayout::eTransferDstOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_cascade_cache[c]->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 snap_barriers[] = {to_src, cache_dst};
				cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = snap_barriers});
				snapshotAtlasToStaticCache(cmd, c);

				vk::ImageMemoryBarrier2 cache_back{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
					.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.dstAccessMask = vk::AccessFlagBits2::eTransferRead,
					.oldLayout = vk::ImageLayout::eTransferDstOptimal,
					.newLayout = vk::ImageLayout::eTransferSrcOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_cascade_cache[c]->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 atlas_back{
					.sType = vk::StructureType::eImageMemoryBarrier2,
					.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
					.srcAccessMask = vk::AccessFlagBits2::eTransferRead,
					.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
						| vk::PipelineStageFlagBits2::eLateFragmentTests,
					.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
						| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
					.oldLayout = vk::ImageLayout::eTransferSrcOptimal,
					.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m_shadow_atlas->getImage(),
					.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
				};
				vk::ImageMemoryBarrier2 post_barriers[] = {cache_back, atlas_back};
				cmd.pipelineBarrier2({.imageMemoryBarrierCount = 2, .pImageMemoryBarriers = post_barriers});
			}
		} else if (has_dynamics) {
			TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: cache restore");
			// Restore static cache to atlas for dynamic overlay
			vk::ImageMemoryBarrier2 atlas_dst{
				.sType = vk::StructureType::eImageMemoryBarrier2,
				.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
					| vk::PipelineStageFlagBits2::eLateFragmentTests,
				.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
				.dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
				.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.newLayout = vk::ImageLayout::eTransferDstOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_shadow_atlas->getImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
			};
			cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_dst});
			copyStaticCacheToAtlas(cmd, c);

			vk::ImageMemoryBarrier2 atlas_back{
				.sType = vk::StructureType::eImageMemoryBarrier2,
				.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
				.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests
					| vk::PipelineStageFlagBits2::eLateFragmentTests,
				.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite
					| vk::AccessFlagBits2::eDepthStencilAttachmentRead,
				.oldLayout = vk::ImageLayout::eTransferDstOptimal,
				.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_shadow_atlas->getImage(),
				.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
			};
			cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &atlas_back});
		} else {
			continue;
		}

		// Dynamic overlay: cull + render dynamic objects on top using full cascade VP
		if (has_dynamics) {
			TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: dynamic overlay");
			const glm::mat4& lv = m_light_views[frame][c];
			glm::vec3 light_dir = -glm::vec3(lv[0][2], lv[1][2], lv[2][2]);
			MeshletCullingSystem::ShadowCullRequest dyn_req{
				.view_proj    = m_light_projs[frame][c] * lv,
				.light_pos    = -light_dir * 100000.0f,
				.slot         = c,
				.lod_bias     = static_cast<int32_t>(c) + 1,
				.shadow_mode  = ShadowPassMode::DYNAMIC_ONLY,
			};
			meshlet_cull.dispatchShadowCulls(cmd, &dyn_req, 1, scene_mgr, frame);

			beginShadowRegionRender(cmd, region, vk::AttachmentLoadOp::eLoad);
			rebindMeshletState();
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
				0, {*m_meshlet_cascade_descriptor_sets[frame][c]}, {});
			drawMeshletIndirect(c, true);
			cmd.endRendering();
		}
	}

	// Point/spot light shadows (already culled with ALL_OBJECTS)
	if (num_shadow_lights > 0) {
		ZoneScopedN("Light shadows");
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: light shadows");
		for (uint32_t i = 0; i < num_shadow_lights; i++) {
			uint32_t slot = NUM_CSM_CASCADES + i;
			beginShadowRegionRender(cmd, m_atlas_regions[slot], vk::AttachmentLoadOp::eClear);
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
				0, {*m_meshlet_shadow_descriptor_sets[frame][i]}, {});
			drawMeshletIndirect(slot);
			cmd.endRendering();
		}
	}

	{
		TracyVkZone(m_tracy_gfx_ctx, *cmd, "Shadow: post-render transition");
		transitionAtlasPostRender(cmd);
	}
}

} // namespace ve
