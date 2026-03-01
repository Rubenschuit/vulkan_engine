/* This class is responsible for creating and managing the Vulkan device
and its associated resources, such as the command pool and queues.
It selects the appropriate physical device and creates a logical device.
It also sets up validation layers if enabled. Moreover it provides
methods for creating and managing Vulkan resources, such as buffers and images.
There are also methods for submitting single time command buffers to a queue. */
#pragma once
#include "ve_export.hpp"
#include "platform/ve_window.hpp"
#include "ve_config.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_beta.h> // required for macOS portability subset extension
#include <vector>
#include <algorithm>

namespace ve {

enum class QueueKind { Graphics, Compute, Transfer };

struct SwapChainSupportDetails {
	vk::SurfaceCapabilitiesKHR capabilities;
	std::vector<vk::SurfaceFormatKHR> formats;
	std::vector<vk::PresentModeKHR> presentModes;
};

struct QueueFamilyIndices {
	uint32_t graphicsFamily = UINT32_MAX;
	uint32_t computeFamily  = UINT32_MAX;
	uint32_t transferFamily = UINT32_MAX;

	bool isComplete() const {
		return graphicsFamily != UINT32_MAX && computeFamily != UINT32_MAX && transferFamily != UINT32_MAX;
	}
	bool allSameFamily() const {
		return graphicsFamily == computeFamily && computeFamily == transferFamily;
	}
	std::vector<uint32_t> uniqueFamilies() const {
		std::vector<uint32_t> v{graphicsFamily, computeFamily, transferFamily};
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		return v;
	}
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
	uint32_t getComputeQueueFamilyIndex() const { return m_compute_queue_index; }
	uint32_t getTransferQueueFamilyIndex() const { return m_transfer_queue_index; }
	const QueueFamilyIndices& getQueueFamilyIndices() const { return m_queue_family_indices; }

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
	// Copy buffer to image with multiple mip levels. buffer_offsets and extents must have mip_levels entries.
	void copyBufferToImageWithMipmaps(vk::raii::Buffer& src_buffer, const vk::raii::Image& dst_image,
		uint32_t array_layers, uint32_t mip_levels,
		const std::vector<vk::DeviceSize>& buffer_offsets,
		const std::vector<vk::Extent3D>& extents);

	// no-op if already idle
#ifndef NDEBUG
	void assertDeviceIdle() { m_device.waitIdle(); }
#else
	void assertDeviceIdle() {}
#endif

	const vk::PhysicalDeviceProperties getDeviceProperties() const { return m_physical_device.getProperties(); }
	vk::SampleCountFlagBits getSampleCount() const { return m_max_msaa_samples; };
	bool hasHdrColorSpaceExtension() const { return m_has_hdr_instance_extension; }
	bool supportsBC() const { return m_supports_bc; }
	bool supportsASTC() const { return m_supports_astc; }
	bool supportsDrawIndirectCount() const { return m_supports_draw_indirect_count; }
	bool supportsCalibratedTimestamps() const { return m_supports_calibrated_timestamps; }
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

	bool isDeviceSuitable (const vk::raii::PhysicalDevice& device) const;
	std::vector<const char *> getRequiredInstanceExtensions();
	QueueFamilyIndices findAllQueueFamilies(const vk::raii::PhysicalDevice& physical_device) const;
	SwapChainSupportDetails querySwapChainSupport(const vk::raii::PhysicalDevice& device) const;
	vk::SampleCountFlagBits queryMaxUsableSampleCount() const;



	VeWindow &m_window;
	vk::raii::Context m_context;
	vk::raii::Instance m_instance{nullptr};
	vk::raii::Device m_device{nullptr};
	vk::raii::DebugUtilsMessengerEXT m_debug_messenger{nullptr};
	vk::raii::SurfaceKHR m_surface{nullptr};
	vk::raii::PhysicalDevice m_physical_device{nullptr};

	vk::raii::CommandPool m_command_pool{nullptr};
	vk::raii::CommandPool m_command_pool_transfer{nullptr}; // eTransient
	vk::raii::CommandPool m_command_pool_compute{nullptr};  // eTransient
	vk::raii::Queue m_queue{nullptr};
	vk::raii::Queue m_transfer_queue{nullptr};
	vk::raii::Queue m_compute_queue{nullptr};
	QueueFamilyIndices m_queue_family_indices;
	uint32_t m_queue_index = UINT32_MAX; // queue family index for graphics and present
	uint32_t m_transfer_queue_index = UINT32_MAX;
	uint32_t m_compute_queue_index = UINT32_MAX;
	//vk::raii::Queue present_queue{nullptr};

	// MSAA samples
	vk::SampleCountFlagBits m_max_msaa_samples = vk::SampleCountFlagBits::e1; // set in pickPhysicalDevice
	
	bool m_has_hdr_instance_extension = false;
	bool m_supports_bc = false;
	bool m_supports_astc = false;
	bool m_supports_draw_indirect_count = false;
	bool m_supports_calibrated_timestamps = false;

	const std::vector<const char *> m_validation_layers = ve::VALIDATION_LAYERS;
	std::vector<const char*> m_required_device_extensions = ve::REQUIRED_DEVICE_EXTENSIONS;
};

}