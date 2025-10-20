#include "pch.hpp"
#include "particle_system.hpp"
#include <random>
#include <chrono>
#include <chrono>

namespace ve {

ParticleSystem::ParticleSystem(VeDevice& device,
								std::shared_ptr<VeDescriptorPool> descriptor_pool,
								const vk::raii::DescriptorSetLayout& global_set_layout,
								vk::Format color_format,
								uint32_t particle_count,
								glm::vec3 origin	)
	: m_ve_device(device), m_particle_count(particle_count),
		m_origin(origin), m_descriptor_pool(std::move(descriptor_pool)) {
	VE_LOGI("ParticleSystem constructor: particles=" << m_particle_count);
	m_pending_particle_count = m_particle_count;
	m_capacity = 0;
	createShaderStorageBuffers();
	createUniformBuffers();
	createDescriptorSetLayouts();
	createDescriptorSets();
	createComputePipelineLayout();
	createComputePipeline();
	createPipelineLayout(global_set_layout);
	createPipeline(color_format);
}

ParticleSystem::~ParticleSystem() {}

// TODO: make less terrible
void ParticleSystem::scheduleRestart() {
	// Schedule a GPU-side reset on next compute dispatch to avoid CPU stalls
	m_pending_reset.store(true, std::memory_order_relaxed);
	// Basic seed using time; could be improved or controlled by caller
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
	m_reset_seed = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Create or recreate the SSBOs for particle storage and
// schedule init of particles on next compute dispatch
void ParticleSystem::createShaderStorageBuffers() {

	// Allocate to capacity, which may be larger than current count for amortized growth
	uint32_t alloc_count = std::max(m_particle_count, m_capacity > 0 ? m_capacity : m_particle_count);
	m_capacity = alloc_count;
	std::vector<Particle> particles(m_capacity);
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
	}
	scheduleRestart(); // sets m_reset_seed and m_pending_reset so the shader knows to init
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
void ParticleSystem::createDescriptorSetLayouts() {
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(3, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)
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
		VeDescriptorWriter(*m_compute_set_layout, *m_descriptor_pool)
			.writeBuffer(3, &ubo_info)
			.writeBuffer(1, &ssbo_info_last_frame)
			.writeBuffer(2, &ssbo_info)
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

void ParticleSystem::createComputePipeline() {
	const char* shader_path = "shaders/particle_computec.spv";
	m_compute_pipeline = std::make_unique<VeComputePipeline>(m_ve_device, shader_path, m_compute_pipeline_layout);
}

void ParticleSystem::createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout) {

	std::array<vk::DescriptorSetLayout, 1> set_layouts{*global_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data()
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void ParticleSystem::createPipeline(vk::Format color_format) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config);
	// Use instanced attributes for particles; static unit quad provided by fixed pipeline state
	config.attribute_descriptions = Particle::getAttributeDescriptions();
	config.binding_descriptions = Particle::getBindingDescription();

	config.color_format = color_format;
	config.pipeline_layout = m_pipeline_layout;
	// Enable depth testing but disable depth writes for alpha-blended particles
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.rasterization_info.depthBiasEnable = VK_FALSE;
	m_pipeline = std::make_unique<VePipeline>(m_ve_device, "shaders/particle_compute.spv", config);
}

// Updates the particle system by recording compute commands into the compute command buffer.
// updates the particle parameters UBO
void ParticleSystem::update(VeFrameInfo& frame_info) {
	assert(frame_info.current_frame < MAX_FRAMES_IN_FLIGHT && "current_frame out of bounds");
	assert(m_compute_uniform_buffers.size() == MAX_FRAMES_IN_FLIGHT && "compute_uniform_buffers size incorrect");
	assert(m_total_time >= 0.0f && "total_time should be non-negative");
	assert(frame_info.frame_time >= 0.0f && "delta_time should be non-negative");

	m_total_time += frame_info.frame_time;

	ParticleParams params{};
	params.delta_time = frame_info.frame_time;
	params.total_time = m_total_time;
	params.particle_count = m_particle_count;
	params.origin = m_origin;
	params.reset_kind = m_reset_kind;
	params.mode = m_mode;
	params.mean = m_mean;
	params.stddev = m_stddev;
	if (m_pending_reset.load(std::memory_order_relaxed)) {
		params.reset = 1u;
		params.seed = m_reset_seed;
		m_total_time = 0.0f;
		// Clear pending flag so it only applies once
		m_pending_reset.store(false, std::memory_order_relaxed);
	} else {
		params.reset = 0u;
		params.seed = 0u;
	}
	m_compute_uniform_buffers[frame_info.current_frame]->writeToBuffer(&params);
	frame_info.compute_command_buffer.reset();
	frame_info.compute_command_buffer.begin(vk::CommandBufferBeginInfo{});
	frame_info.compute_command_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	frame_info.compute_command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eCompute,
		*m_compute_pipeline_layout,
		0,
		*m_compute_descriptor_sets[frame_info.current_frame],
		{}
	);

	// Dispatch enough workgroups to cover all particles, even when not a multiple of 256
	// shader discards excess threads
	uint32_t group_count_x = (m_particle_count + 256 - 1) / 256; // ceilDiv
	if (group_count_x > 0) {
		frame_info.compute_command_buffer.dispatch(group_count_x, 1, 1);
	}
	frame_info.compute_command_buffer.end();
}


// Renders all particles with a single draw call. The shader storage buffer
// with particle positions and colors is bound as a vertex buffer.
// Instance rendering is used to draw a quad for each particle.
void ParticleSystem::render(VeFrameInfo& frame_info) const {
	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_pipeline->getPipeline());

	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		m_pipeline_layout,
		0,
		{ *frame_info.global_descriptor_set },
		{}
	);
	vk::DeviceSize offsets[] = { 0 };
	vk::Buffer buffers[] = { *m_shader_storage_buffers[frame_info.current_frame]->getBuffer() };
	frame_info.command_buffer.bindVertexBuffers(0, buffers, offsets);

	// cap particles when spawning in
	uint32_t particles_to_spawn = m_particle_count;
	float delay_factor = 0.5f; // time to full spawn
	if (m_total_time < delay_factor) {
		particles_to_spawn = static_cast<uint32_t>(m_particle_count * (m_total_time / delay_factor));
	}
	// unit quad is generated in shader from SV_VertexID
	frame_info.command_buffer.draw(6, particles_to_spawn, 0, 0);
}

void ParticleSystem::setParticleCount(uint32_t count) {
	if (count == 0) count = 1; // avoid zero-sized buffers
	if (count == m_particle_count) return;
	VE_LOGI("ParticleSystem::setParticleCount from " << m_particle_count << " to " << count);
	// Grow capacity if needed; shrinking does not free immediately to avoid churn
	if (count > m_capacity) {
		// Ensure GPU is idle before resizing GPU resources
		m_ve_device.getDevice().waitIdle();
		m_particle_count = count;
		m_pending_particle_count = m_particle_count;
		// Recreate storage buffers sized to new capacity and reset on next dispatch
		m_capacity = count;
		createShaderStorageBuffers();
		createDescriptorSets();
	} else {
		// Within capacity: just adjust logical count; compute shader will skip extra threads
		m_particle_count = count;
		m_pending_particle_count = m_particle_count;
		scheduleRestart(); // schedule a re-init if desired when count changes
	}
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
