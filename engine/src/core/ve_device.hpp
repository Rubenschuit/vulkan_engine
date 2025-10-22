/* This class is responsible for creating and managing the Vulkan device
and its associated resources, such as the command pool and queues.
It selects the appropriate physical device and creates a logical device.
It also sets up validation layers if enabled. Moreover it provides
methods for creating and managing Vulkan resources, such as buffers and images.
There are also methods for submitting single time command buffers to a queue. */
#pragma once
#include "ve_export.hpp"
#include "ve_window.hpp"
#include "ve_config.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_beta.h> // required for macOS portability subset extension
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

class VENGINE_API VeDevice {
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

	vk::raii::CommandPool& getCommandPool() { return m_command_pool; }
	vk::raii::CommandPool& getComputeCommandPool() { return m_command_pool_compute; }
	vk::raii::Device& getDevice() { return m_device; }
	vk::raii::Queue& getQueue() { return m_queue; }
	vk::raii::Queue& getComputeQueue() { return m_compute_queue; }
	vk::raii::SurfaceKHR* getSurface() { return &m_surface; }
	vk::raii::Instance& getInstance() { return m_instance; }
	vk::raii::PhysicalDevice& getPhysicalDevice() { return m_physical_device; }
	uint32_t getGraphicsQueueFamilyIndex() const { return m_queue_index; }

	SwapChainSupportDetails getSwapChainSupport() { return querySwapChainSupport(m_physical_device); }
	uint32_t findMemoryType(uint32_t type_filter, vk::MemoryPropertyFlags properties);
	vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);
	vk::Format findDepthFormat();

	void createBuffer(
		vk::DeviceSize size,
		vk::BufferUsageFlags usage,
		vk::MemoryPropertyFlags req_properties,
		vk::raii::Buffer& buffer,
		vk::raii::DeviceMemory& buffer_memory);
	void copyBuffer(vk::raii::Buffer& src_buffer, vk::raii::Buffer& dst_buffer, vk::DeviceSize size);
	void copyBufferToImage(vk::raii::Buffer& src_buffer, const vk::raii::Image& dst_image, uint32_t width, uint32_t height, uint32_t array_layers = 1);

	vk::PhysicalDeviceProperties getDeviceProperties() const { return m_physical_device.getProperties(); }

	// Single-time command buffer helpers (select queue/pool)
	std::unique_ptr<vk::raii::CommandBuffer> beginSingleTimeCommands(QueueKind kind = QueueKind::Graphics);
	void endSingleTimeCommands(vk::raii::CommandBuffer& cmd, QueueKind kind = QueueKind::Graphics);

private:
	void createInstance();
	void setupDebugMessenger();
	void createSurface();
	void pickPhysicalDevice();
	void createLogicalDevice();
	void createCommandPools();

	bool isDeviceSuitable (const vk::raii::PhysicalDevice& device);
	std::vector<const char *> getRequiredInstanceExtensions();
	uint32_t findQueueFamilies(const vk::raii::PhysicalDevice& phyisical_device);
	uint32_t findTransferQueueFamilies(const vk::raii::PhysicalDevice& phyisical_device);
	//TODO: uint32_t findComputeQueueFamilies(const vk::raii::PhysicalDevice& phyisical_device);
	SwapChainSupportDetails querySwapChainSupport(const vk::raii::PhysicalDevice& device);

	VeWindow &m_window;
	vk::raii::Instance m_instance{nullptr};
	vk::raii::Device m_device{nullptr};
	vk::raii::Context m_context;
	vk::raii::DebugUtilsMessengerEXT m_debug_messenger{nullptr};
	vk::raii::SurfaceKHR m_surface{nullptr};
	vk::raii::PhysicalDevice m_physical_device{nullptr};

	vk::raii::CommandPool m_command_pool{nullptr};
	vk::raii::CommandPool m_command_pool_transfer{nullptr}; // eTransient
	vk::raii::CommandPool m_command_pool_compute{nullptr};  // eTransient
	vk::raii::Queue m_queue{nullptr};
	vk::raii::Queue m_transfer_queue{nullptr};
	vk::raii::Queue m_compute_queue{nullptr};
	uint32_t m_queue_index = UINT32_MAX; // queue family index for graphics and present
	uint32_t m_transfer_queue_index = UINT32_MAX;
	uint32_t m_compute_queue_index = UINT32_MAX;
	//vk::raii::Queue present_queue{nullptr};

	const std::vector<const char *> m_validation_layers = ve::VALIDATION_LAYERS;
	std::vector<const char*> m_required_device_extensions = ve::REQUIRED_DEVICE_EXTENSIONS;
};
}