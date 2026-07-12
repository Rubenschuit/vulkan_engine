// Screen-space reflections: Hi-Z traversal of a min/max pyramid built from
// the current-frame depth (SsrHizPyramid) along reflection rays from the
// prepass normals, fetching radiance from the previous frame's HDR history
// (reprojected via prev_projection_view). Output is rgb radiance + confidence
// in alpha, composited into the IBL specular term by the PBR shader
#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "rendering/ve_frame_info.hpp"

#include <memory>
#include <filesystem>

namespace ve {

class EventBus;
class SsrHizPyramid;

class VENGINE_API SsrSystem {
public:
	SsrSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		std::filesystem::path shader_path,
		vk::Extent2D ssr_extent,
		vk::Extent2D full_extent,
		vk::Format color_format,
		const vk::raii::ImageView& depth_image_view,
		const vk::raii::ImageView& normal_roughness_image_view,
		EventBus& event_bus);
	~SsrSystem();

	SsrSystem(const SsrSystem&) = delete;
	SsrSystem& operator=(const SsrSystem&) = delete;

	// Record the trace dispatch. Must run on the graphics timeline (history
	// copy ordering) while depth and the normal target are in their
	// compute-read layouts, before scene render.
	void dispatch(VeFrameInfo& frame_info, vk::raii::CommandBuffer& cmd);

	// Copies the resolve target into the history image and rebuilds its mip
	// chain.
	// Expects the resolve target in eShaderReadOnlyOptimal and returns both images
	// there. Must be recorded on the graphics queue
	void recordHistoryCopy(vk::raii::CommandBuffer& command_buffer, vk::Image resolve_target);
	void invalidateHistory() { m_history_valid = false; }
	bool historyValid() const { return m_history_valid; }

	const vk::raii::DescriptorSetLayout& getSsrSetLayout() const {
		return m_output_set_layout->getDescriptorSetLayout();
	}
	vk::raii::DescriptorSet& getOutputDescriptorSet() { return m_output_descriptor_set; }
	// Zero radiance + zero confidence: the IBL composite lerp becomes an identity
	vk::raii::DescriptorSet& getDummyOutputDescriptorSet() { return m_dummy_output_descriptor_set; }

private:
	void createHistoryImage();
	void createOutputImage();
	void createResolvedImage();
	void createDummyImage();
	void createSetLayouts();
	void createSampler();
	void createPipeline(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createDescriptorSets(VeDescriptorPool& descriptor_pool);

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Extent2D m_ssr_extent;    // trace resolution (may be half-res)
	vk::Extent2D m_full_extent;   // history / depth / normal resolution
	vk::Format m_format;

	// Trace parameters
	int m_max_steps = 48;
	float m_thickness = 0.3f;
	float m_max_roughness = 0.85f;
	float m_max_distance = 25.0f;

	std::unique_ptr<VeImage> m_history_image;
	std::unique_ptr<VeImage> m_output_image;
	std::unique_ptr<VeImage> m_resolved_image;
	std::unique_ptr<VeImage> m_dummy_image;
	std::unique_ptr<SsrHizPyramid> m_hiz_pyramid;
	bool m_history_valid = false;

	vk::raii::Sampler m_linear_clamp_sampler{nullptr};

	std::unique_ptr<VeDescriptorSetLayout> m_io_set_layout;       // trace inputs + output storage
	std::unique_ptr<VeDescriptorSetLayout> m_resolve_set_layout;  // raw + depth + resolved storage
	std::unique_ptr<VeDescriptorSetLayout> m_output_set_layout;   // sampled + sampler

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	vk::raii::Pipeline m_pipeline{nullptr};
	vk::raii::ShaderModule m_shader_module{nullptr};
	vk::raii::PipelineLayout m_resolve_pipeline_layout{nullptr};
	vk::raii::Pipeline m_resolve_pipeline{nullptr};
	vk::raii::ShaderModule m_resolve_shader_module{nullptr};

	vk::raii::DescriptorSet m_io_descriptor_set{nullptr};
	vk::raii::DescriptorSet m_resolve_descriptor_set{nullptr};
	vk::raii::DescriptorSet m_output_descriptor_set{nullptr};
	vk::raii::DescriptorSet m_dummy_output_descriptor_set{nullptr};

	// Cached single-sample views for descriptor rewrites
	vk::ImageView m_depth_image_view{};
	vk::ImageView m_normal_image_view{};
};

}
