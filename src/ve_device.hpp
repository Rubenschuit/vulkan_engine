#pragma once

#include "ve_window.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_beta.h> // required for macOS portability subset extension
#include "ve_config.hpp"

// std lib headers
#include <string>
#include <vector>

namespace ve {

	struct SwapChainSupportDetails {
		vk::SurfaceCapabilitiesKHR capabilities;
		std::vector<vk::SurfaceFormatKHR> formats;
		std::vector<vk::PresentModeKHR> presentModes;
	};

	struct QueueFamilyIndices {
		uint32_t graphicsFamily;
		uint32_t presentFamily;
		bool graphicsFamilyHasValue = false;
		bool presentFamilyHasValue = false;
		bool isComplete() { return graphicsFamilyHasValue && presentFamilyHasValue; }
	};

	class VeDevice {
	public:

		#ifdef NDEBUG
			const bool enable_validation_layers = false;
		#else
			const bool enable_validation_layers = true;
		#endif

		VeDevice(VeWindow &window);
		~VeDevice();

		VeDevice(const VeDevice &) = delete;
		VeDevice& operator=(const VeDevice &) = delete;

		vk::raii::CommandPool& getCommandPool()  { return command_pool; }
		vk::raii::Device& getDevice() { return device; }
		vk::raii::Queue& getQueue() { return queue; }
		vk::raii::SurfaceKHR* getSurface() { return &surface; }

		SwapChainSupportDetails getSwapChainSupport() { return querySwapChainSupport(physical_device); }
		uint32_t findMemoryType(uint32_t type_filter, vk::MemoryPropertyFlags properties);
		vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);
		vk::Format findDepthFormat();
		void createImage(
			uint32_t width,
			uint32_t height,
			vk::Format format,
			vk::ImageTiling tiling,
			vk::ImageUsageFlags usage,
			vk::MemoryPropertyFlags properties,
			vk::raii::Image& image,
			vk::raii::DeviceMemory& image_memory);

		vk::raii::ImageView createImageView(vk::raii::Image& image, vk::Format format, vk::ImageAspectFlags aspect_flags);

		void createBuffer(
			vk::DeviceSize size,
			vk::BufferUsageFlags usage,
			vk::MemoryPropertyFlags req_properties,
			vk::raii::Buffer& buffer,
			vk::raii::DeviceMemory& buffer_memory);
		void copyBuffer(vk::raii::Buffer& src_buffer, vk::raii::Buffer& dst_buffer, vk::DeviceSize size);

		vk::PhysicalDeviceProperties getDeviceProperties() const { return physical_device.getProperties(); }

	private:
		void createInstance();
		void setupDebugMessenger();
		void createSurface();
		void pickPhysicalDevice();
		void createLogicalDevice();
		void createCommandPool();

		bool isDeviceSuitable (const vk::raii::PhysicalDevice& device);
		std::vector<const char *> getRequiredInstanceExtensions();
		uint32_t findQueueFamilies(const vk::raii::PhysicalDevice& p_device);
		uint32_t findTransferQueueFamilies(const vk::raii::PhysicalDevice& p_device);
		SwapChainSupportDetails querySwapChainSupport(const vk::raii::PhysicalDevice& device);

		VeWindow &window;
		vk::raii::Instance instance{nullptr};
		vk::raii::Device device{nullptr};
		vk::raii::Context context;
		vk::raii::DebugUtilsMessengerEXT debug_messenger{nullptr};
		vk::raii::SurfaceKHR surface{nullptr};
		vk::raii::PhysicalDevice physical_device{nullptr};
		vk::raii::CommandPool command_pool{nullptr};
		vk::raii::CommandPool command_pool_transfer{nullptr};
		vk::raii::Queue queue{nullptr};
		vk::raii::Queue transfer_queue{nullptr};
		uint32_t queue_index = UINT32_MAX; // queue family index for graphics and present
		uint32_t transfer_queue_index = UINT32_MAX; // queue family index for transfer
		//vk::raii::Queue present_queue{nullptr};
		const std::vector<const char *> validation_layers = VALIDATION_LAYERS;
		std::vector<const char*> required_device_extensions = REQUIRED_DEVICE_EXTENSIONS;
	};
}