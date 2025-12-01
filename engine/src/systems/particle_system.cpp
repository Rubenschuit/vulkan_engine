#include "pch.hpp"
#include "systems/particle_system.hpp"
#include <random>
#include <chrono>
#include <chrono>

namespace ve {

ParticleSystem::ParticleSystem(
	VeDevice& device,
	std::shared_ptr<VeDescriptorPool> descriptor_pool,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& texture_set_layout,
	vk::Format color_format,
	uint32_t particle_count,
	glm::vec3 origin,
	std::filesystem::path shader_path,
	bool start_active)
	: m_ve_device(device), m_particle_count(particle_count),
	  m_origin(origin), m_descriptor_pool(std::move(descriptor_pool)),
	  m_shader_path(shader_path) {
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
	createPipelineLayout(global_set_layout, texture_set_layout);
	createPipeline(color_format, m_ve_device.getSampleCount());
	if (start_active) {
		scheduleRestart();
	}
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
	staging_buffer.writeToBuffer((void*)particles.data());

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

	// Create global counter buffer (2 uints: WriteHead, ReadHead)
	m_global_counter_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		sizeof(uint32_t) * 4, // Padding to 16 bytes just in case
		1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	// Initialize counters to zero
	uint32_t zeros[4] = {0, 0, 0, 0};
	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(uint32_t) * 4,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(zeros);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_global_counter_buffer->getBuffer(), sizeof(uint32_t) * 4);

	// Create GPU trail buffer
	vk::DeviceSize trail_buffer_size = sizeof(Particle) * m_trail_buffer_size;
	m_gpu_trail_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		trail_buffer_size,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);
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
void ParticleSystem::createDescriptorSetLayouts() {
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(3, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Indirect buffer
		.addBinding(6, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Render buffer
		.addBinding(7, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Global Counter
		.addBinding(8, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // Trail Queue
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
		auto counter_info = m_global_counter_buffer->getDescriptorInfo();
		auto trail_info = m_gpu_trail_buffer->getDescriptorInfo();

		VeDescriptorWriter(*m_compute_set_layout, *m_descriptor_pool)
			.writeBuffer(3, &ubo_info)
			.writeBuffer(1, &ssbo_info_last_frame)
			.writeBuffer(2, &ssbo_info)
			.writeBuffer(4, &spawn_info)
			.writeBuffer(5, &indirect_info)
			.writeBuffer(6, &render_info)
			.writeBuffer(7, &counter_info)
			.writeBuffer(8, &trail_info)
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

	// attempt to reduce z-fighting
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLessOrEqual;
	config.rasterization_info.depthBiasEnable = VK_TRUE;
	config.rasterization_info.depthBiasConstantFactor = 0.0f;
	config.rasterization_info.depthBiasClamp = 0.0f;
	config.rasterization_info.depthBiasSlopeFactor = -1.0f;

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
void ParticleSystem::update(VeFrameInfo& frame_info) {
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
	params.trail_buffer_size = m_trail_buffer_size;
	params.trail_interval = m_trail_interval;
	params.trail_timeout = m_trail_timeout;
	params.flash_scale = m_flash_scale;
	params.flash_time = m_flash_time;
	// Default wind
	params.wind_direction = m_wind_direction;

	// Calculate Frustum Planes from Camera
	glm::mat4 view = frame_info.camera.getView();
	glm::mat4 proj = frame_info.camera.getProj();
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
		.buffer = *m_indirect_buffers[frame_info.current_frame]->getBuffer(),
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


	frame_info.compute_command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	frame_info.compute_command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eCompute,
		*m_compute_pipeline_layout,
		0,
		{*m_compute_descriptor_sets[frame_info.current_frame]},
		{}
	);

	// Dispatch enough workgroups to cover all particles, even when not a multiple of 256
	// shader discards excess threads
	const uint32_t workgroup_size = 256;
	uint32_t group_count_x = (m_capacity + workgroup_size - 1) / workgroup_size; // ceilDiv
	if (group_count_x > 0) {
		frame_info.compute_command_buffer.dispatch(group_count_x, 1, 1);
	}


	// Add barrier to ensure compute writes are visible to vertex shader read and indirect command execution
	vk::BufferMemoryBarrier barrier{
		.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
		.dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = *m_render_buffers[frame_info.current_frame]->getBuffer(),
		.offset = 0,
		.size = VK_WHOLE_SIZE
	};

	// Barrier for indirect buffer
	vk::BufferMemoryBarrier indirect_barrier{
		.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
		.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = *m_indirect_buffers[frame_info.current_frame]->getBuffer(),
		.offset = 0,
		.size = VK_WHOLE_SIZE
	};

	vk::BufferMemoryBarrier barriers[] = { barrier, indirect_barrier };

	frame_info.compute_command_buffer.pipelineBarrier(
		vk::PipelineStageFlagBits::eComputeShader,
		vk::PipelineStageFlagBits::eVertexInput | vk::PipelineStageFlagBits::eDrawIndirect,
		{},
		nullptr,
		{ barriers[0], barriers[1] },
		nullptr
	);


}


// Renders all particles with a single draw call. The shader storage buffer
// with particle positions and colors is bound as a vertex buffer.
// Instance rendering is used to draw a quad for each particle.
void ParticleSystem::render(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->getPipeline());

	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{ *frame_info.global_descriptor_set, *frame_info.texture_descriptor_set },
		{}
	);
	vk::DeviceSize offsets[] = { 0 };
	// Use Render Buffer (compacted) instead of simulation buffer
	vk::Buffer buffers[] = { *m_render_buffers[frame_info.current_frame]->getBuffer() };
	frame_info.command_buffer.bindVertexBuffers(0, buffers, offsets);

	// Indirect Draw
	frame_info.command_buffer.drawIndirect(
		*m_indirect_buffers[frame_info.current_frame]->getBuffer(),
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
		.info = {0, count, TYPE_DEFAULT, 0}
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

		m_particle_count = count;
		m_pending_particle_count = m_particle_count;
		// Recreate storage buffers sized to new capacity and reset on next dispatch
		m_capacity = std::max(count, static_cast<uint32_t>(m_capacity ));
		createShaderStorageBuffers();
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

void ParticleSystem::setTrailBufferSize(uint32_t size) {
	if (size == 0) size = 10; // minimal safe size
	if (size == m_trail_buffer_size) return;

	m_trail_buffer_size = size;
	VE_LOGI("ParticleSystem: Resizing trail buffer to " << m_trail_buffer_size);

	m_ve_device.getDevice().waitIdle();

	// Destroy old buffer first to free resources
	m_gpu_trail_buffer.reset();

	// Recreate buffer
	vk::DeviceSize buffer_size = sizeof(Particle) * m_trail_buffer_size;
	m_gpu_trail_buffer = std::make_unique<VeBuffer>(
		m_ve_device,
		buffer_size,
		1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal
	);

	// Clear trail buffer to prevent reading garbage data
	std::vector<Particle> empty_particles(m_trail_buffer_size);
	for (auto& p : empty_particles) {
		p.position.w = 0.0f;
		p.color.a = 0.0f;
		p.extra_data = glm::vec4(0.0f);
	}
	VeBuffer trail_staging_buffer(
		m_ve_device,
		buffer_size,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	trail_staging_buffer.map();
	trail_staging_buffer.writeToBuffer(empty_particles.data());
	m_ve_device.copyBuffer(trail_staging_buffer.getBuffer(), m_gpu_trail_buffer->getBuffer(), buffer_size);

	// Reset global counter buffer (WriteHead and ReadHead to 0)
	uint32_t zeros[4] = {0, 0, 0, 0};
	VeBuffer staging_buffer(
		m_ve_device,
		sizeof(uint32_t) * 4,
		1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
	);
	staging_buffer.map();
	staging_buffer.writeToBuffer(zeros);
	m_ve_device.copyBuffer(staging_buffer.getBuffer(), m_global_counter_buffer->getBuffer(), sizeof(uint32_t) * 4);

	// Recreate descriptor sets to point to new buffer
	m_compute_descriptor_sets.clear();
	createDescriptorSets();
}

} // namespace ve