/* VeRenderer provides methods to to render the current frame.
It manages the swap chain and command buffers. Default present mode is immediate. */
#pragma once
#include "ve_export.hpp"
#include "ve_tracy.hpp"
#include "rendering/frame_profiler.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_command_resource_manager.hpp"
#include "vulkan/ve_thread_pool.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_swap_chain.hpp"
#include <memory>
#include <vector>


namespace ve {

enum class HDRColorMode : int;

class VENGINE_API VeRenderer {
public:
	VeRenderer(VeDevice& device, VeWindow& window);
	~VeRenderer();

	VeRenderer(const VeRenderer&) = delete;
	VeRenderer& operator=(const VeRenderer&) = delete;

	std::string getDeviceName() const { return m_ve_device.getDeviceProperties().deviceName.data(); }
	VmaAllocator getAllocator() const { return m_ve_device.getAllocator(); }
	uint32_t getMemoryHeapCount() { return m_ve_device.getPhysicalDevice().getMemoryProperties().memoryHeapCount; }
	vk::PhysicalDeviceMemoryProperties getMemoryProperties() { return m_ve_device.getPhysicalDevice().getMemoryProperties(); }
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
	vk::raii::CommandBuffer& getCurrentGraphics2CommandBuffer();
	vk::raii::CommandBuffer& getCurrentGraphics3CommandBuffer();
	vk::raii::CommandBuffer& getCurrentCompute2CommandBuffer();
	vk::raii::CommandBuffer& getCurrentUICommandBuffer();
	const vk::raii::ImageView& getSwapChainImageView(size_t index) const { return m_ve_swap_chain->getSwapChainImageViews()[index]; }
	const vk::raii::ImageView& getResolveTargetImageView() const { return m_ve_swap_chain->getResolveTargetImageView(); }
	const vk::raii::ImageView& getDepthImageView() const { return m_ve_swap_chain->getDepthImageView(); }
	vk::Image getDepthImage() const { return m_ve_swap_chain->getDepthImage(); }
	/// Single-sample depth: resolved from MSAA prepass when MSAA active, otherwise same as depth
	const vk::raii::ImageView& getResolvedDepthImageView() const { return m_ve_swap_chain->getResolvedDepthImageView(); }
	vk::Image getResolvedDepthImage() const { return m_ve_swap_chain->getResolvedDepthImage(); }
	bool isSwapChainOutOfDate() const { return m_swap_chain_needs_recreation; }

	// WBOIT image accessors
	const vk::raii::ImageView& getWboitAccumImageView() const { return m_wboit_accum->getImageView(); }
	const vk::raii::ImageView& getWboitRevealageImageView() const { return m_wboit_revealage->getImageView(); }

	// Begin a new frame. Returns true if a frame was acquired and recording can start.
	// When false is returned (e.g. swap chain out of date), the command buffer is not valid for use.
	bool beginFrame();
	void submitCompute(vk::raii::CommandBuffer& compute_command_buffer);
	void beginDepthPrePass(vk::raii::CommandBuffer& command_buffer,
		bool secondary_contents = false, bool clear = true);
	void endDepthPrePass(vk::raii::CommandBuffer& command_buffer);

	void beginSceneRender(vk::raii::CommandBuffer& command_buffer,
		bool load_depth = false, bool secondary_contents = false, bool resolve_msaa = true);
	void endSceneRender(vk::raii::CommandBuffer& command_buffer);

	// WBOIT rendering: geometry pass writes to accum + revealage with read-only depth
	void beginWboitRender(vk::raii::CommandBuffer& command_buffer);
	void endWboitRender(vk::raii::CommandBuffer& command_buffer);
	// WBOIT composite: alpha-blend result onto resolve target
	void beginWboitComposite(vk::raii::CommandBuffer& command_buffer);
	void endWboitComposite(vk::raii::CommandBuffer& command_buffer);

	// Start rendering to the swapchain (editor_mode=false) or viewport image (editor_mode=true).
	void beginPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode = false);
	void endPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode = false);

	// Prepare the UI command buffer for recording (barrier/transition).
	// Call before ImGuiLayer::renderUI().
	void beginUIRecording(bool editor_mode);

	// Scene render extent (editor viewport resolution)
	void resizeSceneRender(uint32_t w, uint32_t h);
	void resetSceneRenderExtent();

	// Viewport image accessors
	VkImageView getViewportImageView() const;
	VkSampler getViewportSampler() const;
	void resizeViewportImage(uint32_t width, uint32_t height);
	uint32_t getViewportWidth() const { return m_viewport_image ? m_viewport_image->getWidth() : 0; }
	uint32_t getViewportHeight() const { return m_viewport_image ? m_viewport_image->getHeight() : 0; }

	// End scene + UI command buffers, submit both, present, and advance the frame.
	void endFrame();

	// Split async submission
	void submitGraphicsPhase1();
	void submitShadowPhase(vk::raii::CommandBuffer& shadow_cb);
	void submitComputePhase2(vk::raii::CommandBuffer& compute2_cb);
	void setSplitActive(bool active);

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

	// --- App-facing wrappers ---
	std::vector<int> getAvailableSampleCounts() const;
	int getCurrentSampleCountInt() const { return static_cast<int>(m_desired_num_samples); }
	void setSampleCountInt(int count);
	void setVSync(bool enabled) { setPresentMode(enabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate); }
	HDRColorMode getHDRColorMode() const;
	const char* getHDRColorModeString() const;
	void recreateSwapChain();
	void setSwapChainNeedsRecreation() { m_swap_chain_needs_recreation = true; }

	void waitIdle() { m_ve_device.getDevice().waitIdle(); }
	CommandResourceManager& getCommandManager() { return m_command_manager; }
	VeThreadPool& getThreadPool() { return *m_thread_pool; }

	/// Inheritance info for secondary CBs recording inside the scene render pass.
	vk::CommandBufferInheritanceRenderingInfo getSceneInheritanceInfo() const;
	/// Inheritance info for secondary CBs recording inside the depth prepass.
	vk::CommandBufferInheritanceRenderingInfo getDepthPrepassInheritanceInfo() const;

	TracyVkCtx getTracyGraphicsCtx() { return m_tracy_graphics_ctx; }
	TracyVkCtx getTracyComputeCtx() { return m_tracy_compute_ctx; }

	FrameProfiler& getProfiler() { return m_profiler; }
	const ProfileResults& getProfileResults() const { return m_profiler.getResults(); }
	float getGpuTime() const { return m_profiler.getResults().gpu(ProfileTimer::FRAME_TOTAL); }
	float getComputeGpuTime() const { return m_profiler.getResults().gpu(ProfileTimer::COMPUTE_TOTAL); }
	float getGpuOverlap() const { return m_profiler.getResults().gpu_overlap; }

	bool hasHdrSupport() const { return m_ve_device.hasHdrColorSpaceExtension(); }
	void setHdrEnabled(bool enabled) { m_hdr_enabled = hasHdrSupport() && enabled; m_swap_chain_needs_recreation = true; }
	bool isHdrEnabled() const { return m_hdr_enabled; }

private:
	void createViewportResources();
	void recreateWboitImages();
	void transitionToPresent(vk::raii::CommandBuffer& command_buffer);

	VeDevice& m_ve_device;
	CommandResourceManager m_command_manager;
	VeWindow& m_ve_window;
	std::unique_ptr<VeSwapChain> m_ve_swap_chain;

	vk::PresentModeKHR m_present_mode = vk::PresentModeKHR::eImmediate;
	uint32_t m_current_image_index;
	bool m_is_frame_started = false;
	bool m_swap_chain_needs_recreation = false;
	bool m_split_active = false;

	bool m_msaa_enabled = false;
	bool m_hdr_enabled = false;
	vk::SampleCountFlagBits m_desired_num_samples = vk::SampleCountFlagBits::e1;
	vk::Extent2D m_scene_render_extent{0, 0};  // 0 = use swapchain extent

	FrameProfiler m_profiler;

	// Tracy GPU profiling contexts
	TracyVkCtx m_tracy_graphics_ctx = nullptr;
	TracyVkCtx m_tracy_compute_ctx = nullptr;

	// Multi-threaded command recording
	std::unique_ptr<VeThreadPool> m_thread_pool;
	vk::Format m_depth_format = vk::Format::eUndefined;
	vk::Format m_scene_color_format = vk::Format::eUndefined; // cached for inheritance info pointer stability

	// WBOIT render targets (single-sample)
	std::unique_ptr<VeImage> m_wboit_accum;      // RGBA16F
	std::unique_ptr<VeImage> m_wboit_revealage;  // R16F

	// Viewport image for editor mode (render-to-texture)
	std::unique_ptr<VeImage> m_viewport_image;
	vk::raii::Sampler m_viewport_sampler{nullptr};
};
}

