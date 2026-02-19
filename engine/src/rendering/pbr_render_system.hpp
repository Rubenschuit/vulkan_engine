#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_material_properties.hpp"

#include <array>
#include <memory>
#include <vector>
#include <filesystem>

namespace ve {
	class VeDevice;
	class VePipeline;
	class VeMesh;
	class MeshComponent;
}

namespace ve {

class VENGINE_API PbrRenderSystem {
public:
	// Instanced draw groups: opaque drawables merged by (mesh, material) for batched draws
	struct InstanceGroup {
		VeMesh* mesh = nullptr;
		VkDescriptorSet material_set = VK_NULL_HANDLE;
		uint32_t first_instance = 0;  // offset into SSBO
		uint32_t instance_count = 0;
		float has_texture = 0.0f;
		float alpha_cutoff = 0.0f;
		uint32_t material_flags = 0;
		bool double_sided = false;
	};

	PbrRenderSystem(
		VeDevice& device,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path shader_path);
	~PbrRenderSystem();

	PbrRenderSystem(const PbrRenderSystem&) = delete;
	PbrRenderSystem& operator=(const PbrRenderSystem&) = delete;

	void renderObjects(VeFrameInfo& frame_info) const;
	/// Call prepareFrame once, then renderOpaque, then (e.g. skybox), then renderTransparent.
	void prepareFrame(VeFrameInfo& frame_info) const;
	void renderOpaque(VeFrameInfo& frame_info) const;
	void renderTransparent(VeFrameInfo& frame_info) const;
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		for (auto& p : m_pipelines) 
			p.reset();
		createPipelines(color_format, sample_count);
	}
	void setTopology(vk::PrimitiveTopology topology) {
		m_topology = topology;
	}

	const std::vector<InstanceGroup>& getOpaqueGroups() const { return m_opaque_groups; }
	void setDepthPrePassActive(bool active) { m_depth_prepass_active = active; }

private:
	bool m_depth_prepass_active = false;
	void createPipelineLayout(
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& material_set_layout,
		const vk::raii::DescriptorSetLayout& shadow_set_layout);
	void createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);
	void renderOpaqueGroup(VeFrameInfo& frame_info, const InstanceGroup& group,
		VkDescriptorSet& bound_material_set, VeMesh*& bound_mesh) const;

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::PrimitiveTopology m_topology = vk::PrimitiveTopology::eTriangleList;
	vk::Format m_color_format = vk::Format::eUndefined;
	vk::SampleCountFlagBits m_sample_count = vk::SampleCountFlagBits::e1;

	static constexpr uint32_t SHADOW_MODE_COUNT = 4;  // DISABLED, REGULAR, PCF, PCSS
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::array<std::unique_ptr<VePipeline>, SHADOW_MODE_COUNT> m_pipelines;

	struct Drawable {
		VkDescriptorSet material_set;
		Entity entity;
		MeshComponent* mesh = nullptr;
		VeMesh* mesh_ptr = nullptr;         // cached mesh->getMesh() — avoids pointer chase during sort
		VeMaterial* material_ptr = nullptr;  // cached mesh->getMaterial()
		float dist_sq = 0.0f;
		AlphaMode alpha_mode = AlphaMode::ALPHA_OPAQUE;
		uint32_t ssbo_index = 0;  // index into instance SSBO (set in prepareFrame)
	};
	mutable std::vector<Drawable> m_opaque_drawables;
	mutable std::vector<Drawable> m_transparent_drawables;
	mutable std::vector<InstanceGroup> m_opaque_groups;
};

} // namespace ve
