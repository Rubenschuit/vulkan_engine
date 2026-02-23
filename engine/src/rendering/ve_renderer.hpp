/* VeRenderer provides methods to to render the current frame.
It manages the swap chain and command buffers. Default present mode is immediate. */
#pragma once
#include "ve_export.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_swap_chain.hpp"
#include <memory>
#include <vector>


namespace ve {

class VENGINE_API VeRenderer {
public:
	VeRenderer(VeDevice& device, VeWindow& window);
	~VeRenderer();

	VeRenderer(const VeRenderer&) = delete;
	VeRenderer& operator=(const VeRenderer&) = delete;

	bool isFrameInProgress() const { return m_is_frame_started; }
	float getExtentAspectRatio() const;
	vk::Format getSwapChainImageFormat() const;
	vk::ColorSpaceKHR getSwapChainColorSpace() const;
	vk::Format getOffscreenImageFormat() const;
	size_t getImageCount() const;
	vk::Extent2D getExtent() const;
	vk::Extent2D getSwapChainExtent() const;
	uint32_t getCurrentFrame() const;
	uint32_t getCurrentImageIndex() const { assert(m_is_frame_started); return m_current_image_index; }
	vk::raii::CommandBuffer& getCurrentCommandBuffer();
	vk::raii::CommandBuffer& getCurrentComputeCommandBuffer();
	const vk::raii::ImageView& getSwapChainImageView(size_t index) const { return m_ve_swap_chain->getSwapChainImageViews()[index]; }
	const vk::raii::ImageView& getResolveTargetImageView() const { return m_ve_swap_chain->getResolveTargetImageView(); }
	const vk::raii::ImageView& getDepthImageView() const { return m_ve_swap_chain->getDepthImageView(); }
	const vk::raii::Image& getDepthImage() const { return m_ve_swap_chain->getDepthImage(); }
	/// Single-sample depth: resolved from MSAA prepass when MSAA active, otherwise same as depth
	const vk::raii::ImageView& getResolvedDepthImageView() const { return m_ve_swap_chain->getResolvedDepthImageView(); }
	const vk::raii::Image& getResolvedDepthImage() const { return m_ve_swap_chain->getResolvedDepthImage(); }
	bool isSwapChainOutOfDate() const { return m_swap_chain_needs_recreation; }

	// Begin a new frame. Returns true if a frame was acquired and recording can start.
	// When false is returned (e.g. swap chain out of date), the command buffer is not valid for use.
	bool beginFrame();
	void submitCompute(vk::raii::CommandBuffer& compute_command_buffer);
	void beginDepthPrePass(vk::raii::CommandBuffer& command_buffer);
	void endDepthPrePass(vk::raii::CommandBuffer& command_buffer);

	void beginSceneRender(vk::raii::CommandBuffer& command_buffer, bool load_depth = false);
	// Ends dynamic rendering for the scene but does not transition to Present.
	void endSceneRender(vk::raii::CommandBuffer& command_buffer);

	// Start rendering to the swapchain (editor_mode=false) or viewport image (editor_mode=true).
	void beginPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode = false);
	void endPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode = false);

	// In editor mode: prepare swapchain for ImGui rendering (transition + clear)
	void beginEditorUIRender(vk::raii::CommandBuffer& command_buffer);
	void endEditorUIRender(vk::raii::CommandBuffer& command_buffer);

	// Scene render extent (editor viewport resolution)
	void resizeSceneRender(uint32_t w, uint32_t h);
	void resetSceneRenderExtent();

	// Viewport image accessors
	VkImageView getViewportImageView() const;
	VkSampler getViewportSampler() const;
	void resizeViewportImage(uint32_t width, uint32_t height);
	uint32_t getViewportWidth() const { return m_viewport_image ? m_viewport_image->getWidth() : 0; }
	uint32_t getViewportHeight() const { return m_viewport_image ? m_viewport_image->getHeight() : 0; }

	// Transition the current swapchain image to PresentSrcKHR, submits and presents it,
	// and advances the current frame.
	void endFrame(vk::raii::CommandBuffer& command_buffer);

	// only max or none MSAA supported for now
	void setMSAAEnabled(bool enabled) { m_msaa_enabled = enabled; m_desired_num_samples = enabled ? m_ve_device.getSampleCount() : vk::SampleCountFlagBits::e1; m_swap_chain_needs_recreation = true; }
	void setSampleCount(vk::SampleCountFlagBits sample_count) {
		// Clamp to device max
		vk::SampleCountFlagBits max_samples = m_ve_device.getSampleCount();
		m_desired_num_samples = (sample_count > max_samples) ? max_samples : sample_count;
		m_msaa_enabled = (m_desired_num_samples != vk::SampleCountFlagBits::e1);
		m_swap_chain_needs_recreation = true;
	}
	vk::SampleCountFlagBits getSampleCount() const { return m_desired_num_samples; }
	vk::SampleCountFlagBits getMaxSampleCount() const { return m_ve_device.getSampleCount(); }
	void setPresentMode(vk::PresentModeKHR present_mode) {
		m_present_mode = present_mode;
		m_swap_chain_needs_recreation = true;
		VE_LOGI("Present mode set to " + std::to_string(static_cast<int>(present_mode)) + " with MSAA " + std::to_string(static_cast<int>(m_desired_num_samples))); }
	void recreateSwapChain();
	void setSwapChainNeedsRecreation() { m_swap_chain_needs_recreation = true; }

	float getGpuTime() const { return m_gpu_time; }
	float getComputeGpuTime() const { return m_compute_gpu_time; }
	float getGpuOverlap() const { return m_gpu_overlap; }
	vk::QueryPool getQueryPool() const { return *m_query_pool; }
	uint32_t getComputeStartQuery() const { return getCurrentFrame() * 4; }

	bool hasHdrSupport() const { return m_ve_device.hasHdrColorSpaceExtension(); }
	void setHdrEnabled(bool enabled) { m_hdr_enabled = hasHdrSupport() && enabled; m_swap_chain_needs_recreation = true; }
	bool isHdrEnabled() const { return m_hdr_enabled; }

private:
	void createCommandBuffers();
	void createViewportResources();
	void transitionToPresent(vk::raii::CommandBuffer& command_buffer);

	VeDevice& m_ve_device;
	VeWindow& m_ve_window;
	std::unique_ptr<VeSwapChain> m_ve_swap_chain;
	std::vector<vk::raii::CommandBuffer> m_command_buffers;
	std::vector<vk::raii::CommandBuffer> m_compute_command_buffers;

	vk::PresentModeKHR m_present_mode = vk::PresentModeKHR::eImmediate;
	uint32_t m_current_image_index;
	bool m_is_frame_started = false;
	bool m_swap_chain_needs_recreation = false;

	bool m_msaa_enabled = false;
	bool m_hdr_enabled = false;
	vk::SampleCountFlagBits m_desired_num_samples = vk::SampleCountFlagBits::e1;
	vk::Extent2D m_scene_render_extent{0, 0};  // 0 = use swapchain extent

	vk::raii::QueryPool m_query_pool = nullptr;
	float m_gpu_time = 0.0f;         // graphics-only GPU time
	float m_compute_gpu_time = 0.0f; // compute-only GPU time
	float m_gpu_overlap = 0.0f;      // compute/graphics overlap
	std::vector<bool> m_query_active;

	// Viewport image for editor mode (render-to-texture)
	std::unique_ptr<VeImage> m_viewport_image;
	vk::raii::Sampler m_viewport_sampler{nullptr};
};
}

