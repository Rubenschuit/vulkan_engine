#include "pch.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/gpu_culling_system.hpp"
#include "rendering/meshlet_culling_system.hpp"
#include "rendering/gpu_scene_manager.hpp"
#include "rendering/pbr_mega_buffer.hpp"
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

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <algorithm>
#include <vector>

namespace ve {

struct ShadowPushConstantData {
	alignas(4) uint32_t instance_offset; // SSBO offset (SV_InstanceID is 0-based in Slang)
};
static_assert(sizeof(ShadowPushConstantData) == 4, "Shadow push constants must be 4 bytes");

ShadowRenderSystem::ShadowRenderSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_descriptor_pool(descriptor_pool), m_shader_path(shader_path) {

	// Initialize 2D arrays for per-frame point light data
	m_shadow_ubos.resize(MAX_FRAMES_IN_FLIGHT);
	m_shadow_global_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_views.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_projs.resize(MAX_FRAMES_IN_FLIGHT);

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_ubos[i].resize(MAX_SHADOW_LAYERS);
		m_shadow_global_descriptor_sets[i].reserve(MAX_SHADOW_LAYERS);
	}

	// Compute atlas layout and create shadow atlas image
	computeAtlasLayout();
	createShadowResources();

	// Create shadow global descriptor set layout (for shadow pass UBO + instance SSBO)
	m_shadow_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.build();

	// Create shadow descriptor set layout (for sampling shadow atlas)
	// eCompute needed so the screen-space shadow mask compute shader can read shadow maps
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

	// Depth-only pipeline: no color output
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
	// Per-frame, per-layer UBOs (all shadow sources share the same ShadowPassUBO format)
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
	// Point light shadow descriptor sets: per-frame, per-shadow-layer
	// Each set: binding 0 = per-light UBO (ShadowPassUBO), binding 1 = shadow instance SSBO
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
	// Two-phase packing:
	//   Phase 1: cascade 0 (top-left) + remaining cascades stacked in column 1 (top-right)
	//   Phase 2: lights fill remaining column 1 space, then overflow to a bottom row

	uint32_t c0_res = CSM_CASCADE_RESOLUTIONS[0];
	m_atlas_regions[0] = {0, 0, c0_res};

	// Column 1: remaining cascades stacked vertically
	uint32_t col1_x = c0_res;
	uint32_t col1_width = 0;
	uint32_t stack_y = 0;
	for (uint32_t c = 1; c < NUM_CSM_CASCADES; c++) {
		uint32_t res = CSM_CASCADE_RESOLUTIONS[c];
		m_atlas_regions[c] = {col1_x, stack_y, res};
		col1_width = std::max(col1_width, res);
		stack_y += res;
	}

	// Light packing: grid into column 1 dead space, then overflow below cascade 0
	constexpr uint32_t total_lights = MAX_POINT_SHADOW_LIGHTS + MAX_SPOT_SHADOW_LIGHTS;
	auto lightRes = [](uint32_t i) -> uint32_t {
		return (i < MAX_POINT_SHADOW_LIGHTS) ? POINT_SHADOW_RESOLUTION : SPOT_SHADOW_RESOLUTION;
	};
	uint32_t max_light_res = std::max(POINT_SHADOW_RESOLUTION, SPOT_SHADOW_RESOLUTION);

	uint32_t placed = 0;
	// Fill remaining column 1 space (below cascades, within cascade 0 height)
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

	// Overflow: remaining lights in a horizontal row below the top zone
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

	// Find suitable depth format
	m_shadow_depth_format = m_ve_device.findSupportedFormat(
		{vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
		vk::ImageTiling::eOptimal,
		vk::FormatFeatureFlagBits::eDepthStencilAttachment | vk::FormatFeatureFlagBits::eSampledImage
	);

	// Create a single 2D depth image for the shadow atlas
	m_shadow_atlas = std::make_unique<VeImage>(
		m_ve_device,
		m_atlas_width,
		m_atlas_height,
		vk::SampleCountFlagBits::e1,
		m_shadow_depth_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eDepth,
		false,  // not cubemap
		1
	);

	// Transition shadow atlas
	m_shadow_atlas->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		vk::AccessFlags2{},
		vk::AccessFlagBits2::eDepthStencilAttachmentRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);

	// Create comparison sampler (for SampleCmpLevelZero) and raw sampler (for PCSS)
	m_shadow_sampler = VeTexture::createDepthCompareSampler(m_ve_device);
	m_shadow_raw_sampler = VeTexture::createShadowRawSampler(m_ve_device);
}

void ShadowRenderSystem::createShadowTextureDescriptorSets(VeDescriptorPool& descriptor_pool) {
	// Create descriptor sets for shadow textures (per-frame) - used by other systems to sample shadows
	m_shadow_descriptor_sets.clear();
	m_shadow_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);

	// Prepare descriptor infos (need to persist during the build call)
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

	// Create and update all per-frame shadow descriptor sets using the builder pattern
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_shadow_set_layout, descriptor_pool)
			.writeImage(0, &cmp_sampler_info)   // binding 0: comparison sampler
			.writeImage(1, &image_info)          // binding 1: shadow map array
			.writeImage(2, &raw_sampler_info)    // binding 2: raw sampler (PCSS)
			.build(set);
		m_shadow_descriptor_sets.push_back(std::move(set));
	}
}

// Update shadow UBOs from CSM cascade data + point light shadow data
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

	// Helper: builds atlas bias matrix for a given shadow layer
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

	// CSM cascades: write per-cascade ShadowPassUBOs + dirty tracking
	if (csm_data.active_cascade_count > 0) {
		for (uint32_t cascade = 0; cascade < csm_data.active_cascade_count; cascade++) {
			m_light_views[frame_index][cascade] = csm_data.light_view[cascade];
			m_light_projs[frame_index][cascade] = csm_data.light_proj[cascade];

			ShadowPassUBO cascade_ubo{};
			cascade_ubo.view = csm_data.light_view[cascade];
			cascade_ubo.proj = csm_data.light_proj[cascade];
			cascade_ubo.projection_view = cascade_ubo.proj * cascade_ubo.view;

			// Dirty tracking: skip cascades whose matrices haven't changed
			auto& state = m_cascade_state[cascade];
			if (!m_force_full_rerender && state.valid
				&& state.prev_view == cascade_ubo.view
				&& state.prev_proj == cascade_ubo.proj) {
				state.dirty = false;
			} else {
				state.dirty = true;
				state.prev_view = cascade_ubo.view;
				state.prev_proj = cascade_ubo.proj;
				state.valid = true;
			}

			// Per-layer UBO (used by CPU shadow path descriptor sets)
			m_shadow_ubos[frame_index][cascade]->writeToBuffer(&cascade_ubo);

			// Per-cascade UBO (used by GPU/meshlet culled shadow paths)
			if (!m_csm_cascade_ubos.empty() && cascade < m_csm_cascade_ubos[frame_index].size())
				m_csm_cascade_ubos[frame_index][cascade]->writeToBuffer(&cascade_ubo);
		}
		m_force_full_rerender = false;
	}

	// non cascade shadow lights
	for (uint32_t shadow_idx = 0; shadow_idx < ubo.num_shadow_lights && shadow_idx < MAX_SHADOW_LIGHTS; shadow_idx++) {
		uint32_t layer = NUM_CSM_CASCADES + shadow_idx;
		m_light_views[frame_index][csm_data.active_cascade_count + shadow_idx] = ubo.shadow_lights[shadow_idx].light_view;
		m_light_projs[frame_index][csm_data.active_cascade_count + shadow_idx] = ubo.shadow_lights[shadow_idx].light_proj;

		ShadowPassUBO shadow_ubo{};
		shadow_ubo.view = ubo.shadow_lights[shadow_idx].light_view;
		shadow_ubo.proj = ubo.shadow_lights[shadow_idx].light_proj;
		shadow_ubo.projection_view = shadow_ubo.proj * shadow_ubo.view;

		// Update main UBO with shadow matrix (used by PBR/simple shaders for point light shadows)
		ubo.shadow_lights[shadow_idx].shadow_matrix = makeAtlasBias(layer) * shadow_ubo.projection_view;

		// Atlas bounds for XY clamping in fragment shader (prevents sampling adjacent regions)
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
	m_shadow_drawables_dirty = true;
	m_force_full_rerender = true;
	m_cached_unique_meshes.clear();
}

void ShadowRenderSystem::subscribeToRegistry(Registry& registry) {
	invalidateShadowDrawables();

	registry.events().subscribe<ComponentAddedEvent<MeshComponent>>(
		[this](const ComponentAddedEvent<MeshComponent>&) {
			invalidateShadowDrawables();
		});
	registry.events().subscribe<ComponentRemovedEvent<MeshComponent>>(
		[this](const ComponentRemovedEvent<MeshComponent>&) {
			invalidateShadowDrawables();
		});
	registry.events().subscribe<EntityDestroyedEvent>(
		[this](const EntityDestroyedEvent&) {
			m_shadow_drawables_dirty = true;
		});
	registry.events().subscribe<MeshDataChangedEvent>(
		[this](const MeshDataChangedEvent&) {
			invalidateShadowDrawables();
		});
}

void ShadowRenderSystem::growShadowInstanceBuffers(uint32_t new_capacity) {
	VE_LOGI("Shadow instance buffer growing: " << m_shadow_instance_capacity << " -> " << new_capacity);
	m_shadow_instance_capacity = new_capacity;
	createShadowInstanceBuffers();
	createShadowPassDescriptorSets(m_descriptor_pool);
}

void ShadowRenderSystem::rebuildMegaBuffers(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& unique_meshes) {
	uint32_t total_vertices = 0;
	uint32_t total_indices = 0;
	for (VeMesh* mesh : unique_meshes) {
		total_vertices += mesh->getVertexCount();
		// Sum indices for all LOD levels (LOD 0 = base, LOD 1+ = simplified)
		for (uint32_t lod = 0; lod < mesh->getLodCount(); lod++)
			total_indices += mesh->getLodIndexCount(lod);
	}

	if (total_vertices == 0 || total_indices == 0) {
		m_mega_entries.clear();
		m_mega_shadow_vbo.reset();
		m_mega_ibo.reset();
		m_mega_total_vertices = 0;
		m_mega_total_indices = 0;
		return;
	}

	// (Re)create mega-buffers if size changed
	if (total_vertices != m_mega_total_vertices || !m_mega_shadow_vbo) {
		m_mega_shadow_vbo = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(glm::vec3),
			total_vertices,
			vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);
		m_mega_total_vertices = total_vertices;
	}
	if (total_indices != m_mega_total_indices || !m_mega_ibo) {
		m_mega_ibo = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(uint32_t),
			total_indices,
			vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);
		m_mega_total_indices = total_indices;
	}

	// Record copies into the frame command buffer (caller adds transfer vertex-input barrier)
	m_mega_entries.clear();
	uint32_t vertex_offset = 0;
	uint32_t index_offset = 0;

	for (VeMesh* mesh : unique_meshes) {
		uint32_t vc = mesh->getVertexCount();

		// Copy shadow VBO region (shared across all LODs)
		vk::BufferCopy vbo_copy{
			.srcOffset = 0,
			.dstOffset = static_cast<vk::DeviceSize>(vertex_offset) * sizeof(glm::vec3),
			.size = static_cast<vk::DeviceSize>(vc) * sizeof(glm::vec3)
		};
		cmd.copyBuffer(mesh->getShadowVertexBuffer().getBuffer(),
		               m_mega_shadow_vbo->getBuffer(), vbo_copy);

		// Copy all LOD index buffers into mega-IBO
		MeshMegaEntry mega_entry;
		mega_entry.vertex_offset = vertex_offset;
		mega_entry.lod_entries.reserve(mesh->getLodCount());

		for (uint32_t lod = 0; lod < mesh->getLodCount(); lod++) {
			uint32_t ic = mesh->getLodIndexCount(lod);
			vk::BufferCopy ibo_copy{
				.srcOffset = 0,
				.dstOffset = static_cast<vk::DeviceSize>(index_offset) * sizeof(uint32_t),
				.size = static_cast<vk::DeviceSize>(ic) * sizeof(uint32_t)
			};
			cmd.copyBuffer(mesh->getLodIndexBuffer(lod).getBuffer(),
			               m_mega_ibo->getBuffer(), ibo_copy);

			mega_entry.lod_entries.push_back({index_offset, ic});
			index_offset += ic;
		}

		m_mega_entries[mesh] = std::move(mega_entry);
		vertex_offset += vc;
	}

	VE_LOGD("Shadow mega-buffer: " << unique_meshes.size() << " unique meshes, " << m_shadow_drawables.size() << " drawables, "
	         << total_vertices << " verts, " << total_indices << " indices");
}

void ShadowRenderSystem::renderShadowMaps(VeFrameInfo& frame_info) {
	const auto& light_views = m_light_views[frame_info.current_frame];

	if (light_views.empty()) return;
	assert(light_views.size() <= MAX_SHADOW_LAYERS && "Active shadow layers exceed MAX_SHADOW_LAYERS");

	// Build shadow draw list only when dirty (shared by all layers)
	if (m_shadow_drawables_dirty) {
		m_shadow_drawables.clear();
		auto& registry = *frame_info.registry;
		auto view = registry.view<MeshComponent, TransformComponent>();
		m_shadow_drawables.reserve(view.sizeHint());
		for (auto [entity, mesh, tc] : view) {
			if (!mesh.getMesh() || !mesh.has_shadow)
				continue;
			// Shadow LOD: at least one step coarser than the visible mesh, clamped to available LODs
			uint32_t shadow_lod = std::min(std::max(1u, mesh.cached_lod + 1),
			                               mesh.getMesh()->getLodCount() - 1);
			m_shadow_drawables.push_back({entity, &mesh, shadow_lod});
		}

		// Sort by (mesh pointer, lod_level) for instanced batching
		std::sort(m_shadow_drawables.begin(), m_shadow_drawables.end(),
			[](const ShadowDrawable& a, const ShadowDrawable& b) {
				VeMesh* ma = a.mesh->getMesh();
				VeMesh* mb = b.mesh->getMesh();
				if (ma != mb)
					return ma < mb;
				return a.lod_level < b.lod_level;
			});

		m_shadow_drawables_dirty = false;

		// Extract sorted unique meshes and rebuild mega-buffers only if the mesh set changed
		std::vector<VeMesh*> current_unique_meshes;
		current_unique_meshes.reserve(m_shadow_drawables.size());
		for (const auto& d : m_shadow_drawables) {
			VeMesh* m = d.mesh->getMesh();
			if (current_unique_meshes.empty() || current_unique_meshes.back() != m)
				current_unique_meshes.push_back(m);
		}

		if (current_unique_meshes != m_cached_unique_meshes) {
			m_cached_unique_meshes = std::move(current_unique_meshes);
			rebuildMegaBuffers(frame_info.cmd(), m_cached_unique_meshes);

			// Barrier: transfer writes must complete before vertex/index reads
			vk::MemoryBarrier2 transfer_barrier{
				.sType = vk::StructureType::eMemoryBarrier2,
				.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
				.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
				.dstStageMask = vk::PipelineStageFlagBits2::eVertexInput,
				.dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eIndexRead,
			};
			vk::DependencyInfo transfer_dep{
				.sType = vk::StructureType::eDependencyInfo,
				.memoryBarrierCount = 1,
				.pMemoryBarriers = &transfer_barrier,
			};
			frame_info.cmd().pipelineBarrier2(transfer_dep);
		}
	}

	auto& registry = *frame_info.registry;
	uint32_t csm_count = frame_info.csm_data.active_cascade_count;

	// Grow shadow instance buffer if needed (worst case: all drawables for CSM + all for point lights)
	uint32_t max_needed = static_cast<uint32_t>(m_shadow_drawables.size()) * 2;
	if (max_needed > m_shadow_instance_capacity) {
		growShadowInstanceBuffers(std::max(max_needed, m_shadow_instance_capacity * 2));
	}

	// Shadow SSBO layout: [CSM instances 0..N-1] [Point light instances N..M-1]
	// CSM instance groups reference indices 0..N-1
	// Point light instance groups reference indices N..M-1
	// Both share m_shadow_instance_buffers; CSM is written first.
	auto* shadow_instance_data = static_cast<InstanceData*>(
		m_shadow_instance_buffers[frame_info.current_frame]->getMappedMemory());
	uint32_t shadow_instance_count = 0;

	// CSM: write instances culled against outermost cascade, grouped by mesh for instancing
	m_csm_instance_groups.clear();
	if (csm_count > 0) {
		glm::mat4 outer_vp = m_light_projs[frame_info.current_frame][csm_count - 1]
		                    * m_light_views[frame_info.current_frame][csm_count - 1];
		FrustumPlane planes[6];
		extractFrustumPlanes(outer_vp, planes);

		for (auto& d : m_shadow_drawables) {
			const VeMesh::AABB& aabb = d.mesh->getWorldAABB();
			if (!isAABBInFrustum(aabb, planes))
				continue;

			if (shadow_instance_count >= m_shadow_instance_capacity) {
				VE_LOGW("Shadow instance buffer full, skipping remaining CSM objects");
				break;
			}
			uint32_t idx = shadow_instance_count++;
			shadow_instance_data[idx].transform = registry.getWorldTransform(d.entity);
			shadow_instance_data[idx].normal_transform[0] = glm::vec4(0.0f);
			shadow_instance_data[idx].normal_transform[1] = glm::vec4(0.0f);
			shadow_instance_data[idx].normal_transform[2] = glm::vec4(0.0f);

			VeMesh* mesh_ptr = d.mesh->getMesh();
			if (!m_csm_instance_groups.empty()
				&& m_csm_instance_groups.back().mesh == mesh_ptr
				&& m_csm_instance_groups.back().lod_level == d.lod_level) {
				m_csm_instance_groups.back().instance_count++;
			} else {
				m_csm_instance_groups.push_back({mesh_ptr, d.lod_level, idx, 1});
			}
		}
	}

	// --- Point light shadows: un-culled (wide FOV, culling cost > savings) ---
	m_shadow_instance_groups.clear();
	for (auto& d : m_shadow_drawables) {
		if (shadow_instance_count >= m_shadow_instance_capacity) {
			VE_LOGW("Shadow instance buffer full, skipping remaining point light shadow objects");
			break;
		}
		uint32_t idx = shadow_instance_count++;
		shadow_instance_data[idx].transform = registry.getWorldTransform(d.entity);
		shadow_instance_data[idx].normal_transform[0] = glm::vec4(0.0f);
		shadow_instance_data[idx].normal_transform[1] = glm::vec4(0.0f);
		shadow_instance_data[idx].normal_transform[2] = glm::vec4(0.0f);

		VeMesh* mesh_ptr = d.mesh->getMesh();
		if (!m_shadow_instance_groups.empty()
			&& m_shadow_instance_groups.back().mesh == mesh_ptr
			&& m_shadow_instance_groups.back().lod_level == d.lod_level) {
			m_shadow_instance_groups.back().instance_count++;
		} else {
			m_shadow_instance_groups.push_back({mesh_ptr, d.lod_level, idx, 1});
		}
	}

	auto& command_buffer = frame_info.cmd();

	// transition entire atlas to depth-attachment
	vk::ImageMemoryBarrier2 pre_barrier{
		.sType = vk::StructureType::eImageMemoryBarrier2,
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader
			| vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_shadow_atlas->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
	};
	vk::DependencyInfo pre_dep{
		.sType = vk::StructureType::eDependencyInfo,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &pre_barrier
	};
	command_buffer.pipelineBarrier2(pre_dep);

	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	command_buffer.setDepthBias(1.25f, 0.0f, 1.75f);

	if (m_mega_shadow_vbo && m_mega_ibo) {
		command_buffer.bindVertexBuffers(0, {m_mega_shadow_vbo->getBuffer()}, {vk::DeviceSize{0}});
		command_buffer.bindIndexBuffer(m_mega_ibo->getBuffer(), 0, vk::IndexType::eUint32);
	}

	// Render CSM cascades
	if (csm_count > 0 && !m_csm_instance_groups.empty()) {
		for (uint32_t c = 0; c < csm_count; c++) {
			if (c < NUM_CSM_CASCADES && !m_cascade_state[c].dirty)
				continue;

			auto& region = m_atlas_regions[c];
			vk::Extent2D extent{region.resolution, region.resolution};

			vk::RenderingAttachmentInfo depth_attachment{
				.imageView = *m_shadow_atlas->getImageView(),
				.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.loadOp = vk::AttachmentLoadOp::eClear,
				.storeOp = vk::AttachmentStoreOp::eStore,
				.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
			};
			vk::RenderingInfo rendering_info{
				.renderArea = {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent},
				.layerCount = 1,
				.colorAttachmentCount = 0,
				.pColorAttachments = nullptr,
				.pDepthAttachment = &depth_attachment
			};

			command_buffer.beginRendering(rendering_info);
			command_buffer.setViewport(0, vk::Viewport{
				.x = static_cast<float>(region.x), .y = static_cast<float>(region.y),
				.width = static_cast<float>(region.resolution),
				.height = static_cast<float>(region.resolution),
				.minDepth = 0.0f, .maxDepth = 1.0f
			});
			command_buffer.setScissor(0, vk::Rect2D{
				.offset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)},
				.extent = extent
			});
			renderShadowMap(frame_info, c, m_csm_instance_groups);
			command_buffer.endRendering();
		}
	}

	// Point/spot light shadows (per-light with atlas region viewport)
	if (!m_shadow_instance_groups.empty()) {
		for (size_t i = csm_count; i < light_views.size(); i++) {
			uint32_t layer = NUM_CSM_CASCADES + static_cast<uint32_t>(i - csm_count);
			assert(layer < MAX_SHADOW_LAYERS);
			auto& region = m_atlas_regions[layer];
			vk::Extent2D extent{region.resolution, region.resolution};

			vk::RenderingAttachmentInfo depth_attachment{
				.imageView = *m_shadow_atlas->getImageView(),
				.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.loadOp = vk::AttachmentLoadOp::eClear,
				.storeOp = vk::AttachmentStoreOp::eStore,
				.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
			};
			vk::RenderingInfo rendering_info{
				.renderArea = {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent},
				.layerCount = 1,
				.colorAttachmentCount = 0,
				.pColorAttachments = nullptr,
				.pDepthAttachment = &depth_attachment
			};

			command_buffer.beginRendering(rendering_info);
			command_buffer.setViewport(0, vk::Viewport{
				.x = static_cast<float>(region.x), .y = static_cast<float>(region.y),
				.width = static_cast<float>(region.resolution),
				.height = static_cast<float>(region.resolution),
				.minDepth = 0.0f, .maxDepth = 1.0f
			});
			command_buffer.setScissor(0, vk::Rect2D{
				.offset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)},
				.extent = extent
			});

			renderShadowMap(frame_info, layer, m_shadow_instance_groups);
			command_buffer.endRendering();
		}
	}

	// Single post-barrier: transition entire atlas back to shader-read
	vk::ImageMemoryBarrier2 post_barrier{
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
	vk::DependencyInfo post_dep{
		.sType = vk::StructureType::eDependencyInfo,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &post_barrier
	};
	command_buffer.pipelineBarrier2(post_dep);
}

void ShadowRenderSystem::renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index,
                                          const std::vector<ShadowInstanceGroup>& instance_groups) const {

	const vk::raii::DescriptorSet& shadow_global_set = m_shadow_global_descriptor_sets[frame_info.current_frame][light_index];

	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*shadow_global_set},
		{}
	);

	if (m_mega_shadow_vbo && m_mega_ibo && !m_mega_entries.empty()) {
		// Mega-buffer path: single VBO/IBO bind, per-group drawIndexed with push constants
		frame_info.cmd().bindVertexBuffers(0, {m_mega_shadow_vbo->getBuffer()}, {vk::DeviceSize{0}});
		frame_info.cmd().bindIndexBuffer(m_mega_ibo->getBuffer(), 0, vk::IndexType::eUint32);

		for (const auto& group : instance_groups) {
			auto it = m_mega_entries.find(group.mesh);
			if (it == m_mega_entries.end())
				continue;
			const auto& mega = it->second;
			uint32_t lod = std::min(group.lod_level, static_cast<uint32_t>(mega.lod_entries.size()) - 1);
			const auto& lod_entry = mega.lod_entries[lod];

			ShadowPushConstantData push{};
			push.instance_offset = group.first_instance;
			frame_info.cmd().pushConstants(
				*m_pipeline_layout,
				vk::ShaderStageFlagBits::eVertex,
				0,
				vk::ArrayProxy<const uint8_t>(sizeof(ShadowPushConstantData), reinterpret_cast<const uint8_t*>(&push))
			);

			frame_info.cmd().drawIndexed(
				lod_entry.index_count, group.instance_count,
				lod_entry.first_index, static_cast<int32_t>(mega.vertex_offset), 0);
		}
	}
}

void ShadowRenderSystem::createGpuShadowDescriptorSets(GpuCullingSystem& gpu_cull_system) {
	// Per-cascade descriptor sets: binding 0 = per-cascade ShadowPassUBO, binding 1 = cascade instance buffer
	m_csm_cascade_ubos.resize(MAX_FRAMES_IN_FLIGHT);
	m_gpu_cascade_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);

	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_csm_cascade_ubos[frame].clear();
		m_gpu_cascade_descriptor_sets[frame].clear();

		for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
			m_csm_cascade_ubos[frame].emplace_back(std::make_unique<VeBuffer>(
				m_ve_device, sizeof(ShadowPassUBO), 1,
				vk::BufferUsageFlagBits::eUniformBuffer,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
				m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
			));
			m_csm_cascade_ubos[frame].back()->map();

			vk::DescriptorBufferInfo cascade_ubo_info{
				.buffer = m_csm_cascade_ubos[frame][cascade]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};
			auto instance_info = gpu_cull_system.getShadowInstanceBuffer(
				static_cast<uint32_t>(frame), cascade).getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, m_descriptor_pool)
				.writeBuffer(0, &cascade_ubo_info)
				.writeBuffer(1, &instance_info)
				.build(ds);
			m_gpu_cascade_descriptor_sets[frame].push_back(std::move(ds));
		}
	}

	// Point/spot light descriptor sets: binding 0 = per-light UBO, binding 1 = per-light instance buffer
	m_gpu_shadow_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_gpu_shadow_descriptor_sets[frame].clear();

		for (uint32_t light = 0; light < MAX_SHADOW_LIGHTS; light++) {
			uint32_t layer = NUM_CSM_CASCADES + light; // UBO index
			uint32_t slot  = layer;                    // shadow_buf_index = array_layer

			vk::DescriptorBufferInfo buffer_info{
				.buffer = m_shadow_ubos[frame][layer]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};
			auto instance_info = gpu_cull_system.getShadowInstanceBuffer(
				static_cast<uint32_t>(frame), slot).getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, m_descriptor_pool)
				.writeBuffer(0, &buffer_info)
				.writeBuffer(1, &instance_info)
				.build(ds);
			m_gpu_shadow_descriptor_sets[frame].push_back(std::move(ds));
		}
	}
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

	const VeCamera* cam_for_hiz = gpu_cull_system.isHizEnabled() ? &frame_info.camera : nullptr;

	// Dispatch shadow culls
	for (uint32_t c = 0; c < csm_count; c++) {
		if (c < NUM_CSM_CASCADES && !m_cascade_state[c].dirty)
			continue;
		glm::mat4 cascade_vp = m_light_projs[frame][c] * m_light_views[frame][c];
		gpu_cull_system.dispatchShadowCull(cmd, cascade_vp, scene_mgr, frame, c,
		                            static_cast<int32_t>(c) + 1, cam_for_hiz);
	}
	for (uint32_t i = 0; i < num_shadow_lights; i++) {
		uint32_t slot = NUM_CSM_CASCADES + i;
		glm::mat4 light_vp = m_light_projs[frame][csm_count + i] * m_light_views[frame][csm_count + i];
		gpu_cull_system.dispatchShadowCull(cmd, light_vp, scene_mgr, frame, slot, 2, cam_for_hiz);
	}

	gpu_cull_system.flushShadowCullBarrier(cmd, scene_mgr, frame);

	// Single pre-barrier: transition entire atlas to depth-attachment
	vk::ImageMemoryBarrier2 pre_barrier{
		.sType = vk::StructureType::eImageMemoryBarrier2,
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader
			| vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_shadow_atlas->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
	};
	vk::DependencyInfo pre_dep{
		.sType = vk::StructureType::eDependencyInfo,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &pre_barrier
	};
	cmd.pipelineBarrier2(pre_dep);

	ShadowPushConstantData push{.instance_offset = 0};

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	cmd.setDepthBias(1.25f, 0.0f, 1.75f);
	mega_buffer.bindShadow(cmd);
	cmd.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
		vk::ArrayProxy<const uint8_t>(sizeof(push), reinterpret_cast<const uint8_t*>(&push)));

	// Render CSM cascades (skip clean cascades)
	for (uint32_t c = 0; c < csm_count; c++) {
		if (c < NUM_CSM_CASCADES && !m_cascade_state[c].dirty)
			continue;

		auto& region = m_atlas_regions[c];
		vk::Extent2D extent{region.resolution, region.resolution};

		vk::RenderingAttachmentInfo depth_attachment{
			.imageView = *m_shadow_atlas->getImageView(),
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
		};
		vk::RenderingInfo rendering_info{
			.renderArea = {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent},
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
		cmd.setScissor(0, vk::Rect2D{
			{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent});
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			0, {*m_gpu_cascade_descriptor_sets[frame][c]}, {});

		auto& shadow_indirect = gpu_cull_system.getShadowIndirectBuffer(frame, c);
		for (uint32_t bucket = 0; bucket < GpuCullingSystem::BUCKET_COUNT; bucket++) {
			uint32_t group_count = scene_mgr.getBucketGroupCount(bucket);
			if (group_count == 0) continue;
			auto offset = static_cast<vk::DeviceSize>(
				scene_mgr.getBucketGroupOffset(bucket)) * sizeof(VkDrawIndexedIndirectCommand);
			if (gpu_cull_system.compactionEnabled()) {
				cmd.drawIndexedIndirectCount(
					gpu_cull_system.getShadowCompactedIndirectBuffer(frame, c).getBuffer(), offset,
					gpu_cull_system.getShadowCompactCountBuffer(frame, c).getBuffer(),
					bucket * sizeof(uint32_t),
					group_count, sizeof(VkDrawIndexedIndirectCommand));
			} else {
				cmd.drawIndexedIndirect(shadow_indirect.getBuffer(), offset,
					group_count, sizeof(VkDrawIndexedIndirectCommand));
			}
		}
		cmd.endRendering();
	}

	// Render shadow maps for Point/Spot lights
	for (uint32_t i = 0; i < num_shadow_lights; i++) {
		uint32_t slot = NUM_CSM_CASCADES + i;
		auto& region = m_atlas_regions[slot];
		vk::Extent2D extent{region.resolution, region.resolution};

		vk::RenderingAttachmentInfo depth_attachment{
			.imageView = *m_shadow_atlas->getImageView(),
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
		};
		vk::RenderingInfo shadow_rendering_info{
			.renderArea = {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent},
			.layerCount = 1,
			.colorAttachmentCount = 0,
			.pColorAttachments = nullptr,
			.pDepthAttachment = &depth_attachment
		};
		cmd.beginRendering(shadow_rendering_info);
		cmd.setViewport(0, vk::Viewport{
			static_cast<float>(region.x), static_cast<float>(region.y),
			static_cast<float>(region.resolution), static_cast<float>(region.resolution),
			0.0f, 1.0f});
		cmd.setScissor(0, vk::Rect2D{
			{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent});
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			0, {*m_gpu_shadow_descriptor_sets[frame][i]}, {});

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
		cmd.endRendering();
	}

	// Single post-barrier: transition entire atlas back to shader-read
	vk::ImageMemoryBarrier2 post_barrier{
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
	vk::DependencyInfo post_dep{
		.sType = vk::StructureType::eDependencyInfo,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &post_barrier
	};
	cmd.pipelineBarrier2(post_dep);
}

void ShadowRenderSystem::createMeshletShadowDescriptorSets(MeshletCullingSystem& meshlet_cull) {
	// Per-cascade descriptor sets: binding 0 = per-cascade ShadowPassUBO, binding 1 = meshlet instance buffer
	// Reuse m_csm_cascade_ubos created by createGpuShadowDescriptorSets (or create if not yet present)
	if (m_csm_cascade_ubos.empty()) {
		m_csm_cascade_ubos.resize(MAX_FRAMES_IN_FLIGHT);
		for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
			m_csm_cascade_ubos[frame].clear();
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

	m_meshlet_cascade_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_meshlet_cascade_descriptor_sets[frame].clear();
		for (uint32_t cascade = 0; cascade < NUM_CSM_CASCADES; cascade++) {
			vk::DescriptorBufferInfo cascade_ubo_info{
				.buffer = m_csm_cascade_ubos[frame][cascade]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};
			auto instance_info = meshlet_cull.getShadowInstanceBuffer(
				static_cast<uint32_t>(frame), cascade).getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, m_descriptor_pool)
				.writeBuffer(0, &cascade_ubo_info)
				.writeBuffer(1, &instance_info)
				.build(ds);
			m_meshlet_cascade_descriptor_sets[frame].push_back(std::move(ds));
		}
	}

	// Point/spot light descriptor sets
	m_meshlet_shadow_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_meshlet_shadow_descriptor_sets[frame].clear();
		for (uint32_t light = 0; light < MAX_SHADOW_LIGHTS; light++) {
			uint32_t layer = NUM_CSM_CASCADES + light;
			uint32_t slot  = layer;

			vk::DescriptorBufferInfo buffer_info{
				.buffer = m_shadow_ubos[frame][layer]->getBuffer(),
				.offset = 0,
				.range = sizeof(ShadowPassUBO)
			};
			auto instance_info = meshlet_cull.getShadowInstanceBuffer(
				static_cast<uint32_t>(frame), slot).getDescriptorInfo();

			vk::raii::DescriptorSet ds{nullptr};
			VeDescriptorWriter(*m_shadow_global_set_layout, m_descriptor_pool)
				.writeBuffer(0, &buffer_info)
				.writeBuffer(1, &instance_info)
				.build(ds);
			m_meshlet_shadow_descriptor_sets[frame].push_back(std::move(ds));
		}
	}
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

	// Build batched shadow cull requests
	std::vector<MeshletCullingSystem::ShadowCullRequest> requests;
	requests.reserve(csm_count + num_shadow_lights);
	for (uint32_t c = 0; c < csm_count; c++) {
		if (c < NUM_CSM_CASCADES && !m_cascade_state[c].dirty)
			continue;
		const glm::mat4& lv = m_light_views[frame][c];
		glm::vec3 light_dir = -glm::vec3(lv[0][2], lv[1][2], lv[2][2]);
		requests.push_back({
			.view_proj = m_light_projs[frame][c] * lv,
			.light_pos = -light_dir * 100000.0f,
			.slot      = c,
			.lod_bias  = static_cast<int32_t>(c) + 1,
		});
	}
	for (uint32_t i = 0; i < num_shadow_lights; i++) {
		const glm::mat4& lv = m_light_views[frame][csm_count + i];
		requests.push_back({
			.view_proj = m_light_projs[frame][csm_count + i] * lv,
			.light_pos = -glm::transpose(glm::mat3(lv)) * glm::vec3(lv[3]),
			.slot      = NUM_CSM_CASCADES + i,
			.lod_bias  = 2,
		});
	}

	meshlet_cull.dispatchShadowCulls(cmd, requests.data(),
		static_cast<uint32_t>(requests.size()), scene_mgr, frame);

	// Single pre-barrier: transition entire atlas to depth-attachment
	vk::ImageMemoryBarrier2 pre_barrier{
		.sType = vk::StructureType::eImageMemoryBarrier2,
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader
			| vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_shadow_atlas->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}
	};
	vk::DependencyInfo pre_dep{
		.sType = vk::StructureType::eDependencyInfo,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &pre_barrier
	};
	cmd.pipelineBarrier2(pre_dep);

	ShadowPushConstantData push{.instance_offset = 0};
	constexpr uint32_t SHADOW_MAX_PER_BUCKET = MAX_MESHLET_SHADOW_DRAWS / MESHLET_SHADOW_BUCKET_COUNT;

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	cmd.setDepthBias(1.25f, 0.0f, 1.75f);
	mega_buffer.bindShadowMeshletIbo(cmd);
	cmd.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
		vk::ArrayProxy<const uint8_t>(sizeof(push), reinterpret_cast<const uint8_t*>(&push)));

	// Render CSM cascades (skip clean cascades)
	for (uint32_t c = 0; c < csm_count; c++) {
		if (c < NUM_CSM_CASCADES && !m_cascade_state[c].dirty)
			continue;

		auto& region = m_atlas_regions[c];
		vk::Extent2D extent{region.resolution, region.resolution};

		vk::RenderingAttachmentInfo depth_attachment{
			.imageView = *m_shadow_atlas->getImageView(),
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
		};
		vk::RenderingInfo rendering_info{
			.renderArea = {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent},
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
		cmd.setScissor(0, vk::Rect2D{
			{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent});
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			0, {*m_meshlet_cascade_descriptor_sets[frame][c]}, {});

		auto& shadow_indirect = meshlet_cull.getShadowMeshletIndirectBuffer(frame, c);
		auto& shadow_draw_counts = meshlet_cull.getShadowMeshletDrawCounts(frame, c);
		const uint32_t* cpu_counts = meshlet_cull.getShadowCpuDrawCounts(c);
		for (uint32_t bucket = 0; bucket < MESHLET_SHADOW_BUCKET_COUNT; bucket++) {
			auto buf_offset   = static_cast<vk::DeviceSize>(bucket) * SHADOW_MAX_PER_BUCKET
			                    * sizeof(VkDrawIndexedIndirectCommand);
			if (cpu_counts) {
				uint32_t count = std::min(cpu_counts[bucket], SHADOW_MAX_PER_BUCKET);
				cmd.drawIndexedIndirect(shadow_indirect.getBuffer(), buf_offset,
					count, sizeof(VkDrawIndexedIndirectCommand));
			} else {
				auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
				cmd.drawIndexedIndirectCount(
					shadow_indirect.getBuffer(), buf_offset,
					shadow_draw_counts.getBuffer(), count_offset,
					SHADOW_MAX_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
			}
		}
		cmd.endRendering();
	}

	// Render shadow maps for Point/Spot lights
	for (uint32_t i = 0; i < num_shadow_lights; i++) {
		uint32_t slot = NUM_CSM_CASCADES + i;
		auto& region = m_atlas_regions[slot];
		vk::Extent2D extent{region.resolution, region.resolution};

		vk::RenderingAttachmentInfo depth_attachment{
			.imageView = *m_shadow_atlas->getImageView(),
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearDepthStencilValue{.depth = 1.0f, .stencil = 0}
		};
		vk::RenderingInfo shadow_rendering_info{
			.renderArea = {{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent},
			.layerCount = 1,
			.colorAttachmentCount = 0,
			.pColorAttachments = nullptr,
			.pDepthAttachment = &depth_attachment
		};
		cmd.beginRendering(shadow_rendering_info);
		cmd.setViewport(0, vk::Viewport{
			static_cast<float>(region.x), static_cast<float>(region.y),
			static_cast<float>(region.resolution), static_cast<float>(region.resolution),
			0.0f, 1.0f});
		cmd.setScissor(0, vk::Rect2D{
			{static_cast<int32_t>(region.x), static_cast<int32_t>(region.y)}, extent});
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipeline_layout,
			0, {*m_meshlet_shadow_descriptor_sets[frame][i]}, {});

		auto& shadow_indirect = meshlet_cull.getShadowMeshletIndirectBuffer(frame, slot);
		auto& shadow_draw_counts = meshlet_cull.getShadowMeshletDrawCounts(frame, slot);
		const uint32_t* cpu_counts = meshlet_cull.getShadowCpuDrawCounts(slot);
		for (uint32_t bucket = 0; bucket < MESHLET_SHADOW_BUCKET_COUNT; bucket++) {
			auto buf_offset   = static_cast<vk::DeviceSize>(bucket) * SHADOW_MAX_PER_BUCKET
			                    * sizeof(VkDrawIndexedIndirectCommand);
			if (cpu_counts) {
				uint32_t count = std::min(cpu_counts[bucket], SHADOW_MAX_PER_BUCKET);
				cmd.drawIndexedIndirect(shadow_indirect.getBuffer(), buf_offset,
					count, sizeof(VkDrawIndexedIndirectCommand));
			} else {
				auto count_offset = static_cast<vk::DeviceSize>(bucket) * sizeof(uint32_t);
				cmd.drawIndexedIndirectCount(
					shadow_indirect.getBuffer(), buf_offset,
					shadow_draw_counts.getBuffer(), count_offset,
					SHADOW_MAX_PER_BUCKET, sizeof(VkDrawIndexedIndirectCommand));
			}
		}
		cmd.endRendering();
	}

	// Single post-barrier: transition entire atlas back to shader-read
	vk::ImageMemoryBarrier2 post_barrier{
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
	vk::DependencyInfo post_dep{
		.sType = vk::StructureType::eDependencyInfo,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &post_barrier
	};
	cmd.pipelineBarrier2(post_dep);
}

} // namespace ve
