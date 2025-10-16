#include "pch.hpp"
#include "particle_system.hpp"
#include <random>

namespace ve {

	ParticleSystem::ParticleSystem(VeDevice& device,
								   std::shared_ptr<VeDescriptorPool> descriptor_pool,
								   const vk::raii::DescriptorSetLayout& global_set_layout,
								   vk::Format color_format,
								   uint32_t particle_count)
		: m_ve_device(device), m_particle_count(particle_count),
		  m_descriptor_pool(std::move(descriptor_pool)) {
		VE_LOGI("ParticleSystem ctor: particles=" << m_particle_count);
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
	void ParticleSystem::restart() {
		//wait until device is idle
		m_ve_device.getDevice().waitIdle();
		createShaderStorageBuffers();
		createDescriptorSets();
	}

	void ParticleSystem::createShaderStorageBuffers() {
		// Initialise particles
		std::default_random_engine rnd_engine((unsigned)time(nullptr));
		std::normal_distribution<float> rnd_dist(0.0f, 3.0f);
		std::uniform_real_distribution<float> uni_dist(0.1f, 0.2f);
		std::vector<Particle> particles(m_particle_count);
		for (auto& particle : particles) {
			particle.position = glm::vec4(0.0f, 0.0f, 10.0f, uni_dist(rnd_engine)); // w is scale
			//random velocity between -1 and 1
			particle.velocity = glm::vec4(rnd_dist(rnd_engine),
										  rnd_dist(rnd_engine),
										  rnd_dist(rnd_engine),
										  0.0f);
			particle.color = glm::vec4(rnd_dist(rnd_engine), rnd_dist(rnd_engine), rnd_dist(rnd_engine), 1.0f);
		}
		// Staging buffer for upload to device local
		vk::DeviceSize buffer_size = m_particle_count * sizeof(Particle);
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
			//vk::DescriptorBufferInfo bufferInfo(m_ubos[i]->getBuffer(), 0, sizeof(UniformBufferObject));
            //vk::DescriptorBufferInfo storageBufferInfoLastFrame(m_ssbos[(i - 1) % MAX_FRAMES_IN_FLIGHT]->getBuffer(), 0, sizeof(Particle) * m_particle_count);
            //vk::DescriptorBufferInfo storageBufferInfoCurrentFrame(m_ssbos[i]->getBuffer(), 0, sizeof(Particle) * m_particle_count);
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
		const char* shader_path = "shaders/particle_compute.spv";
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
		m_pipeline = std::make_unique<VePipeline>(m_ve_device, "shaders/particle_billboard.spv", config);
	}

	void ParticleSystem::recordCompute(VeFrameInfo& frame_info) const {
		// Update compute parameters UBO
		ParticleParams params{};
		params.delta_time = frame_info.frame_time;
		params.total_time = 0.0f;
		params.particle_count = m_particle_count;
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
		// No per-vertex geometry buffer; unit quad is generated in shader from SV_VertexID
		frame_info.command_buffer.draw(6, m_particle_count, 0, 0);
	}

} // namespace ve
