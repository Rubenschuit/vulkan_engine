/* VeRenderer provides methods to to render the current frame.
It manages the swap chain and command buffers. */
#pragma once
#include "ve_export.hpp"
#include "ve_device.hpp"
#include "ve_window.hpp"
#include "ve_swap_chain.hpp"
#include <memory>
#include <vector>


namespace ve {

class VENGINE_API VeRenderer {
public:
	VeRenderer(VeDevice& device, VeWindow& window);
	~VeRenderer();

	//destroy copy and move constructors and assignment operators
	VeRenderer(const VeRenderer&) = delete;
	VeRenderer& operator=(const VeRenderer&) = delete;

	bool isFrameInProgress() const { return m_is_frame_started; }
		float getExtentAspectRatio() const;
		vk::Format getSwapChainImageFormat() const;
		size_t getImageCount() const;
		vk::Extent2D getExtent() const;
		uint32_t getCurrentFrame() const;
		uint32_t getCurrentImageIndex() const { assert(m_is_frame_started); return m_current_image_index; }
		vk::raii::CommandBuffer& getCurrentCommandBuffer();
		vk::raii::CommandBuffer& getCurrentComputeCommandBuffer();
		const vk::raii::ImageView& getSwapChainImageView(size_t index) const { return m_ve_swap_chain->getSwapChainImageViews()[index]; }

	// Begin a new frame. Returns true if a frame was acquired and recording can start.
	// When false is returned (e.g. swap chain out of date), no command buffer is valid for use.
	bool beginFrame();
	// Simply submits compute work for the current frame.
	void submitCompute(vk::raii::CommandBuffer& compute_command_buffer);
	// Begin dynamic rendering for the scene.
	void beginSceneRender(vk::raii::CommandBuffer& command_buffer);
	// Ends dynamic rendering for the scene but does not transition to Present.
	void endSceneRender(vk::raii::CommandBuffer& command_buffer);
	// Transition the current swapchain image to PresentSrcKHR, submits and presents it,
	// and advances the current frame.
	void endFrame(vk::raii::CommandBuffer& command_buffer);



private:
	void createCommandBuffers();
	void recreateSwapChain();
	void transitionToPresent(vk::raii::CommandBuffer& command_buffer);

	VeDevice& m_ve_device;
	VeWindow& m_ve_window;
	std::unique_ptr<VeSwapChain> m_ve_swap_chain;
	std::vector<vk::raii::CommandBuffer> m_command_buffers;
	std::vector<vk::raii::CommandBuffer> m_compute_command_buffers;

	uint32_t m_current_image_index;
	bool m_is_frame_started = false;
};

}

