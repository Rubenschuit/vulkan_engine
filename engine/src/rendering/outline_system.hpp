/*
 * OutlineSystem: Jump Flood Algorithm (JFA) outline rendering.
 * Renders a uniform-width outline around target meshes (e.g. editor selection).
 *
 * 1. Mask pass (graphics): rasterize target meshes into R8_UNORM silhouette
 * 2. JFA init  (compute):  seed JFA texture from mask
 * 3. JFA steps (compute):  flood nearest-seed coordinates (ping-pong, capped to outline width)
 * 4. Composite (graphics):  alpha-blend outline onto post-processed image
 */
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <array>
#include <filesystem>

namespace ve {
class VeDevice;
class VeImage;
class VePipeline;
class VeDescriptorPool;
class VeDescriptorSetLayout;
}

namespace ve {

class VENGINE_API OutlineSystem {
public:
	OutlineSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		std::filesystem::path shader_path,
		vk::Extent2D extent,
		vk::Format composite_color_format);
	~OutlineSystem();

	OutlineSystem(const OutlineSystem&) = delete;
	OutlineSystem& operator=(const OutlineSystem&) = delete;

	// Collects selected entity + all descendants with MeshComponent.
	void renderMask(VeFrameInfo& frame_info, Registry& registry, Entity root_entity);

	// Called on graphics cmd buffer after mask.
	void dispatchJFA(VeFrameInfo& frame_info, float outline_width);

	// Called inside post-process render pass.
	void composite(vk::raii::CommandBuffer& command_buffer, uint32_t frame,
	               float outline_width, const glm::vec3& outline_color);

	void recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D extent,
	              vk::Format composite_color_format);

	bool hasOutline() const { return m_has_outline; }

private:
	void createImages(vk::Extent2D extent);
	void createSampler();
	void createMaskPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createMaskPipeline();
	void createJfaSetLayouts();
	void createJfaPipelineLayouts();
	void createJfaPipelines();
	void createCompositeSetLayout();
	void createCompositePipelineLayout();
	void createCompositePipeline(vk::Format color_format);
	void createDescriptorSets(VeDescriptorPool& descriptor_pool);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Extent2D m_extent{};
	bool m_has_outline = false;
	bool m_final_reads_a = true;

	// --- Images (per-frame) ---
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_mask_images;
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_jfa_images_a;
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_jfa_images_b;

	// --- Sampler ---
	vk::raii::Sampler m_nearest_sampler{nullptr};

	// --- Mask pass (graphics) ---
	vk::raii::PipelineLayout m_mask_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_mask_pipeline;

	// --- JFA Init (compute) ---
	std::unique_ptr<VeDescriptorSetLayout> m_jfa_init_set_layout;
	vk::raii::PipelineLayout m_jfa_init_pipeline_layout{nullptr};
	vk::raii::Pipeline m_jfa_init_pipeline{nullptr};
	vk::raii::ShaderModule m_jfa_init_module{nullptr};

	// --- JFA Step (compute) ---
	std::unique_ptr<VeDescriptorSetLayout> m_jfa_step_set_layout;
	vk::raii::PipelineLayout m_jfa_step_pipeline_layout{nullptr};
	vk::raii::Pipeline m_jfa_step_pipeline{nullptr};
	vk::raii::ShaderModule m_jfa_step_module{nullptr};

	// --- Composite (graphics) ---
	std::unique_ptr<VeDescriptorSetLayout> m_composite_set_layout;
	vk::raii::PipelineLayout m_composite_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_composite_pipeline;

	// --- Descriptor sets (per-frame) ---
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_jfa_init_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_jfa_step_a_to_b_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_jfa_step_b_to_a_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	// Composite reads from whichever JFA buffer has the final result
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_composite_a_sets =
		makeNullArray<vk::raii::DescriptorSet>();
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_composite_b_sets =
		makeNullArray<vk::raii::DescriptorSet>();
};

} // namespace ve