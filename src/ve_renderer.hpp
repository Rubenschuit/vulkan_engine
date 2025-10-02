/* VeRenderer provides methods to to render the current frame.
   It manages the swap chain and command buffers. */
#pragma once
#include <ve_device.hpp>
#include <ve_window.hpp>
#include <ve_swap_chain.hpp>
#include <memory>
#include <vector>


namespace ve {
	class VeRenderer {
	public:
		VeRenderer(VeDevice& device, VeWindow& window);
		~VeRenderer();

		//destroy copy and move constructors and assignment operators
		VeRenderer(const VeRenderer&) = delete;
		VeRenderer& operator=(const VeRenderer&) = delete;

		bool isFrameInProgress() const { return is_frame_started; }
		float getExtentAspectRatio() const { return ve_swap_chain->extentAspectRatio(); }
		vk::Format getSwapChainImageFormat() const { return ve_swap_chain->getSwapChainImageFormat(); }
		uint32_t getCurrentFrame() const {
			assert(is_frame_started && "Frame is not in progress");
			return ve_swap_chain->getCurrentFrame();
		}
		vk::raii::CommandBuffer& getCurrentCommandBuffer() {
			assert(is_frame_started && "Frame is not in progress");
			return command_buffers[ve_swap_chain->getCurrentFrame()];
		}

		// Begin a new frame. Returns true if a frame was acquired and recording can start.
		// When false is returned (e.g. swap chain out of date), no command buffer is valid for use.
		bool beginFrame();
		void endFrame(vk::raii::CommandBuffer& command_buffer);

		void beginRender(vk::raii::CommandBuffer& command_buffer);
		void endRender(vk::raii::CommandBuffer& command_buffer);

	private:
		void createCommandBuffers();
		void recreateSwapChain();

		VeDevice& ve_device;
		VeWindow& ve_window;
		std::unique_ptr<VeSwapChain> ve_swap_chain;
		std::vector<vk::raii::CommandBuffer> command_buffers;

		uint32_t current_image_index;
		bool is_frame_started = false;
	};
}

