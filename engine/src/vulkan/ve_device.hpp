/* Owns the Vulkan instance, physical + logical device, VMA allocator, and one
queue plus transient command pool per queue kind (graphics/present, compute,
transfer). Compute and transfer fall back to the graphics family when no
dedicated family exists; uniqueFamilies() drives buffer/image sharing mode.
Also provides single-time command submission and format/feature queries.
*/
#pragma once
#include "ve_export.hpp"
#include "platform/ve_window.hpp"
#include "ve_config.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_beta.h> // required for macOS portability subset extension
#include <vector>
#include <algorithm>
#include <filesystem>

// Forward-declare VMA types
struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

namespace ve {

enum class QueueKind { Graphics, Compute, Transfer };

struct SwapChainSupportDetails {
	vk::SurfaceCapabilitiesKHR capabilities;
	std::vector<vk::SurfaceFormatKHR> formats;
	std::vector<vk::PresentModeKHR> present_modes;
};

struct QueueFamilyIndices {
	uint32_t graphics_family = UINT32_MAX;
	uint32_t compute_family  = UINT32_MAX;
	uint32_t transfer_family = UINT32_MAX;

	bool isComplete() const {
		return graphics_family != UINT32_MAX && compute_family != UINT32_MAX && transfer_family != UINT32_MAX;
	}
	bool allSameFamily() const {
		return graphics_family == compute_family && compute_family == transfer_family;
	}
	std::vector<uint32_t> uniqueFamilies() const {
		std::vector<uint32_t> v{graphics_family, compute_family, transfer_family};
		std::sort(v.begin(), v.end());
		v.erase(std::unique(v.begin(), v.end()), v.end());
		return v;
	}
};

class VENGINE_API VeDevice {
public:

	#if defined(NDEBUG) || defined(VE_DISABLE_VK_VALIDATION)
		const bool enable_validation_layers = false;
	#else
		const bool enable_validation_layers = true;
	#endif

	// Validation messages counted process-wide by the debug callback
	static uint32_t validationErrorCount();
	static uint32_t validationWarningCount();

	VeDevice(VeWindow &window);
	~VeDevice();

	VeDevice(const VeDevice &) = delete;
	VeDevice& operator=(const VeDevice &) = delete;

	vk::raii::CommandPool& getCommandPool() { return m_command_pool; }
	vk::raii::CommandPool& getComputeCommandPool() { return m_command_pool_compute; }
	vk::raii::Device& getDevice() { return m_device; }
	vk::raii::Queue& getQueue() { return m_queue; }
	vk::raii::Queue& getComputeQueue() { return m_compute_queue; }
	vk::raii::Queue& getTransferQueue() { return m_transfer_queue; }
	vk::raii::SurfaceKHR* getSurface() { return &m_surface; }
	vk::raii::Instance& getInstance() { return m_instance; }
	vk::raii::PhysicalDevice& getPhysicalDevice() { return m_physical_device; }
	uint32_t getGraphicsQueueFamilyIndex() const { return m_queue_index; }
	uint32_t getComputeQueueFamilyIndex() const { return m_compute_queue_index; }
	uint32_t getTransferQueueFamilyIndex() const { return m_transfer_queue_index; }
	const QueueFamilyIndices& getQueueFamilyIndices() const { return m_queue_family_indices; }

	SwapChainSupportDetails getSwapChainSupport() { return querySwapChainSupport(m_physical_device); }
	VmaAllocator getAllocator() const { return m_allocator; }

	// Shared pipeline cache used by all VePipeline/VeComputePipeline creation.
	// loadPipelineCache seeds it from disk (ignored on device/UUID mismatch) and
	// remembers the path; the destructor writes the cache back automatically.
	vk::raii::PipelineCache& getPipelineCache() { return m_pipeline_cache; }
	void loadPipelineCache(const std::filesystem::path& file);
	void savePipelineCache() noexcept;

	vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);
	vk::Format findDepthFormat();

	void copyBuffer(vk::Buffer src_buffer, vk::Buffer dst_buffer, vk::DeviceSize size);
	void copyBufferToImage(vk::Buffer src_buffer, vk::Image dst_image, uint32_t width, uint32_t height, uint32_t array_layers = 1);
	void copyBufferToImageWithMipmaps(vk::Buffer src_buffer, vk::Image dst_image,
		uint32_t array_layers, uint32_t mip_levels,
		const std::vector<vk::DeviceSize>& buffer_offsets,
		const std::vector<vk::Extent3D>& extents);

	// Record-only variants
	static void copyBuffer(vk::raii::CommandBuffer& cmd,
		vk::Buffer src_buffer, vk::Buffer dst_buffer, vk::DeviceSize size,
		vk::DeviceSize src_offset = 0, vk::DeviceSize dst_offset = 0);
	static void copyBufferToImage(vk::raii::CommandBuffer& cmd,
		vk::Buffer src_buffer, vk::Image dst_image,
		uint32_t width, uint32_t height, uint32_t array_layers = 1, vk::DeviceSize src_offset = 0);
	static void copyBufferToImageWithMipmaps(vk::raii::CommandBuffer& cmd,
		vk::Buffer src_buffer, vk::Image dst_image,
		uint32_t array_layers, uint32_t mip_levels,
		const std::vector<vk::DeviceSize>& buffer_offsets,
		const std::vector<vk::Extent3D>& extents);

#ifndef NDEBUG
	void assertDeviceIdle() { m_device.waitIdle(); }
#else
	void assertDeviceIdle() {}
#endif

	const vk::PhysicalDeviceProperties getDeviceProperties() const { return m_physical_device.getProperties(); }
	bool hasDedicatedComputeQueue() const { return m_has_dedicated_compute; }
	vk::SampleCountFlagBits getSampleCount() const { return m_max_msaa_samples; };
	bool hasHdrColorSpaceExtension() const { return m_has_hdr_instance_extension; }
	bool supportsBC() const { return m_supports_bc; }
	bool supportsASTC() const { return m_supports_astc; }
	bool supportsETC2() const { return m_supports_etc2; }
	bool supportsDrawIndirectCount() const {return m_supports_draw_indirect_count; }
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
	void createAllocator();

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
	vk::raii::CommandPool m_command_pool_graphics_transient{nullptr}; // eTransient, for beginSingleTimeCommands(Graphics)
	vk::raii::CommandPool m_command_pool_transfer{nullptr}; // eTransient
	vk::raii::CommandPool m_command_pool_compute{nullptr};  // eTransient
	vk::raii::Queue m_queue{nullptr};
	vk::raii::Queue m_transfer_queue{nullptr};
	vk::raii::Queue m_compute_queue{nullptr};
	QueueFamilyIndices m_queue_family_indices;
	uint32_t m_queue_index = UINT32_MAX; // queue family index for graphics and present
	uint32_t m_transfer_queue_index = UINT32_MAX;
	uint32_t m_compute_queue_index = UINT32_MAX;

	// MSAA samples
	vk::SampleCountFlagBits m_max_msaa_samples = vk::SampleCountFlagBits::e1; // set in pickPhysicalDevice

	bool m_has_hdr_instance_extension = false;
	bool m_has_portability_enumeration = false;
	bool m_supports_bc = false;
	bool m_supports_astc = false;
	bool m_supports_etc2 = false;
	bool m_supports_draw_indirect_count = false;
	bool m_supports_calibrated_timestamps = false;
	bool m_has_dedicated_compute = false;

	VmaAllocator m_allocator = nullptr;

	vk::raii::PipelineCache m_pipeline_cache{nullptr};
	std::filesystem::path m_pipeline_cache_path;

	const std::vector<const char *> m_validation_layers = ve::VALIDATION_LAYERS;
	std::vector<const char*> m_required_device_extensions = ve::REQUIRED_DEVICE_EXTENSIONS;
};

}