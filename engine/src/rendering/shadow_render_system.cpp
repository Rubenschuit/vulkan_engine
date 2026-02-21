#include "pch.hpp"
#include "rendering/shadow_render_system.hpp"
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
	std::filesystem::path shader_path,
	std::filesystem::path csm_shader_path)
	: m_ve_device(device), m_descriptor_pool(descriptor_pool), m_shader_path(shader_path), m_csm_shader_path(csm_shader_path) {

	// Initialize 2D arrays for per-frame point light data
	m_shadow_ubos.resize(MAX_FRAMES_IN_FLIGHT);
	m_shadow_global_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_views.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_projs.resize(MAX_FRAMES_IN_FLIGHT);

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_ubos[i].resize(MAX_SHADOW_LAYERS);
		m_shadow_global_descriptor_sets[i].reserve(MAX_SHADOW_LAYERS);
	}

	// Create shadow resources (images, sampler, descriptor sets)
	createShadowResources();

	// Create shadow global descriptor set layout (for shadow pass UBO + instance SSBO)
	m_shadow_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.build();

	// Create shadow descriptor set layout (for sampling shadow maps)
	// eCompute needed so the screen-space shadow mask compute shader can read shadow maps
	m_shadow_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute)       // comparison sampler
		.addBinding(1, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute)   // shadow map array
		.addBinding(2, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute)       // raw sampler (PCSS)
		.build();

	createShadowUBOs();
	createShadowInstanceBuffers();
	createShadowPassDescriptorSets(descriptor_pool);
	createCsmDescriptorSets(descriptor_pool);
	createShadowTextureDescriptorSets(descriptor_pool);
	createPipelineLayout(material_set_layout);
	createPipeline(m_shadow_depth_format);
	createCsmPipeline(m_shadow_depth_format);
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

void ShadowRenderSystem::createCsmPipeline(vk::Format depth_format) {
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

	pipeline_config.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLess;

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	pipeline_config.view_mask = (1u << NUM_CSM_CASCADES) - 1;  // e.g. 0xF for 4 cascades

	m_csm_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_csm_shader_path,
		pipeline_config);
	assert(m_csm_pipeline != VK_NULL_HANDLE && "Failed to create CSM multiview pipeline");
}

void ShadowRenderSystem::createShadowUBOs() {
	// CSM: single CsmMultiviewUBO per frame (all cascades in one buffer)
	vk::DeviceSize csm_buffer_size = sizeof(CsmMultiviewUBO);
	m_csm_ubos.clear();
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_csm_ubos.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device,
			csm_buffer_size,
			1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
		));
		m_csm_ubos[frame]->map();
	}

	// Point light shadows: per-frame, per-point-light UBOs (layers NUM_CSM_CASCADES..MAX_SHADOW_LAYERS-1)
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
				.buffer = *m_shadow_ubos[frame][layer]->getBuffer(),
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

void ShadowRenderSystem::createCsmDescriptorSets(VeDescriptorPool& descriptor_pool) {
	// CSM multiview descriptor sets: per-frame
	// Each set: binding 0 = CsmMultiviewUBO, binding 1 = shadow instance SSBO
	m_csm_descriptor_sets.clear();
	m_csm_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);

	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::DescriptorBufferInfo csm_ubo_info{
			.buffer = *m_csm_ubos[frame]->getBuffer(),
			.offset = 0,
			.range = sizeof(CsmMultiviewUBO)
		};
		auto instance_info = m_shadow_instance_buffers[frame]->getDescriptorInfo();

		vk::raii::DescriptorSet descriptor_set{nullptr};
		VeDescriptorWriter(*m_shadow_global_set_layout, descriptor_pool)
			.writeBuffer(0, &csm_ubo_info)
			.writeBuffer(1, &instance_info)
			.build(descriptor_set);

		m_csm_descriptor_sets.push_back(std::move(descriptor_set));
	}
}

void ShadowRenderSystem::createShadowResources() {
	VE_LOGD("Shadow system: Creating shadow resources");

	// Find suitable depth format
	m_shadow_depth_format = m_ve_device.findSupportedFormat(
		{vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
		vk::ImageTiling::eOptimal,
		vk::FormatFeatureFlagBits::eDepthStencilAttachment | vk::FormatFeatureFlagBits::eSampledImage
	);

	// Create a single 2D array texture for all shadow maps (MAX_SHADOW_LAYERS layers)
	// Layers 0..NUM_CSM_CASCADES-1 = CSM cascades, NUM_CSM_CASCADES..MAX_SHADOW_LAYERS-1 = point lights
	m_shadow_map_array = std::make_unique<VeImage>(
		m_ve_device,
		SHADOW_MAP_RESOLUTION,
		SHADOW_MAP_RESOLUTION,
		vk::SampleCountFlagBits::e1,
		m_shadow_depth_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eDepth,
		false,  // not cubemap
		MAX_SHADOW_LAYERS  // array layers (CSM + point)
	);

	// Create individual layer views for point light shadow rendering
	m_shadow_map_layer_views.clear();
	m_shadow_map_layer_views.reserve(MAX_SHADOW_LAYERS);
	for (uint32_t i = 0; i < MAX_SHADOW_LAYERS; i++) {
		m_shadow_map_layer_views.push_back(m_shadow_map_array->createLayerImageView(i));
	}

	// Create multi-layer 2DArray view for CSM multiview rendering
	m_csm_multiview_image_view = m_shadow_map_array->createMultiLayerImageView(0, NUM_CSM_CASCADES);

	// Transition shadow maps
	m_shadow_map_array->transitionImageLayout(
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
		.imageView = *m_shadow_map_array->getImageView(),
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

	// Bias matrix for shadow mapping (converts NDC to texture coordinates)
	static const glm::mat4 bias_matrix(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f
	);

	// CSM cascades: write all cascade matrices into a single CsmMultiviewUBO
	if (csm_data.active_cascade_count > 0) {
		CsmMultiviewUBO csm_ubo{};
		for (uint32_t cascade = 0; cascade < csm_data.active_cascade_count; cascade++) {
			m_light_views[frame_index][cascade] = csm_data.light_view[cascade];
			m_light_projs[frame_index][cascade] = csm_data.light_proj[cascade];

			csm_ubo.view[cascade] = csm_data.light_view[cascade];
			csm_ubo.proj[cascade] = csm_data.light_proj[cascade];
			csm_ubo.projection_view[cascade] = csm_ubo.proj[cascade] * csm_ubo.view[cascade];
		}
		m_csm_ubos[frame_index]->writeToBuffer(&csm_ubo);
	}

	// Point light shadows: layers NUM_CSM_CASCADES..NUM_CSM_CASCADES+num_shadow_lights-1
	for (uint32_t shadow_idx = 0; shadow_idx < ubo.num_shadow_lights && shadow_idx < MAX_POINT_SHADOW_LIGHTS; shadow_idx++) {
		uint32_t layer = NUM_CSM_CASCADES + shadow_idx;
		m_light_views[frame_index][csm_data.active_cascade_count + shadow_idx] = ubo.shadow_lights[shadow_idx].light_view;
		m_light_projs[frame_index][csm_data.active_cascade_count + shadow_idx] = ubo.shadow_lights[shadow_idx].light_proj;

		ShadowPassUBO shadow_ubo{};
		shadow_ubo.view = ubo.shadow_lights[shadow_idx].light_view;
		shadow_ubo.proj = ubo.shadow_lights[shadow_idx].light_proj;
		shadow_ubo.projection_view = shadow_ubo.proj * shadow_ubo.view;

		// Update main UBO with shadow matrix (used by PBR/simple shaders for point light shadows)
		ubo.shadow_lights[shadow_idx].shadow_matrix = bias_matrix * shadow_ubo.projection_view;

		m_shadow_ubos[frame_index][layer]->writeToBuffer(&shadow_ubo);
	}
}

void ShadowRenderSystem::invalidateShadowDrawables() {
	m_shadow_drawables_dirty = true;
	m_cached_unique_meshes.clear();
}

void ShadowRenderSystem::growShadowInstanceBuffers(uint32_t new_capacity) {
	VE_LOGI("Shadow instance buffer growing: " << m_shadow_instance_capacity << " -> " << new_capacity);
	m_shadow_instance_capacity = new_capacity;
	createShadowInstanceBuffers();
	createShadowPassDescriptorSets(m_descriptor_pool);
	createCsmDescriptorSets(m_descriptor_pool);
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
		cmd.copyBuffer(*mesh->getShadowVertexBuffer().getBuffer(),
		               *m_mega_shadow_vbo->getBuffer(), vbo_copy);

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
			cmd.copyBuffer(*mesh->getLodIndexBuffer(lod).getBuffer(),
			               *m_mega_ibo->getBuffer(), ibo_copy);

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
		auto& mesh_pool = registry.meshes();
		m_shadow_drawables.reserve(mesh_pool.size());
		for (uint32_t i = 0; i < mesh_pool.size(); i++) {
			MeshComponent& mesh = mesh_pool.data()[i];
			if (!mesh.getMesh() || !mesh.has_shadow) continue;
			uint32_t entity_idx = mesh_pool.entityAt(i);
			Entity entity = registry.entityFromIndex(entity_idx);
			if (!registry.isActive(entity)) continue;
			auto* transform = registry.getComponent<TransformComponent>(entity);
			if (!transform) continue;
			// Shadow LOD: at least one step coarser than the visible mesh, clamped to available LODs
			uint32_t shadow_lod = std::min(std::max(1u, mesh.cached_lod),
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
			rebuildMegaBuffers(frame_info.command_buffer, m_cached_unique_meshes);

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
			frame_info.command_buffer.pipelineBarrier2(transfer_dep);
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
				m_csm_instance_groups.emplace_back(mesh_ptr, d.lod_level, idx, 1);
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
			m_shadow_instance_groups.emplace_back(mesh_ptr, d.lod_level, idx, 1);
		}
	}

	auto& command_buffer = frame_info.command_buffer;
	vk::Extent2D shadow_extent{SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION};

	// CSM multiview: single render pass for all cascades
	if (csm_count > 0 && !m_csm_instance_groups.empty()) {
		// Transition all CSM layers to depth-attachment in a single barrier
		vk::ImageMemoryBarrier2 csm_pre_barrier{
			.sType = vk::StructureType::eImageMemoryBarrier2,
			.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
			.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
			.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = *m_shadow_map_array->getImage(),
			.subresourceRange = {
				.aspectMask = vk::ImageAspectFlagBits::eDepth,
				.baseMipLevel = 0, .levelCount = 1,
				.baseArrayLayer = 0, .layerCount = csm_count
			}
		};
		vk::DependencyInfo pre_dep{
			.sType = vk::StructureType::eDependencyInfo,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &csm_pre_barrier
		};
		command_buffer.pipelineBarrier2(pre_dep);

		vk::RenderingAttachmentInfo depth_attachment{
			.imageView = *m_csm_multiview_image_view,  // 2DArray view of layers 0..NUM_CSM_CASCADES-1
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearDepthStencilValue(1.0f, 0)
		};
		vk::RenderingInfo csm_rendering_info{
			.renderArea = { {0, 0}, shadow_extent },
			.layerCount = csm_count,
			.viewMask = (1u << csm_count) - 1,
			.colorAttachmentCount = 0,
			.pColorAttachments = nullptr,
			.pDepthAttachment = &depth_attachment
		};

		command_buffer.beginRendering(csm_rendering_info);
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(SHADOW_MAP_RESOLUTION),
			.height = static_cast<float>(SHADOW_MAP_RESOLUTION),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), shadow_extent));
		command_buffer.setDepthBias(1.25f, 0.0f, 1.75f);

		// Bind CSM multiview pipeline and descriptor set
		command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_csm_pipeline->getPipeline());
		command_buffer.bindDescriptorSets(
			vk::PipelineBindPoint::eGraphics,
			*m_pipeline_layout,
			0,
			{*m_csm_descriptor_sets[frame_info.current_frame]},
			{}
		);

		// Single bind of mega-buffers, per-group drawIndexed with push constants
		command_buffer.bindVertexBuffers(0, {*m_mega_shadow_vbo->getBuffer()}, {vk::DeviceSize{0}});
		command_buffer.bindIndexBuffer(*m_mega_ibo->getBuffer(), 0, vk::IndexType::eUint32);

		for (const auto& group : m_csm_instance_groups) {
			auto it = m_mega_entries.find(group.mesh);
			assert(it != m_mega_entries.end());
			const auto& mega = it->second;
			uint32_t lod = std::min(group.lod_level, static_cast<uint32_t>(mega.lod_entries.size()) - 1);
			const auto& lod_entry = mega.lod_entries[lod];

			ShadowPushConstantData push{};
			push.instance_offset = group.first_instance;
			command_buffer.pushConstants(
				*m_pipeline_layout,
				vk::ShaderStageFlagBits::eVertex,
				0,
				vk::ArrayProxy<const uint8_t>(sizeof(ShadowPushConstantData), reinterpret_cast<const uint8_t*>(&push))
			);

			command_buffer.drawIndexed(
				lod_entry.index_count, group.instance_count,
				lod_entry.first_index, static_cast<int32_t>(mega.vertex_offset), 0);
		}

		command_buffer.endRendering();

		// Transition all CSM layers back to shader-read
		vk::ImageMemoryBarrier2 csm_post_barrier{
			.sType = vk::StructureType::eImageMemoryBarrier2,
			.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
			.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = *m_shadow_map_array->getImage(),
			.subresourceRange = {
				.aspectMask = vk::ImageAspectFlagBits::eDepth,
				.baseMipLevel = 0, .levelCount = 1,
				.baseArrayLayer = 0, .layerCount = csm_count
			}
		};
		vk::DependencyInfo post_dep{
			.sType = vk::StructureType::eDependencyInfo,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &csm_post_barrier
		};
		command_buffer.pipelineBarrier2(post_dep);
	}

	// Point light shadows (no multiview)
	if (!m_shadow_instance_groups.empty()) {
		command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());

		for (size_t i = csm_count; i < light_views.size(); i++) {
			uint32_t array_layer = NUM_CSM_CASCADES + static_cast<uint32_t>(i - csm_count);

			assert(array_layer < MAX_SHADOW_LAYERS);
			assert(array_layer < m_shadow_map_layer_views.size());

			vk::RenderingAttachmentInfo depth_attachment{
				.imageView = *m_shadow_map_layer_views[array_layer],
				.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.loadOp = vk::AttachmentLoadOp::eClear,
				.storeOp = vk::AttachmentStoreOp::eStore,
				.clearValue = vk::ClearDepthStencilValue(1.0f, 0)
			};
			vk::RenderingInfo shadow_rendering_info{
				.renderArea = { {0, 0}, shadow_extent },
				.layerCount = 1,
				.colorAttachmentCount = 0,
				.pColorAttachments = nullptr,
				.pDepthAttachment = &depth_attachment
			};

			vk::ImageMemoryBarrier2 barrier{
				.sType = vk::StructureType::eImageMemoryBarrier2,
				.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
				.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
				.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
				.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
				.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
				.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = *m_shadow_map_array->getImage(),
				.subresourceRange = {
					.aspectMask = vk::ImageAspectFlagBits::eDepth,
					.baseMipLevel = 0, .levelCount = 1,
					.baseArrayLayer = array_layer, .layerCount = 1
				}
			};
			vk::DependencyInfo dep_info{
				.sType = vk::StructureType::eDependencyInfo,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &barrier
			};
			command_buffer.pipelineBarrier2(dep_info);

			command_buffer.beginRendering(shadow_rendering_info);
			command_buffer.setViewport(0, vk::Viewport{
				.x = 0.0f, .y = 0.0f,
				.width = static_cast<float>(SHADOW_MAP_RESOLUTION),
				.height = static_cast<float>(SHADOW_MAP_RESOLUTION),
				.minDepth = 0.0f, .maxDepth = 1.0f
			});
			command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), shadow_extent));
			command_buffer.setDepthBias(1.25f, 0.0f, 1.75f);

			renderShadowMap(frame_info, array_layer, m_shadow_instance_groups);

			command_buffer.endRendering();

			barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
			barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
			barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
			barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
			barrier.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
			barrier.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
			command_buffer.pipelineBarrier2(dep_info);
		}
	}
}

void ShadowRenderSystem::renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index,
                                          const std::vector<ShadowInstanceGroup>& instance_groups) const {

	const vk::raii::DescriptorSet& shadow_global_set = m_shadow_global_descriptor_sets[frame_info.current_frame][light_index];

	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*shadow_global_set},
		{}
	);

	if (m_mega_shadow_vbo && m_mega_ibo && !m_mega_entries.empty()) {
		// Mega-buffer path: single VBO/IBO bind, per-group drawIndexed with push constants
		frame_info.command_buffer.bindVertexBuffers(0, {*m_mega_shadow_vbo->getBuffer()}, {vk::DeviceSize{0}});
		frame_info.command_buffer.bindIndexBuffer(*m_mega_ibo->getBuffer(), 0, vk::IndexType::eUint32);

		for (const auto& group : instance_groups) {
			auto it = m_mega_entries.find(group.mesh);
			if (it == m_mega_entries.end())
				continue;
			const auto& mega = it->second;
			uint32_t lod = std::min(group.lod_level, static_cast<uint32_t>(mega.lod_entries.size()) - 1);
			const auto& lod_entry = mega.lod_entries[lod];

			ShadowPushConstantData push{};
			push.instance_offset = group.first_instance;
			frame_info.command_buffer.pushConstants(
				*m_pipeline_layout,
				vk::ShaderStageFlagBits::eVertex,
				0,
				vk::ArrayProxy<const uint8_t>(sizeof(ShadowPushConstantData), reinterpret_cast<const uint8_t*>(&push))
			);

			frame_info.command_buffer.drawIndexed(
				lod_entry.index_count, group.instance_count,
				lod_entry.first_index, static_cast<int32_t>(mega.vertex_offset), 0);
		}
	}
}

} // namespace ve
