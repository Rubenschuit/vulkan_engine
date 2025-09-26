#pragma once

#include "ve_window.hpp"
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_beta.h> // required for macOS portability subset extension

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
			const bool enable_validation_vayers = false;
		#else
			const bool enable_validation_layers = true;
		#endif

		VeDevice(VeWindow &window);
		~VeDevice();

		// Not copyable or movable
		VeDevice(const VeDevice &) = delete;
		void operator=(const VeDevice &) = delete;
		VeDevice(VeDevice &&) = delete;
		VeDevice &operator=(VeDevice &&) = delete;

		vk::raii::CommandPool& getCommandPool() { return command_pool; }
		vk::raii::Device& getDevice() { return device; }
		vk::raii::Queue& getQueue() { return queue; }
		vk::raii::SurfaceKHR* getSurface() { return &surface; }

		SwapChainSupportDetails getSwapChainSupport() { return querySwapChainSupport(physical_device); }
		uint32_t findMemoryType(uint32_t type_filter, vk::MemoryPropertyFlags properties);
		vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);

		void createBuffer(
			vk::DeviceSize size,
			vk::BufferUsageFlags usage,
			vk::MemoryPropertyFlags req_properties,
			vk::raii::Buffer& buffer,
			vk::raii::DeviceMemory& buffer_memory);

		vk::raii::CommandBuffer beginSingleTimeCommands();
		void endSingleTimeCommands(vk::raii::CommandBuffer* command_buffer);
		void copyBuffer(vk::Buffer src_buffer, vk::Buffer dst_buffer, vk::DeviceSize size);
		void copyBufferToImage(vk::Buffer buffer, vk::Image image, uint32_t width, uint32_t height, uint32_t layer_count);
		void createImageWithInfo(
		const vk::ImageCreateInfo& image_info,
		vk::MemoryPropertyFlags properties,
		vk::raii::Image* image,
		vk::raii::DeviceMemory* image_memory);

	private:
		void createInstance();
		void setupDebugMessenger();
		void createSurface();
		void pickPhysicalDevice();
		void createLogicalDevice();
		void createCommandPool();

		//bool isDeviceSuitable(vk::PhysicalDevice device);
		std::vector<const char *> getRequiredExtensions();
		bool checkValidationLayerSupport();
		uint32_t findQueueFamilies();
		SwapChainSupportDetails querySwapChainSupport(vk::PhysicalDevice device);
		bool checkDeviceExtensionSupport(vk::PhysicalDevice device);
		void hasGlfwRequiredInstanceExtensions();
		void populateDebugMessengerCreateInfo(vk::DebugUtilsMessengerCreateInfoEXT &create_info);

		VeWindow &window;
		vk::raii::Instance instance{nullptr};
		vk::raii::Device device{nullptr};
		vk::raii::Context context;
		vk::raii::DebugUtilsMessengerEXT debug_messenger{nullptr};
		vk::raii::SurfaceKHR surface{nullptr};
		vk::raii::PhysicalDevice physical_device{nullptr};
		vk::raii::CommandPool command_pool{nullptr};
		vk::raii::Queue queue{nullptr};
		const std::vector<const char *> validation_layers = {
			"VK_LAYER_KHRONOS_validation"
		};
		std::vector<const char*> requiredDeviceExtension = {
			vk::KHRSwapchainExtensionName,
			vk::KHRSpirv14ExtensionName,
			vk::KHRSynchronization2ExtensionName,
			vk::KHRCreateRenderpass2ExtensionName,
			VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, // required for portability on macOS
		};
	};
}