#pragma once
#include "ve_config.hpp"
#include "ve_buffer.hpp"
#include "ve_descriptors.hpp"
#include "ve_frame_info.hpp"
#include "ve_pipeline.hpp"
#include "ve_compute_pipeline.hpp"

#include <memory>
#include <vector>
#include <atomic>
#include <atomic>

namespace ve {

	struct ParticleParams {
		float delta_time;
		float total_time = 0.0f;
		uint32_t particle_count;
		uint32_t reset; // 1 = reset particles this dispatch
		uint32_t seed;  // rng seed for reset
		uint32_t reset_kind = 1u; // 1 = point, 2 = disc
		int32_t mode = 4; // default to galaxyMassive
		alignas(16) glm::vec3 origin;
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
					uint32_t particle_count,
					glm::vec3 origin);
		~ParticleSystem();

		ParticleSystem(const ParticleSystem&) = delete;
		ParticleSystem& operator=(const ParticleSystem&) = delete;

		void update(VeFrameInfo& frame_info);
		void render(VeFrameInfo& frame_info) const;
		void restart(); // schedule GPU reset of particle positions
        void setMode(int32_t mode) { m_mode = mode; }
        void resetPoint() { m_reset_kind = 1u; restart(); }
        void resetDisc() { m_reset_kind = 2u; restart(); }


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
		float m_total_time = 0.0f;
		glm::vec3 m_origin{0.0f, 0.0f, 10.0f};

		// Descriptor layouts for this system
		std::unique_ptr<VeDescriptorSetLayout> m_compute_set_layout;

		// Per-frame resources
		std::vector<std::unique_ptr<VeBuffer>> m_compute_uniform_buffers;  // small UBO per frame
		std::vector<std::unique_ptr<VeBuffer>> m_shader_storage_buffers; // large SSBO per frame
		std::vector<vk::raii::DescriptorSet> m_compute_descriptor_sets;

		// Reset control
		// atomic not necessary (no multi-threading yet)
		std::atomic<bool> m_pending_reset{false};
		uint32_t m_reset_seed{0};
		uint32_t m_reset_kind{1u}; // 1=point, 2=disc
		int32_t m_mode{1}; // 1..4 modes

		// Shared pool for descriptor allocations (shared to ensure lifetime across systems)
		std::shared_ptr<VeDescriptorPool> m_descriptor_pool;

		//
		vk::raii::PipelineLayout m_compute_pipeline_layout{nullptr};
		std::unique_ptr<VeComputePipeline> m_compute_pipeline;
		vk::raii::PipelineLayout m_pipeline_layout{nullptr};
		std::unique_ptr<VePipeline> m_pipeline;

	};

} // namespace ve
