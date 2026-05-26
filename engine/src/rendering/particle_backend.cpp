#include "pch.hpp"
#include "rendering/particle_backend.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"
#include <random>
#include <chrono>
#include <cassert>

namespace ve {

ParticleBackend::ParticleBackend(const ParticleBackendCreateInfo& info)
	: m_ve_device(info.device), m_capacity(info.capacity),
	  m_descriptor_pool(info.descriptor_pool),
	  m_default_atlas_handle(info.default_atlas),
	  m_shader_path(info.shader_path) {

	if (info.event_bus) {
		info.event_bus->subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
			recreatePipeline(e.offscreen_format, e.sample_count);
		});
	}

	VE_LOGI("ParticleBackend constructor: particles=" << m_capacity);
	m_pending_capacity = m_capacity;

	// Initialize emitter registry. Build the free list in reverse so registerEmitter
	// hands out low ids first.
	m_emitter_storage.resize(MAX_EMITTERS, EmitterParams{});
	m_emitter_used.resize(MAX_EMITTERS, 0);
	m_emitter_free_list.reserve(MAX_EMITTERS);
	for (uint32_t i = MAX_EMITTERS; i > 0; --i)
		m_emitter_free_list.push_back(i - 1);

	// Atlas slot free-list (reverse so low indices allocate first). All slots
	// start pointing at the default atlas; registerAtlas/releaseAtlas patch
	// individual slots after the initial descriptor set has been built.
	m_free_atlas_slots.reserve(MAX_PARTICLE_ATLASES);
	for (uint32_t i = MAX_PARTICLE_ATLASES; i > 0; --i)
		m_free_atlas_slots.push_back(i - 1);
	for (auto& slot : m_atlas_slots)
		slot = m_default_atlas_handle;

	createShaderStorageBuffers();
	createSpawnEventBuffers();
	createFreeListBuffers();
	createUniformBuffers();
	createEmitterParamsBuffers();
	createDescriptorSetLayouts();
	createDescriptorSets();
	createComputePipelineLayout();
	createComputePipeline();
	createSpawnComputePipeline();
	createRecycleComputePipeline();
	createPipelineLayout(info.global_set_layout, m_render_set_layout->getDescriptorSetLayout());
	createPipeline(info.color_format, info.sample_count);
	createRenderDescriptorSet();
}

void ParticleBackend::createRenderDescriptorSet() {
	std::array<vk::DescriptorImageInfo, MAX_PARTICLE_ATLASES> infos{};
	for (uint32_t i = 0; i < MAX_PARTICLE_ATLASES; ++i)
		infos[i] = m_atlas_slots[i].get()->getDescriptorInfo();
	VeDescriptorWriter(*m_render_set_layout, *m_descriptor_pool)
		.writeImageArray(0, infos.data(), MAX_PARTICLE_ATLASES)
		.build(m_render_descriptor_set);
}

void ParticleBackend::writeAtlasSlot(uint32_t slot) {
	assert(slot < MAX_PARTICLE_ATLASES);
	vk::DescriptorImageInfo info = m_atlas_slots[slot].get()->getDescriptorInfo();
	vk::WriteDescriptorSet write{
		.sType = vk::StructureType::eWriteDescriptorSet,
		.dstSet = *m_render_descriptor_set,
		.dstBinding = 0,
		.dstArrayElement = slot,
		.descriptorCount = 1,
		.descriptorType = vk::DescriptorType::eCombinedImageSampler,
		.pImageInfo = &info,
	};
	m_ve_device.getDevice().updateDescriptorSets(write, {});
}

uint32_t ParticleBackend::registerAtlas(ResourceHandle<VeTexture> atlas) {
	if (m_free_atlas_slots.empty()) {
		VE_LOGE("ParticleBackend::registerAtlas: out of atlas slots (MAX_PARTICLE_ATLASES=" << MAX_PARTICLE_ATLASES << "); falling back to procedural mask");
		return ROUND_MASK_SENTINEL;
	}
	uint32_t slot = m_free_atlas_slots.back();
	m_free_atlas_slots.pop_back();
	m_atlas_slots[slot] = std::move(atlas);
	writeAtlasSlot(slot);
	return slot;
}

void ParticleBackend::releaseAtlas(uint32_t slot) {
	if (slot >= MAX_PARTICLE_ATLASES) {
		VE_LOGW("ParticleBackend::releaseAtlas: invalid slot " << slot);
		return;
	}
	m_atlas_slots[slot] = m_default_atlas_handle;
	writeAtlasSlot(slot);
	m_free_atlas_slots.push_back(slot);
}

void ParticleBackend::setDefaultAtlas(ResourceHandle<VeTexture> atlas) {
	m_default_atlas_handle = std::move(atlas);
	// Repoint every free (= logically default) slot. Registered slots keep
	// their texture until released.
	for (uint32_t slot : m_free_atlas_slots) {
		m_atlas_slots[slot] = m_default_atlas_handle;
		writeAtlasSlot(slot);
	}
}

ParticleBackend::~ParticleBackend() {}

void ParticleBackend::scheduleRestart() {
	m_pending_reset = true;
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
	m_reset_seed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

EmitterId ParticleBackend::registerEmitter(const EmitterParams& params) {
	if (m_emitter_free_list.empty()) {
		VE_LOGE("ParticleBackend::registerEmitter: out of emitter slots (MAX_EMITTERS=" << MAX_EMITTERS << ")");
		return INVALID_EMITTER;
	}
	const EmitterId id = m_emitter_free_list.back();
	m_emitter_free_list.pop_back();
	m_emitter_storage[id] = params;
	m_emitter_used[id] = 1;
	return id;
}

void ParticleBackend::updateEmitter(EmitterId id, const EmitterParams& params) {
	if (id >= MAX_EMITTERS || !m_emitter_used[id]) {
		VE_LOGW("ParticleBackend::updateEmitter on unregistered id " << id);
		return;
	}
	m_emitter_storage[id] = params;
}

void ParticleBackend::releaseEmitter(EmitterId id) {
	if (id >= MAX_EMITTERS || !m_emitter_used[id]) {
		VE_LOGW("ParticleBackend::releaseEmitter on unregistered id " << id);
		return;
	}
	m_emitter_used[id] = 0;
	m_emitter_free_list.push_back(id);
}

const EmitterParams& ParticleBackend::getEmitterParams(EmitterId id) const {
	assert(id < MAX_EMITTERS && m_emitter_used[id] && "getEmitterParams on unregistered id");
	return m_emitter_storage[id];
}

EmitterParams& ParticleBackend::getEmitterParams(EmitterId id) {
	assert(id < MAX_EMITTERS && m_emitter_used[id] && "getEmitterParams on unregistered id");
	return m_emitter_storage[id];
}

void ParticleBackend::createShaderStorageBuffers() {

	vk::DeviceSize buffer_size = static_cast<vk::DeviceSize>(m_capacity) * sizeof(Particle);
	vk::DeviceSize render_buffer_size = static_cast<vk::DeviceSize>(m_capacity) * sizeof(RenderParticle);

	// Stamp FREE_LIST_SENTINEL into velocity.w so compMain's death branch
	// doesn't fire on every slot the first frame
	constexpr float FREE_LIST_SENTINEL = -1.0e9f;
	Particle free_slot_template{};
	free_slot_template.velocity.w = FREE_LIST_SENTINEL;
	std::vector<Particle> particles(m_capacity, free_slot_template);

	VeBuffer staging_buffer(
		m_ve_device,
		buffer_size,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(particles.data());

	m_shader_storage_buffers.clear();
	m_shader_storage_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	m_render_buffers.clear();
	m_render_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	m_indirect_buffers.clear();
	m_indirect_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	VkDrawIndirectCommand initial_command{
		.vertexCount = 6,
		.instanceCount = 0,
		.firstVertex = 0,
		.firstInstance = 0
	};

	vk::DeviceSize indirect_size = sizeof(VkDrawIndirectCommand);

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		m_shader_storage_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			buffer_size,
			1,
			vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eTransferDst |
			vk::BufferUsageFlagBits::eVertexBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);
		m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_shader_storage_buffers[i]->getBuffer(), buffer_size);

		m_render_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			render_buffer_size,
			1,
			vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eVertexBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);

		m_indirect_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			indirect_size,
			1,
			vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eIndirectBuffer |
			vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);
		m_indirect_buffers[i]->map();
		m_indirect_buffers[i]->writeToBuffer(&initial_command);
	}
}

// Per-frame host-visible SSBO carrying queued SpawnEvents for the spawn
// pre-pass. Sized by MAX_SPAWN_EVENTS, independent of particle capacity.
void ParticleBackend::createSpawnEventBuffers() {
	m_spawn_storage_buffers.clear();
	m_spawn_storage_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	vk::DeviceSize buffer_size = sizeof(SpawnEvent) * MAX_SPAWN_EVENTS;

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		m_spawn_storage_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			buffer_size,
			1,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);
		m_spawn_storage_buffers[i]->map();
	}
}

// GPU free list of unused particle slots, the counter pair that drives it,
// and the per-frame recycle queue that defers pushes onto it. Sized by
// m_capacity, so must be recreated on resize.
void ParticleBackend::createFreeListBuffers() {
	m_slot_pool_counters_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(uint32_t) * 2,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	// Seed counters: every active slot is on the free list, recycle queue empty.
	{
		uint32_t initial_counters[2] = {m_capacity, 0u};
		auto cmd = m_ve_device.beginSingleTimeCommands(QueueKind::Transfer);
		cmd->updateBuffer<uint32_t>(m_slot_pool_counters_buffer->getBuffer(), 0, initial_counters);
		m_ve_device.endSingleTimeCommands(*cmd, QueueKind::Transfer);
	}

	vk::DeviceSize free_slot_stack_size = sizeof(uint32_t) * m_capacity;
	m_free_slot_stack_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		free_slot_stack_size,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	m_free_slot_stack_staging_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		free_slot_stack_size,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	m_free_slot_stack_staging_buffer->map();
	{
		std::vector<uint32_t> sequence(m_capacity);
		for (uint32_t i = 0; i < m_capacity; ++i)
			sequence[i] = i;
		m_free_slot_stack_staging_buffer->writeToBuffer(sequence.data());
	}
	m_ve_device.copyBuffer(m_free_slot_stack_staging_buffer->getBuffer(), m_free_slot_stack_buffer->getBuffer(), free_slot_stack_size);

	vk::DeviceSize recycle_queue_size = sizeof(uint32_t) * m_capacity;
	m_recycle_queue_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		recycle_queue_size,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);
}

void ParticleBackend::createUniformBuffers() {
	m_compute_uniform_buffers.clear();
	m_compute_uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		m_compute_uniform_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(ParticleParams),
			1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible |
			vk::MemoryPropertyFlagBits::eHostCoherent
		);
		m_compute_uniform_buffers[i]->map();
	}
}

void ParticleBackend::createEmitterParamsBuffers() {
	m_emitter_params_buffers.clear();
	m_emitter_params_buffers.resize(MAX_FRAMES_IN_FLIGHT);
	vk::DeviceSize size = sizeof(EmitterParams) * MAX_EMITTERS;
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		m_emitter_params_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			size,
			1,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		);
		m_emitter_params_buffers[i]->map();
	}
}

// Compute descriptor layout bindings:
//   1, 2   particles ping-pong (prev, out)
//   3      params UBO
//   4      spawn events
//   5      indirect draw command
//   6      particles_render
//   7      slot_pool_counters
//   8      free_slot_stack (LIFO)
//   9      recycle_queue
//   10     emitter_params
void ParticleBackend::createDescriptorSetLayouts() {
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(3, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(6, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(7, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(8, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(9, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(10, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.build();

	// Bindless atlas array (set 1 binding 0): MAX_PARTICLE_ATLASES combined-image-sampler
	// slots. Fragment shader samples particle_atlases[atlas_index] per particle.
	// UpdateAfterBind: registerAtlas() rewrites individual slots via vkUpdateDescriptorSets
	// while the set may be bound by in-flight CBs. PartiallyBound: unused slots are valid.
	m_render_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment, MAX_PARTICLE_ATLASES)
		.setBindingFlags(0,
			vk::DescriptorBindingFlagBits::eUpdateAfterBind |
			vk::DescriptorBindingFlagBits::ePartiallyBound)
		.setLayoutFlags(vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool)
		.build();
}

void ParticleBackend::createDescriptorSets() {
	m_compute_descriptor_sets.clear();
	m_compute_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		vk::raii::DescriptorSet set{nullptr};
		auto ubo_info = m_compute_uniform_buffers[i]->getDescriptorInfo();
		auto ssbo_info = m_shader_storage_buffers[i]->getDescriptorInfo();
		uint32_t prev = (i + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
		auto ssbo_info_last_frame = m_shader_storage_buffers[prev]->getDescriptorInfo();
		auto spawn_info = m_spawn_storage_buffers[i]->getDescriptorInfo();

		auto indirect_info = m_indirect_buffers[i]->getDescriptorInfo();
		auto render_info = m_render_buffers[i]->getDescriptorInfo();
		auto slot_pool_counters_info = m_slot_pool_counters_buffer->getDescriptorInfo();
		auto free_slot_stack_info = m_free_slot_stack_buffer->getDescriptorInfo();
		auto recycle_queue_info = m_recycle_queue_buffer->getDescriptorInfo();
		auto emitter_info = m_emitter_params_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, *m_descriptor_pool)
			.writeBuffer(3, &ubo_info)
			.writeBuffer(1, &ssbo_info_last_frame)
			.writeBuffer(2, &ssbo_info)
			.writeBuffer(4, &spawn_info)
			.writeBuffer(5, &indirect_info)
			.writeBuffer(6, &render_info)
			.writeBuffer(7, &slot_pool_counters_info)
			.writeBuffer(8, &free_slot_stack_info)
			.writeBuffer(9, &recycle_queue_info)
			.writeBuffer(10, &emitter_info)
			.build(set);
		m_compute_descriptor_sets.push_back(std::move(set));
	}
}

void ParticleBackend::createComputePipelineLayout() {
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = &*m_compute_set_layout->getDescriptorSetLayout(),
	};
	m_compute_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void ParticleBackend::createComputePipeline() {
	std::filesystem::path path = m_shader_path.parent_path() / "particle_update_comp.spv";
	m_compute_pipeline = std::make_unique<VeComputePipeline>(m_ve_device, path, m_compute_pipeline_layout);
}

void ParticleBackend::createSpawnComputePipeline() {
	std::filesystem::path path = m_shader_path.parent_path() / "particle_spawn_comp.spv";
	m_spawn_compute_pipeline = std::make_unique<VeComputePipeline>(m_ve_device, path, m_compute_pipeline_layout);
}

void ParticleBackend::createRecycleComputePipeline() {
	std::filesystem::path path = m_shader_path.parent_path() / "particle_recycle_comp.spv";
	m_recycle_compute_pipeline = std::make_unique<VeComputePipeline>(m_ve_device, path, m_compute_pipeline_layout);
}

void ParticleBackend::createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& texture_set_layout) {

	std::array<vk::DescriptorSetLayout, 2> set_layouts{*global_set_layout, *texture_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data()
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void ParticleBackend::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = sample_count;
	config.attribute_descriptions = RenderParticle::getAttributeDescriptions();
	config.binding_descriptions = RenderParticle::getBindingDescription();

	config.color_format = color_format;
	config.pipeline_layout = *m_pipeline_layout;

	// Billboard quads don't need culling
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;

	// attempt to reduce z-fighting
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.depth_stencil_info.depthCompareOp = vk::CompareOp::eGreaterOrEqual;
	config.rasterization_info.depthBiasEnable = VK_TRUE;
	config.rasterization_info.depthBiasConstantFactor = 0.0f;
	config.rasterization_info.depthBiasClamp = 0.0f;
	config.rasterization_info.depthBiasSlopeFactor = 1.0f;

	// Additive blending.
	config.color_blend_attachment.blendEnable = VK_TRUE;
	config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;

	m_pipeline = std::make_unique<VePipeline>(m_ve_device, m_shader_path, config);
}

void ParticleBackend::recordComputeCommands(VeFrameInfo& frame_info) {
	if (!m_enabled)
		return;
	assert(frame_info.current_frame < MAX_FRAMES_IN_FLIGHT && "current_frame out of bounds");
	assert(m_compute_uniform_buffers.size() == MAX_FRAMES_IN_FLIGHT && "compute_uniform_buffers size incorrect");
	assert(m_total_time >= 0.0f && "total_time should be non-negative");
	assert(frame_info.frame_time >= 0.0f && "delta_time should be non-negative");

	m_total_time += frame_info.frame_time;

	ParticleParams params{};
	params.delta_time = frame_info.frame_time * m_speed;
	params.total_time = m_total_time;
	params.capacity = m_capacity;
	params.frame_id = ++m_frame_id;

	// Gribb-Hartmann: extract clip-space planes from Camera
	glm::mat4 view = frame_info.camera_view.view;
	glm::mat4 proj = frame_info.camera_view.proj;
	glm::mat4 m = proj * view;
	glm::mat4 mt = glm::transpose(m);

	params.frustum_planes[0] = mt[3] + mt[0]; // Left
	params.frustum_planes[1] = mt[3] - mt[0]; // Right
	params.frustum_planes[2] = mt[3] + mt[1]; // Bottom
	params.frustum_planes[3] = mt[3] - mt[1]; // Top
	params.frustum_planes[4] = mt[2];         // Near
	params.frustum_planes[5] = mt[3] - mt[2]; // Far

	// Normalize planes
	for (int i = 0; i < 6; i++) {
		float length = glm::length(glm::vec3(params.frustum_planes[i]));
		if (length > 1e-6f)
			params.frustum_planes[i] /= length;
		else
			params.frustum_planes[i] = glm::vec4(0.0f);
	}

	// Reset is checked before draining pending spawns: the spawn dispatch is
	// skipped on reset frames, so draining first would silently lose those
	// events. Leave them in m_pending_spawns and let the next frame fire them.
	if (m_pending_reset) {
		params.reset = 1u;
		params.seed = m_reset_seed;
		m_total_time = 0.0f;
		m_pending_reset = false;
	} else {
		params.reset = 0u;
		params.seed = 0u;
	}

	params.spawn_event_count = 0;
	if (params.reset == 0u) {
		std::vector<SpawnEvent> current_frame_spawns;
		current_frame_spawns.reserve(MAX_SPAWN_EVENTS);

		{
			std::lock_guard<std::mutex> lock(m_pending_spawns_mutex);
			while (!m_pending_spawns.empty() && params.spawn_event_count < MAX_SPAWN_EVENTS) {
				current_frame_spawns.push_back(m_pending_spawns.front());
				m_pending_spawns.pop_front();
				params.spawn_event_count++;
			}
		}

		if (params.spawn_event_count > 0) {
			m_spawn_storage_buffers[frame_info.current_frame]->writeToBuffer(current_frame_spawns.data(), sizeof(SpawnEvent) * params.spawn_event_count);
		}
	}
	m_compute_uniform_buffers[frame_info.current_frame]->writeToBuffer(&params);

	// Upload emitter params SSBO. Whole-array copy is cheap for MAX_EMITTERS=256.
	m_emitter_params_buffers[frame_info.current_frame]->writeToBuffer(
		m_emitter_storage.data(),
		sizeof(EmitterParams) * MAX_EMITTERS);

	VkDrawIndirectCommand command{
		.vertexCount = 6,
		.instanceCount = 0,
		.firstVertex = 0,
		.firstInstance = 0
	};
	m_indirect_buffers[frame_info.current_frame]->writeToBuffer(&command);

	// Barrier to ensure indirect buffer write is visible to compute
	vk::BufferMemoryBarrier indirect_write_barrier{
		.srcAccessMask = vk::AccessFlagBits::eHostWrite,
		.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = m_indirect_buffers[frame_info.current_frame]->getBuffer(),
		.offset = 0,
		.size = VK_WHOLE_SIZE
	};

	frame_info.compute_command_buffer.pipelineBarrier(
		vk::PipelineStageFlagBits::eHost,
		vk::PipelineStageFlagBits::eComputeShader,
		{},
		nullptr,
		indirect_write_barrier,
		nullptr
	);

	// Reset: refill the free list with the first m_capacity slots and put
	// the stack top at m_capacity, so all dispatched slots are free again.
	if (params.reset != 0u) {
		const uint32_t cap = m_capacity;
		auto& cb = frame_info.compute_command_buffer;
		uint32_t initial_counters[2] = {cap, 0u};
		cb.updateBuffer<uint32_t>(m_slot_pool_counters_buffer->getBuffer(), 0, initial_counters);
		cb.copyBuffer(m_free_slot_stack_staging_buffer->getBuffer(), m_free_slot_stack_buffer->getBuffer(),
			vk::BufferCopy{ .srcOffset = 0, .dstOffset = 0, .size = sizeof(uint32_t) * cap });

		vk::MemoryBarrier reset_global_barrier{
			.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
			.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
		};
		cb.pipelineBarrier(
			vk::PipelineStageFlagBits::eTransfer,
			vk::PipelineStageFlagBits::eComputeShader,
			{},
			reset_global_barrier,
			nullptr,
			nullptr
		);
	}

	// Bind descriptor set once; the pipelines share the same layout.
	frame_info.compute_command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eCompute,
		*m_compute_pipeline_layout,
		0,
		{*m_compute_descriptor_sets[frame_info.current_frame]},
		{}
	);

	// Spawn pre-pass: one workgroup per SpawnEvent, pops slots from the dead
	// pool and writes new particles to particles_out[slot] directly. Skipped
	// on reset frames so freshly-queued events don't immediately consume the
	// just-re-seeded pool.
	if (params.spawn_event_count > 0 && params.reset == 0u) {
		frame_info.compute_command_buffer.bindPipeline(
			vk::PipelineBindPoint::eCompute, m_spawn_compute_pipeline->getPipeline());
		frame_info.compute_command_buffer.dispatch(params.spawn_event_count, 1, 1);

		vk::MemoryBarrier spawn_to_update_barrier{
			.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
			.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
		};
		frame_info.compute_command_buffer.pipelineBarrier(
			vk::PipelineStageFlagBits::eComputeShader,
			vk::PipelineStageFlagBits::eComputeShader,
			{},
			spawn_to_update_barrier,
			nullptr,
			nullptr
		);
	}

	frame_info.compute_command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	uint32_t group_count_x = (m_capacity + PARTICLE_WORKGROUP_SIZE - 1) / PARTICLE_WORKGROUP_SIZE;
	if (group_count_x > 0) {
		frame_info.compute_command_buffer.dispatch(group_count_x, 1, 1);
	}

	// Recycle pass: drain the recycle queue back onto the free list and zero
	// the queue count.
	// Skipped on reset frames; the counter re-seed zeroes the queue already.
	if (group_count_x > 0 && params.reset == 0u) {
		vk::MemoryBarrier update_to_recycle_barrier{
			.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
			.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
		};
		frame_info.compute_command_buffer.pipelineBarrier(
			vk::PipelineStageFlagBits::eComputeShader,
			vk::PipelineStageFlagBits::eComputeShader,
			{},
			update_to_recycle_barrier,
			nullptr,
			nullptr
		);

		frame_info.compute_command_buffer.bindPipeline(
			vk::PipelineBindPoint::eCompute, m_recycle_compute_pipeline->getPipeline());
		frame_info.compute_command_buffer.dispatch(1, 1, 1);
	}

	// Make compute shader writes available before the timeline semaphore signal.
	// Cross-queue visibility (DrawIndirect/VertexInput reads of indirect args and
	// vertex buffers) is handled by the graphics-side wait stages.
	vk::MemoryBarrier mem_barrier{
		.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
		.dstAccessMask = vk::AccessFlagBits::eNone
	};

	frame_info.compute_command_buffer.pipelineBarrier(
		vk::PipelineStageFlagBits::eComputeShader,
		vk::PipelineStageFlagBits::eBottomOfPipe,
		{},
		mem_barrier,
		nullptr,
		nullptr
	);
}


// Renders all particles with a single draw call. The shader storage buffer
// with particle positions and colors is bound as a vertex buffer.
// Instance rendering is used to draw a quad for each particle.
void ParticleBackend::render(VeFrameInfo& frame_info) const {
	if (!m_enabled) return;
	frame_info.cmd().bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->getPipeline());

	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{ *frame_info.global_descriptor_set, *m_render_descriptor_set },
		{}
	);
	vk::DeviceSize offsets[] = { 0 };
	// Use Render Buffer (compacted) instead of simulation buffer
	vk::Buffer buffers[] = { m_render_buffers[frame_info.current_frame]->getBuffer() };
	frame_info.cmd().bindVertexBuffers(0, buffers, offsets);

	frame_info.cmd().drawIndirect(
		m_indirect_buffers[frame_info.current_frame]->getBuffer(),
		0,
		1,
		sizeof(VkDrawIndirectCommand)
	);
}

void ParticleBackend::emitParticles(SpawnEvent event) {
	if (event.count == 0)
		return;
	if (m_capacity == 0)
		return;
	if (event.emitter_id >= MAX_EMITTERS)
		return;

	std::lock_guard<std::mutex> lock(m_pending_spawns_mutex);
	if (m_pending_spawns.size() >= MAX_PENDING_SPAWNS)
		return;
	m_pending_spawns.push_back(event);
}

void ParticleBackend::setCapacity(uint32_t count) {
	if (count == 0)
		count = 1; // avoid zero-sized buffers
	if (count == m_capacity)
		return;
	uint32_t old_count = m_capacity;

	m_ve_device.getDevice().waitIdle();

	m_shader_storage_buffers.clear();
	m_render_buffers.clear();
	m_indirect_buffers.clear();
	m_compute_descriptor_sets.clear();

	m_free_slot_stack_buffer.reset();
	m_free_slot_stack_staging_buffer.reset();
	m_recycle_queue_buffer.reset();
	m_slot_pool_counters_buffer.reset();

	m_capacity = count;
	m_pending_capacity = count;
	createShaderStorageBuffers();
	createFreeListBuffers();
	createDescriptorSets();
	VE_LOGI("ParticleBackend::setCapacity from " << old_count << " to " << count);
}

void ParticleBackend::stageCapacity(uint32_t count) {
	if (count == 0)
		count = 1;
	m_pending_capacity = count;
}

void ParticleBackend::applyStagedCapacity() {
	m_apply_pending = true;
}

void ParticleBackend::applyPendingResize() {
	if (!m_apply_pending)
		return;
	m_apply_pending = false;
	if (m_pending_capacity != m_capacity)
		setCapacity(m_pending_capacity);
}


} // namespace ve
