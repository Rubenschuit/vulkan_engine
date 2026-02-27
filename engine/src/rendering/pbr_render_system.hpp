#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "rendering/pbr_mega_buffer.hpp"
#include "resources/ve_material_properties.hpp"

#include <array>
#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VePipeline;
	class VeBuffer;
	class VeMesh;
	class MeshComponent;
	class BindlessTextureRegistry;
	class MaterialSSBOManager;
}

namespace ve {

class VENGINE_API PbrRenderSystem {
public:
	PbrRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& bindless_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
		const vk::raii::DescriptorSetLayout& cluster_set_layout,
		const vk::raii::DescriptorSetLayout& ao_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
	~PbrRenderSystem();

	PbrRenderSystem(const PbrRenderSystem&) = delete;
	PbrRenderSystem& operator=(const PbrRenderSystem&) = delete;

	// Build the mega buffer from all meshes in the scene. Call once after scene load.
	void buildMegaBuffer(vk::raii::CommandBuffer& cmd, const std::vector<VeMesh*>& meshes);

	void prepareFrame(VeFrameInfo& frame_info, MaterialSSBOManager& mat_mgr) const;
	void prepareTransparents(VeFrameInfo& frame_info, MaterialSSBOManager& mat_mgr) const;
	void renderOpaque(VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set) const;
	void renderOpaqueGpuCulled(VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
	                           const VeBuffer& indirect_buffer, const VeBuffer& count_buffer,
	                           uint32_t bucket_stride, uint32_t max_draw_count,
	                           bool use_draw_count) const;
	void renderTransparent(VeFrameInfo& frame_info, const vk::raii::DescriptorSet& bindless_set,
	                       const vk::raii::DescriptorSet* global_set_override = nullptr) const;

	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		for (auto& p : m_pipelines)
			p.reset();
		for (auto& p : m_pipelines_mask)
			p.reset();
		createPipelines(color_format, sample_count);
	}
	void setTopology(Topology topo) {
		m_topology = (topo == Topology::LINE_LIST)
			? vk::PrimitiveTopology::eLineList
			: vk::PrimitiveTopology::eTriangleList;
	}
	void setShadowSamples(uint32_t pcf_samples, uint32_t pcss_filter_samples) {
		m_pcf_samples = pcf_samples;
		m_pcss_filter_samples = pcss_filter_samples;
		for (auto& p : m_pipelines)
			p.reset();
		for (auto& p : m_pipelines_mask)
			p.reset();
		createPipelines(m_color_format, m_sample_count);
	}

	uint32_t getOpaqueDrawCount() const { return m_total_indirect_count; }
	void setDepthPrePassActive(bool active) { m_depth_prepass_active = active; }

	PbrMegaBuffer& getMegaBuffer() { return *m_mega_buffer; }
	const PbrMegaBuffer& getMegaBuffer() const { return *m_mega_buffer; }

	void resetMegaBuffer() { m_mega_buffer->clear(); m_total_indirect_count = 0; }

	// Depth prepass uses buckets 0-1 from the main indirect buffer
	const uint32_t* getDepthBucketOffsets() const { return m_bucket_offsets; }
	const uint32_t* getDepthBucketCounts() const { return m_bucket_counts; }
	const VeBuffer& getIndirectBuffer(uint32_t frame) const { return *m_indirect_buffers[frame]; }

private:
	bool m_depth_prepass_active = false;
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& bindless_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_mask_set_layout,
		const vk::raii::DescriptorSetLayout& cluster_set_layout,
		const vk::raii::DescriptorSetLayout& ao_set_layout);
	void createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::PrimitiveTopology m_topology = vk::PrimitiveTopology::eTriangleList;
	vk::Format m_color_format = vk::Format::eUndefined;
	vk::SampleCountFlagBits m_sample_count = vk::SampleCountFlagBits::e1;

	uint32_t m_pcf_samples = 8;
	uint32_t m_pcss_filter_samples = 16;
	static constexpr uint32_t SHADOW_MODE_COUNT = 4;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::array<std::unique_ptr<VePipeline>, SHADOW_MODE_COUNT> m_pipelines;
	std::array<std::unique_ptr<VePipeline>, SHADOW_MODE_COUNT> m_pipelines_mask;

	std::unique_ptr<PbrMegaBuffer> m_mega_buffer;

	// Indirect draw (opaque): 4 buckets (non-MASK back, non-MASK double, MASK back, MASK double)
	// Depth prepass uses buckets 0-1 (non-MASK only) from the same buffer.
	static constexpr uint32_t BUCKET_COUNT = 4;
	mutable std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_indirect_buffers;
	mutable uint32_t m_bucket_offsets[BUCKET_COUNT]{};
	mutable uint32_t m_bucket_counts[BUCKET_COUNT]{};
	mutable uint32_t m_total_indirect_count = 0;

	// Persistent scratch vectors (avoid per-frame heap allocation)
	mutable std::vector<VkDrawIndexedIndirectCommand> m_indirect_cmds;

	struct Drawable {
		Entity entity;
		MeshComponent* mesh = nullptr;
		VeMesh* mesh_ptr = nullptr;
		VeMaterial* material_ptr = nullptr;
		float dist_sq = 0.0f;
		AlphaMode alpha_mode = AlphaMode::ALPHA_OPAQUE;
		bool double_sided = false;
		uint32_t ssbo_index = 0;
		uint32_t lod_level = 0;
	};
	mutable std::vector<Drawable> m_opaque_drawables;
	mutable std::vector<Drawable> m_transparent_drawables;
};

} // namespace ve
