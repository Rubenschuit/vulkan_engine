#include "pch.hpp"
#include "rendering/particle_system.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include <random>
#include <chrono>
#include <chrono>

namespace ve {

ParticleSystem::ParticleSystem(const ParticleSystemCreateInfo& info)
	: m_ve_device(info.device), m_particle_count(info.particle_count),
	  m_origin(info.origin), m_descriptor_pool(info.descriptor_pool),
	  m_shader_path(info.shader_path) {

	if (info.event_bus) {
		info.event_bus->subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
			recreatePipeline(e.offscreen_format, e.sample_count);
		});
	}

	VE_LOGI("ParticleSystem constructor: particles=" << m_particle_count);
	m_pending_particle_count = m_particle_count;
	m_capacity = 0;

	createShaderStorageBuffers();
	createSpawnBuffers();
	createUniformBuffers();
	createDescriptorSetLayouts();
	createDescriptorSets();
	createComputePipelineLayout();
	createComputePipeline();
	createPipelineLayout(info.global_set_layout, m_render_set_layout->getDescriptorSetLayout());
	createPipeline(info.color_format, info.sample_count);
	createRenderDescriptorSet(info.particle_texture, info.fire_texture, info.smoke_texture);
	if (info.start_active) {
		scheduleRestart();
	}
}

void ParticleSystem::createRenderDescriptorSet(ResourceHandle<VeTexture> particle_texture,
											   ResourceHandle<VeTexture> fire_texture,
											   ResourceHandle<VeTexture> smoke_texture) {
	m_particle_texture_handle = std::move(particle_texture);
	m_fire_texture_handle = std::move(fire_texture);
	m_smoke_texture_handle = std::move(smoke_texture);

	auto particle_info = m_particle_texture_handle.get()->getDescriptorInfo();
	auto fire_info = m_fire_texture_handle.get()->getDescriptorInfo();
	auto smoke_info = m_smoke_texture_handle.get()->getDescriptorInfo();
	VeDescriptorWriter(*m_render_set_layout, *m_descriptor_pool)
		.writeImage(0, &particle_info)
		.writeImage(1, &fire_info)
		.writeImage(2, &smoke_info)
		.build(m_render_descriptor_set);
}

ParticleSystem::~ParticleSystem() {}

// TODO: make less terrible
void ParticleSystem::scheduleRestart() {
	m_pending_reset.store(true, std::memory_order_relaxed);
	// Basic seed using time
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
	m_reset_seed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Create or recreate the SSBOs for particle storage and
// schedule init of particles on next compute dispatch
void ParticleSystem::createShaderStorageBuffers() {

	// Allocate to capacity
	uint32_t alloc_count = std::max(m_particle_count, m_capacity > 0 ? m_capacity : m_particle_count);
	m_capacity = alloc_count;
	// zero initialize
	std::vector<Particle> particles(m_capacity, Particle{});
	// Staging buffer for upload to device local
	vk::DeviceSize buffer_size = static_cast<vk::DeviceSize>(m_capacity) * sizeof(Particle);
	VeBuffer staging_buffer(
		m_ve_device,
		buffer_size,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(particles.data());

	// Create per-frame SSBO and copy initial data
	m_shader_storage_buffers.clear();
	m_shader_storage_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	// Create render buffers
	m_render_buffers.clear();
	m_render_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	// Create indirect draw buffers
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
		// Simulation buffers
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

		// Render buffers (same size as simulation for worst case)
		m_render_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			buffer_size,
			1,
			vk::BufferUsageFlagBits::eStorageBuffer |
			vk::BufferUsageFlagBits::eVertexBuffer, // Used for rendering
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);

		// Indirect buffers
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

void ParticleSystem::createSpawnBuffers() {
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

	// Create indices counter buffer (6 uints: dead_count, alive_count, dead_head, dead_tail, alive_head, alive_tail)
	m_indices_counter_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(uint32_t) * 8, // 6 counters + padding to 32 bytes alignment
		1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	// Initialize counters to zero (dead_count=0, alive_count=0, dead_head=0, dead_tail=0, alive_head=0, alive_tail=0)
	uint32_t zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(uint32_t) * 8,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(zeros);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_indices_counter_buffer->getBuffer(), sizeof(uint32_t) * 8);

	uint32_t buffer_capacity = (m_capacity > 0) ? m_capacity : m_particle_count;
	vk::DeviceSize dead_indices_size = sizeof(uint32_t) * buffer_capacity;
	m_dead_indices_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		dead_indices_size,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	// Create alive indices buffer (size = capacity to match particle buffers)
	vk::DeviceSize alive_indices_size = sizeof(uint32_t) * buffer_capacity;
	m_alive_indices_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		alive_indices_size,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	// Spawn staging buffers (ping-ponged per frame):
	// Spawner writes to current frame's buffer, thread i reads from previous frame's buffer.
	m_spawn_buffers.clear();
	m_spawn_buffers.resize(MAX_FRAMES_IN_FLIGHT);
	m_spawn_flags_buffers.clear();
	m_spawn_flags_buffers.resize(MAX_FRAMES_IN_FLIGHT);

	vk::DeviceSize spawn_buffer_size = sizeof(Particle) * buffer_capacity;
	vk::DeviceSize spawn_flags_size = sizeof(uint32_t) * buffer_capacity;

	// Zero-init staging for spawn flags
	std::vector<uint32_t> zero_flags(buffer_capacity, 0);
	VeBuffer flags_staging(
		m_ve_device, spawn_flags_size, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	flags_staging.map();
	flags_staging.writeToBuffer(zero_flags.data());

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		m_spawn_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device, spawn_buffer_size, 1,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);

		m_spawn_flags_buffers[i] = std::make_unique<VeBuffer>(
			m_ve_device, spawn_flags_size, 1,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eDeviceLocal
		);
		// Zero-initialize flags so first frame doesn't read garbage from spawn_prev
		m_ve_device.copyBuffer(flags_staging.getBuffer(), m_spawn_flags_buffers[i]->getBuffer(), spawn_flags_size);
	}
}

void ParticleSystem::createUniformBuffers() {
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

// For the compute shader we need:
// - UBO with parameters
// - An input and output particle SSBO
// - Spawn event buffer
// - Indirect command buffer (RW)
// - Render buffer (RW)
// - Indices counters buffer (RW)
// - Dead indices buffer (RW)
// - Alive indices buffer (RW)
void ParticleSystem::createDescriptorSetLayouts() {
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(3, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Indirect buffer
		.addBinding(6, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Render buffer
		.addBinding(7, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Indices Counters
		.addBinding(8, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Dead Indices
		.addBinding(9, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Alive Indices
		.addBinding(10, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Spawn Prev
		.addBinding(11, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Spawn Flags Prev
		.addBinding(12, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Spawn Out
		.addBinding(13, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Spawn Flags Out
		.build();

	// Binding 3 different particle textures for rendering (glow, fire, smoke)
	m_render_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();
}

void ParticleSystem::createDescriptorSets() {
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
		auto counter_info = m_indices_counter_buffer->getDescriptorInfo();
		auto dead_indices_info = m_dead_indices_buffer->getDescriptorInfo();
		auto alive_indices_info = m_alive_indices_buffer->getDescriptorInfo();
		// Ping-pong spawn buffers: prev frame for reading, current frame for writing
		auto spawn_prev_info = m_spawn_buffers[prev]->getDescriptorInfo();
		auto spawn_flags_prev_info = m_spawn_flags_buffers[prev]->getDescriptorInfo();
		auto spawn_out_info = m_spawn_buffers[i]->getDescriptorInfo();
		auto spawn_flags_out_info = m_spawn_flags_buffers[i]->getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, *m_descriptor_pool)
			.writeBuffer(3, &ubo_info)
			.writeBuffer(1, &ssbo_info_last_frame)
			.writeBuffer(2, &ssbo_info)
			.writeBuffer(4, &spawn_info)
			.writeBuffer(5, &indirect_info)
			.writeBuffer(6, &render_info)
			.writeBuffer(7, &counter_info)
			.writeBuffer(8, &dead_indices_info)
			.writeBuffer(9, &alive_indices_info)
			.writeBuffer(10, &spawn_prev_info)
			.writeBuffer(11, &spawn_flags_prev_info)
			.writeBuffer(12, &spawn_out_info)
			.writeBuffer(13, &spawn_flags_out_info)
			.build(set);
		m_compute_descriptor_sets.push_back(std::move(set));
	}
}

void ParticleSystem::createComputePipelineLayout() {
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = &*m_compute_set_layout->getDescriptorSetLayout(),
	};
	m_compute_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

// Compute shader .spv file names are appended with a c
// TODO: make this more robust
void ParticleSystem::createComputePipeline() {
	// remove .spv, add c.spv
	std::string path_str = m_shader_path.string();
	path_str = path_str.substr(0, path_str.size() - 4) + "c.spv";
	std::filesystem::path path = std::filesystem::path(path_str);

	m_compute_pipeline = std::make_unique<VeComputePipeline>(m_ve_device, path, m_compute_pipeline_layout);
}

void ParticleSystem::createPipelineLayout(
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

void ParticleSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = sample_count;
	config.attribute_descriptions = Particle::getAttributeDescriptions();
	config.binding_descriptions = Particle::getBindingDescription();

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

	//enable additve blending
	config.color_blend_attachment.blendEnable = VK_TRUE;
	config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne; //
	config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOne; //
	config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne; //
	config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne; //
	config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;

	m_pipeline = std::make_unique<VePipeline>(m_ve_device, m_shader_path, config);
}

// Updates the particle system by recording compute commands into the compute command buffer.
// updates the particle parameters UBO for compute shader
void ParticleSystem::recordComputeCommands(VeFrameInfo& frame_info) {
	if (!m_enabled) return;
	assert(frame_info.current_frame < MAX_FRAMES_IN_FLIGHT && "current_frame out of bounds");
	assert(m_compute_uniform_buffers.size() == MAX_FRAMES_IN_FLIGHT && "compute_uniform_buffers size incorrect");
	assert(m_total_time >= 0.0f && "total_time should be non-negative");
	assert(frame_info.frame_time >= 0.0f && "delta_time should be non-negative");

	m_total_time += frame_info.frame_time;

	ParticleParams params{};
	params.delta_time = frame_info.frame_time * m_speed;
	params.total_time = m_total_time;
	params.particle_count = m_particle_count;
	params.origin = glm::vec4(m_origin, 1.0f);
	params.reset_kind = m_reset_kind;
	params.mode = m_mode;
	params.mean = m_mean;
	params.stddev = m_stddev;
	params.row_count = 8; // TODO: make configurable
	params.min_life = m_min_life;
	params.max_life = m_max_life;
	params.should_respawn = m_should_respawn ? 1u : 0u;
	params.gravity = m_gravity;
	params.trail_interval = m_trail_interval;
	params.trail_timeout = m_trail_timeout;
	params.flash_scale = m_flash_scale;
	params.flash_time = m_flash_time;
	params.frame_id = ++m_frame_id;
	// Default wind
	params.wind_direction = m_wind_direction;

	// Calculate Frustum Planes from Camera
	glm::mat4 view = frame_info.camera_view.view;
	glm::mat4 proj = frame_info.camera_view.proj;
	glm::mat4 m = proj * view;
	glm::mat4 mt = glm::transpose(m);

	params.frustum_planes[0] = mt[3] + mt[0]; // Left
	params.frustum_planes[1] = mt[3] - mt[0]; // Right
	params.frustum_planes[2] = mt[3] + mt[1]; // Bottom
	params.frustum_planes[3] = mt[3] - mt[1]; // Top
	params.frustum_planes[4] = mt[2]; // Near (Vulkan 0 to 1 Z range)
	params.frustum_planes[5] = mt[3] - mt[2]; // Far

	// Normalize planes
	for (int i = 0; i < 6; i++) {
		float length = glm::length(glm::vec3(params.frustum_planes[i]));
		params.frustum_planes[i] /= length;
	}

	// Process pending spawns
	params.spawn_event_count = 0;
	std::vector<SpawnEvent> current_frame_spawns;
	current_frame_spawns.reserve(MAX_SPAWN_EVENTS);

	while (!m_pending_spawns.empty() && params.spawn_event_count < MAX_SPAWN_EVENTS) {
		current_frame_spawns.push_back(m_pending_spawns.front());
		m_pending_spawns.pop_front();
		params.spawn_event_count++;
	}

	if (params.spawn_event_count > 0) {
		m_spawn_storage_buffers[frame_info.current_frame]->writeToBuffer(current_frame_spawns.data(), sizeof(SpawnEvent) * params.spawn_event_count);
	}

	if (m_pending_reset.load(std::memory_order_relaxed)) {
		params.reset = 1u;
		params.seed = m_reset_seed;
		m_total_time = 0.0f;
		m_pending_reset.store(false, std::memory_order_relaxed);
	} else {
		params.reset = 0u;
		params.seed = 0u;
	}
	m_compute_uniform_buffers[frame_info.current_frame]->writeToBuffer(&params);

	// Reset indirect command instance count to 0
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


	// Bind compute pipeline and descriptor sets
	frame_info.compute_command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	frame_info.compute_command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eCompute,
		*m_compute_pipeline_layout,
		0,
		{*m_compute_descriptor_sets[frame_info.current_frame]},
		{}
	);

	// Dispatch workgroups to cover active particles only (not full buffer capacity).
	// Particles at indices >= m_particle_count are dead/unused and don't need processing.
	// Must match [numthreads(256, 1, 1)] in the compute shader.
	const uint32_t workgroup_size = 256;
	uint32_t group_count_x = (m_particle_count + workgroup_size - 1) / workgroup_size;
	if (group_count_x > 0) {
		frame_info.compute_command_buffer.dispatch(group_count_x, 1, 1);
	}


	// Make compute shader writes available before the timeline semaphore signal.
	// Cross-queue visibility (vertex reads, indirect command reads) is handled by
	// the semaphore wait in submitAndPresent with dstStageMask = eVertexInput | eDrawIndirect.
	// We only use compute-compatible stages here so this works on dedicated compute queue families.
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
void ParticleSystem::render(VeFrameInfo& frame_info) const {
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

	// Indirect Draw
	frame_info.cmd().drawIndirect(
		m_indirect_buffers[frame_info.current_frame]->getBuffer(),
		0,
		1,
		sizeof(VkDrawIndirectCommand)
	);
}

void ParticleSystem::emitParticles(uint32_t count) {
	SpawnEvent e{
		.position_scale = glm::vec4(m_origin, 1.0f),
		.velocity_life = glm::vec4(0.0f),
		.color = glm::vec4(1.0f),
		.info = {0, count, static_cast<uint32_t>(ParticleType::DEFAULT), 0}
	};
	emitParticles(e);
}

void ParticleSystem::emitParticles(SpawnEvent info) {
	if (info.info.y == 0) return; // count is 0
	if (m_particle_count == 0) return;

	// info.y is count.
	uint32_t count = info.info.y;

	// Ensure head is valid
	if (m_emit_head >= m_particle_count) m_emit_head = 0;

	// Check for wrap
	if (m_emit_head + count <= m_particle_count) {
		// No wrap
		info.info.x = m_emit_head; // start_index
		m_pending_spawns.push_back(info);
		m_emit_head += count;
	} else {
		// Wrap around
		uint32_t first_chunk = m_particle_count - m_emit_head;
		uint32_t second_chunk = count - first_chunk;

		// First chunk
		SpawnEvent e1 = info;
		e1.info.x = m_emit_head;
		e1.info.y = first_chunk;
		m_pending_spawns.push_back(e1);

		// Second chunk
		SpawnEvent e2 = info;
		e2.info.x = 0;
		e2.info.y = second_chunk;
		m_pending_spawns.push_back(e2);

		m_emit_head = second_chunk;
	}
}

void ParticleSystem::setParticleCount(uint32_t count, bool reset) {
	if (count == 0) count = 1; // avoid zero-sized buffers
	if (count == m_particle_count) return;

	m_emit_head = 0;
	// Grow capacity if needed
	if (count > m_capacity) {
		// Ensure GPU is idle before resizing GPU resources
		m_ve_device.getDevice().waitIdle();

		// Explicitly clear old resources before creating new ones
		m_shader_storage_buffers.clear();
		m_render_buffers.clear();
		m_indirect_buffers.clear();
		m_compute_descriptor_sets.clear();

		m_dead_indices_buffer.reset();
		m_alive_indices_buffer.reset();
		m_indices_counter_buffer.reset();
		m_spawn_buffers.clear();
		m_spawn_flags_buffers.clear();

		m_particle_count = count;
		m_pending_particle_count = m_particle_count;
		// Recreate storage buffers sized to new capacity and reset on next dispatch
		m_capacity = std::max(count, static_cast<uint32_t>(m_capacity ));
		createShaderStorageBuffers();
		createSpawnBuffers();
		createDescriptorSets();
	} else {
		// Within capacity: just adjust logical count; compute shader will skip extra threads
		m_particle_count = count;
		m_pending_particle_count = m_particle_count;
		if (reset) scheduleRestart();
	}
	VE_LOGI("ParticleSystem::setParticleCount from " << m_particle_count << " to " << count << " with capacity " << m_capacity);
}

void ParticleSystem::ensureCapacity(uint32_t needed) {
	if (needed <= m_capacity) return;
	setParticleCount(needed); // setParticleCount handles growing capacity and reinit
}

void ParticleSystem::stageParticleCount(uint32_t count) {
	if (count == 0) count = 1;
	m_pending_particle_count = count;
}

void ParticleSystem::applyStagedParticleCount() {
	if (m_pending_particle_count != m_particle_count) {
		setParticleCount(m_pending_particle_count);
		scheduleRestart();
	}
}


} // namespace ve