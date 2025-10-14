/* VeRenderer provides methods to to render the current frame.
   It manages the swap chain and command buffers. */
#pragma once
#include "ve_device.hpp"
#include "ve_window.hpp"
#include "ve_swap_chain.hpp"
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

		bool isFrameInProgress() const { return m_is_frame_started; }
		float getExtentAspectRatio() const { return m_ve_swap_chain->getExtentAspectRatio(); }
		vk::Format getSwapChainImageFormat() const { return m_ve_swap_chain->getSwapChainImageFormat(); }
		uint32_t getCurrentFrame() const {
			assert(m_is_frame_started && "Frame is not in progress");
			return m_ve_swap_chain->getCurrentFrame();
		}
		vk::raii::CommandBuffer& getCurrentCommandBuffer() {
			assert(m_is_frame_started && "Frame is not in progress");
			return m_command_buffers[m_ve_swap_chain->getCurrentFrame()];
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

		VeDevice& m_ve_device;
		VeWindow& m_ve_window;
		std::unique_ptr<VeSwapChain> m_ve_swap_chain;
		std::vector<vk::raii::CommandBuffer> m_command_buffers;

		uint32_t m_current_image_index;
		bool m_is_frame_started = false;
	};
}

