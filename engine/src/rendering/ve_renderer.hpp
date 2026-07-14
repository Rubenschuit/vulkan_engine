/* VeRenderer owns the swap chain, per-frame command buffers, and the frame
lifecycle: begin/end frame, render-pass begin/end pairs, split submission for
the async-compute path, and present. Also exposes display settings
(MSAA/VSync/HDR) and the frame profiler. Default present mode is immediate. 
*/
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
#include <filesystem>
#include <memory>
#include <vector>


namespace ve {

class VeResourceManager;
class EventBus;
class VeBuffer;

enum class HDRColorMode : int {
	SDR = 0,
	SCRGB = 1,        // Extended sRGB linear (scRGB)
	HDR10_PQ = 2,     // HDR10 ST2084 (PQ)
};

class VENGINE_API VeRenderer {
public:
	VeRenderer(VeDevice& device, VeWindow& window, VeResourceManager& resource_manager, EventBus& event_bus);
	~VeRenderer();

	VeRenderer(const VeRenderer&) = delete;
	VeRenderer& operator=(const VeRenderer&) = delete;

	// --- Frame lifecycle ---

	// Begin a new frame: waits on the frame fence and resets per-frame state
	bool beginFrame();
	// Acquires the swapchain image if not yet acquired this frame.
	// false = frame aborted.
	bool ensureImageAcquired();
	// End scene + UI command buffers, submit both, present, and advance the frame.
	void endFrame();

	void markSceneFrame() { m_scene_frame = true; }
	bool isFrameInProgress() const { return m_is_frame_started; }
	bool isSwapChainOutOfDate() const { return m_swap_chain_needs_recreation; }

	// --- Render passes, in frame order ---

	void beginGeometryPrePass(vk::raii::CommandBuffer& command_buffer,
		bool secondary_contents = false, bool clear = true);
	void endGeometryPrePass(vk::raii::CommandBuffer& command_buffer);
	void beginSceneRender(vk::raii::CommandBuffer& command_buffer,
		bool load_depth = false, bool secondary_contents = false, bool resolve_msaa = true);
	void endSceneRender(vk::raii::CommandBuffer& command_buffer);
	void beginWboitRender(vk::raii::CommandBuffer& command_buffer);
	void endWboitRender(vk::raii::CommandBuffer& command_buffer);
	void beginWboitComposite(vk::raii::CommandBuffer& command_buffer);
	void endWboitComposite(vk::raii::CommandBuffer& command_buffer);
	// Render to the swapchain (editor_mode=false) or viewport image (editor_mode=true).
	void beginPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode = false);
	void endPostProcessRender(vk::raii::CommandBuffer& command_buffer, bool editor_mode = false);
	void beginUIRecording(bool editor_mode);

	// --- Submission ---

	void submitCompute(vk::raii::CommandBuffer& compute_command_buffer);
	// Scene-frame split submission. Called mid-frame from the async path.
	void submitPreSwapGraphics(bool depth_compute_follows);
	void submitShadowGraphics(vk::raii::CommandBuffer& shadow_cb);
	void submitDepthCompute(vk::raii::CommandBuffer& depth_compute_cb);

	// --- Command buffers ---

	vk::raii::CommandBuffer& getCurrentCommandBuffer();
	vk::raii::CommandBuffer& getCurrentComputeCommandBuffer();
	vk::raii::CommandBuffer& getShadowGraphicsCommandBuffer();
	vk::raii::CommandBuffer& getSwapGraphicsCommandBuffer();
	vk::raii::CommandBuffer& getDepthComputeCommandBuffer();
	vk::raii::CommandBuffer& getCurrentUICommandBuffer();
	CommandResourceManager& getCommandManager() { return m_command_manager; }
	VeThreadPool& getThreadPool() { return *m_thread_pool; }
	vk::CommandBufferInheritanceRenderingInfo getSceneInheritanceInfo() const;

	// --- Swap chain and render target queries ---

	float getExtentAspectRatio() const;
	vk::Format getSwapChainImageFormat() const;
	vk::ColorSpaceKHR getSwapChainColorSpace() const;
	vk::Format getOffscreenImageFormat() const;
	size_t getImageCount() const;
	vk::Extent2D getExtent() const;
	vk::Extent2D getSwapChainExtent() const;
	uint32_t getCurrentFrame() const;
	uint32_t getCurrentImageIndex() const { assert(m_is_frame_started); return m_current_image_index; }
	const vk::raii::ImageView& getSwapChainImageView(size_t index) const { return m_ve_swap_chain->getSwapChainImageViews()[index]; }
	const vk::raii::ImageView& getResolveTargetImageView() const { return m_ve_swap_chain->getResolveTargetImageView(); }
	vk::Image getResolveTargetImage() const { return m_ve_swap_chain->getResolveTargetImage(); }
	const vk::raii::ImageView& getDepthImageView() const { return m_ve_swap_chain->getDepthImageView(); }
	vk::Image getDepthImage() const { return m_ve_swap_chain->getDepthImage(); }
	// Single-sample depth: resolved from MSAA prepass when MSAA active, otherwise same as depth
	const vk::raii::ImageView& getResolvedDepthImageView() const { return m_ve_swap_chain->getResolvedDepthImageView(); }
	vk::Image getResolvedDepthImage() const { return m_ve_swap_chain->getResolvedDepthImage(); }
	bool hasResolvedDepth() const { return m_ve_swap_chain->hasResolvedDepth(); }
	const vk::raii::ImageView& getResolvedNormalRoughnessImageView() const { return m_ve_swap_chain->getResolvedNormalRoughnessImageView(); }
	vk::Image getResolvedNormalRoughnessImage() const { return m_ve_swap_chain->getResolvedNormalRoughnessImage(); }
	const vk::raii::ImageView& getWboitAccumImageView() const { return m_wboit_accum->getImageView(); }
	const vk::raii::ImageView& getWboitRevealageImageView() const { return m_wboit_revealage->getImageView(); }

	// --- Editor viewport ---

	// Scene render extent (editor viewport resolution)
	void resizeSceneRender(uint32_t w, uint32_t h);
	void resetSceneRenderExtent();
	VkImageView getViewportImageView() const;
	VkSampler getViewportSampler() const;
	void resizeViewportImage(uint32_t width, uint32_t height);
	uint32_t getViewportWidth() const { return m_viewport_image ? m_viewport_image->getWidth() : 0; }
	uint32_t getViewportHeight() const { return m_viewport_image ? m_viewport_image->getHeight() : 0; }

	// --- Display settings (MSAA / VSync / HDR) ---

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
	// App-facing wrappers
	std::vector<int> getAvailableSampleCounts() const;
	int getCurrentSampleCountInt() const { return static_cast<int>(m_desired_num_samples); }
	void setSampleCountInt(int count);
	void setVSync(bool enabled) { setPresentMode(enabled ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate); }
	bool hasHdrSupport() const { return m_ve_device.hasHdrColorSpaceExtension(); }
	void setHdrEnabled(bool enabled) { m_hdr_enabled = hasHdrSupport() && enabled; m_swap_chain_needs_recreation = true; }
	bool isHdrEnabled() const { return m_hdr_enabled; }
	HDRColorMode getHDRColorMode() const;
	const char* getHDRColorModeString() const;
	void recreateSwapChain();
	void setSwapChainNeedsRecreation() { m_swap_chain_needs_recreation = true; }

	// --- Screenshot ---

	// Queues a swapchain readback: the copy is recorded in the next
	// scene frame's endFrame, which then blocks on GPU completion and writes
	// an 8-bit PNG.
	void requestScreenshot(std::filesystem::path path) { m_screenshot_path = std::move(path); }

	// --- Device and profiling ---

	std::string getDeviceName() const { return m_ve_device.getDeviceProperties().deviceName.data(); }
	VmaAllocator getAllocator() const { return m_ve_device.getAllocator(); }
	uint32_t getMemoryHeapCount() { return m_ve_device.getPhysicalDevice().getMemoryProperties().memoryHeapCount; }
	vk::PhysicalDeviceMemoryProperties getMemoryProperties() { return m_ve_device.getPhysicalDevice().getMemoryProperties(); }
	void waitIdle() { m_ve_device.getDevice().waitIdle(); }
	TracyVkCtx getTracyGraphicsCtx() { return m_tracy_graphics_ctx; }
	TracyVkCtx getTracyComputeCtx() { return m_tracy_compute_ctx; }
	FrameProfiler& getProfiler() { return m_profiler; }
	const ProfileResults& getProfileResults() const { return m_profiler.getResults(); }
	float getGpuTime() const { return m_profiler.getResults().gpu(ProfileTimer::FRAME_TOTAL); }
	float getComputeGpuTime() const { return m_profiler.getResults().gpu(ProfileTimer::COMPUTE_TOTAL); }
	float getGpuOverlap() const { return m_profiler.getResults().gpu_overlap; }

private:
	void createViewportResources();
	void recreateWboitImages();
	void transitionToPresent(vk::raii::CommandBuffer& command_buffer);
	void recordScreenshotCopy(vk::raii::CommandBuffer& command_buffer);
	void writeScreenshotPng();

	VeDevice& m_ve_device;
	CommandResourceManager m_command_manager;
	VeWindow& m_ve_window;
	VeResourceManager& m_resource_manager;
	EventBus& m_event_bus;
	std::unique_ptr<VeSwapChain> m_ve_swap_chain;

	vk::PresentModeKHR m_present_mode = vk::PresentModeKHR::eImmediate;
	uint32_t m_current_image_index;
	bool m_is_frame_started = false;
	bool m_swap_chain_needs_recreation = false;
	bool m_image_acquired_this_frame = false;
	bool m_frame_aborted = false;
	bool m_pre_swap_submitted_this_frame = false;
	bool m_scene_frame = false;
	bool m_ui_label_open = false;

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

	// Screenshot
	std::filesystem::path m_screenshot_path;
	std::unique_ptr<VeBuffer> m_screenshot_buffer;
	vk::Extent2D m_screenshot_extent{};
	bool m_screenshot_bgra = false;
};
}

