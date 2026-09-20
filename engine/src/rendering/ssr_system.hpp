// Screen-space reflections: Hi-Z traversal of the ScreenRayResources min/max
// depth pyramid along reflection rays from the prepass normals, fetching
// radiance from its previous-frame HDR history (reprojected via
// prev_projection_view). Output is rgb radiance + confidence in alpha,
// composited into the IBL specular term by the PBR shader
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
class ScreenRayResources;

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
		const ScreenRayResources& screen_rays,
		EventBus& event_bus);
	~SsrSystem();

	SsrSystem(const SsrSystem&) = delete;
	SsrSystem& operator=(const SsrSystem&) = delete;

	void dispatch(VeFrameInfo& frame_info, vk::raii::CommandBuffer& cmd, bool async);

	// Hands the consumed image to the PBR fragment stage. Record on the
	// graphics command buffer before scene render.
	void acquireForRead(vk::raii::CommandBuffer& cmd);

	const vk::raii::DescriptorSetLayout& getSsrSetLayout() const {
		return m_output_set_layout->getDescriptorSetLayout();
	}
	vk::raii::DescriptorSet& getOutputDescriptorSet() { return m_output_descriptor_set; }
	// Zero radiance + zero confidence: the IBL composite lerp becomes an identity
	vk::raii::DescriptorSet& getDummyOutputDescriptorSet() { return m_dummy_output_descriptor_set; }

private:
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
	vk::Extent2D m_full_extent;   // depth / normal resolution
	vk::Format m_format;
	const ScreenRayResources& m_screen_rays;

	// Trace parameters
	int m_max_steps = 48;
	float m_thickness = 0.3f;
	float m_max_roughness = 0.4f; // overwritten from RenderSettings on the first SettingsWatcher tick
	float m_max_distance = 55.0f;

	std::unique_ptr<VeImage> m_output_image;
	std::unique_ptr<VeImage> m_resolved_image;
	std::unique_ptr<VeImage> m_dummy_image;
	bool m_resolve_active = false;

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
