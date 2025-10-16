#pragma once
#include "ve_config.hpp"
#include "ve_buffer.hpp"
#include "ve_descriptors.hpp"
#include "ve_frame_info.hpp"
#include "ve_pipeline.hpp"
#include "ve_compute_pipeline.hpp"

#include <memory>
#include <vector>

namespace ve {

	struct ParticleParams {
		float delta_time;
		float total_time;
		uint32_t particle_count;
		uint32_t pad0;
	};

	struct Particle {
		glm::vec4 position; // w is scale
		glm::vec4 velocity; // w component unused
		glm::vec4 color;

		static std::vector<vk::VertexInputBindingDescription> getBindingDescription() {
			// Per-instance particle attributes (position, color)
			return { { 0, sizeof(Particle), vk::VertexInputRate::eInstance } };
		}

		// we dont need velocity for rendering
		static std::vector<vk::VertexInputAttributeDescription> getAttributeDescriptions() {
			return {
				vk::VertexInputAttributeDescription( 0, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Particle, position) ),
				vk::VertexInputAttributeDescription( 1, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(Particle, color) )
			};
		}
};

	class ParticleSystem {
	public:
		ParticleSystem(VeDevice& device,
					std::shared_ptr<VeDescriptorPool> descriptor_pool,
					const vk::raii::DescriptorSetLayout& global_set_layout,
					vk::Format color_format,
					uint32_t particle_count);
		~ParticleSystem();

		ParticleSystem(const ParticleSystem&) = delete;
		ParticleSystem& operator=(const ParticleSystem&) = delete;

		void recordCompute(VeFrameInfo& frame_info) const;
		void render(VeFrameInfo& frame_info) const;
		void restart(); // reset particle positions


	private:
		void createDescriptorSetLayouts();
		void createShaderStorageBuffers();
		void createUniformBuffers();
		void createDescriptorSets();
		void createComputePipelineLayout();
		void createComputePipeline();
		void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
		void createPipeline(vk::Format color_format);

		VeDevice& m_ve_device;
		uint32_t m_particle_count = 0;

		// Descriptor layouts for this system
		std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;

		// Per-frame resources
		std::vector<std::unique_ptr<VeBuffer>> m_compute_uniform_buffers;  // small UBO per frame
		std::vector<std::unique_ptr<VeBuffer>> m_shader_storage_buffers; // large SSBO per frame
		std::vector<vk::raii::DescriptorSet> m_compute_descriptor_sets;

		// Shared pool for descriptor allocations (shared to ensure lifetime across systems)
		std::shared_ptr<VeDescriptorPool> m_descriptor_pool;

		//
		vk::raii::PipelineLayout m_compute_pipeline_layout{nullptr};
		std::unique_ptr<VeComputePipeline> m_compute_pipeline;
		vk::raii::PipelineLayout m_pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> m_pipeline;

	};

} // namespace ve
