#include "pch.hpp"
#include "ve_device.hpp"

// std headers
#include <cstring>
#include <ranges>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <cassert>


namespace ve {

//local function
static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(
	vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
	vk::DebugUtilsMessageTypeFlagsEXT type,
	const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void*) {
	// Basic ANSI color mapping 
	const char* reset   =  "\033[0m";  // reset color
	const char* red     = "\033[31m";  // red
	const char* yellow  = "\033[33m";  // yellow
	const char* blue    = "\033[34m";  // blue
	const char* magenta = "\033[35m";  // magenta
	const char* gray    = "\033[90m";  // gray

	const char* sevColor = gray;
	const char* sevLabel = "VERBOSE";
	if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)   { sevColor = red;    sevLabel = "ERROR"; }
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning){ sevColor = yellow; sevLabel = "WARNING"; }
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)   { sevColor = blue;   sevLabel = "INFO"; }
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose){ sevColor = gray;   sevLabel = "VERBOSE"; }

	// Type coloring (optional subtle variation)
	const char* typeColor = magenta;
	std::string typeStr = to_string(type);

	std::cerr << sevColor << "[VULKAN]" << '[' << sevLabel << "] "
				<< typeColor << '[' << typeStr << "] "
				<< reset << pCallbackData->pMessage << '\n';
	return vk::False; // don't abort
}

VeDevice::VeDevice(VeWindow &window) : m_window(window) {
	createInstance();
	setupDebugMessenger();
	createSurface();
	pickPhysicalDevice();
	createLogicalDevice();
	createCommandPools();
}

VeDevice::~VeDevice() {
	// commandPool, ve_device, surface, debugMessenger and instance are RAII objects and will be cleaned up automatically
}

void VeDevice::createInstance() {
	constexpr vk::ApplicationInfo appInfo{
			.pApplicationName   = "Hello Triangle",
			.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 ),
			.pEngineName        = "No Engine",
			.engineVersion      = VK_MAKE_VERSION( 1, 0, 0 ),
			.apiVersion         = vk::ApiVersion14
	};

	// Get the required layers
	std::vector<char const*> required_layers;
	if (enable_validation_layers) {
		required_layers.assign(m_validation_layers.begin(), m_validation_layers.end());
	}

	// Check if the required layers are supported by the device
	auto layer_properties = m_context.enumerateInstanceLayerProperties();
	//any_of returns true if any element in the range satisfies the predicate
	//none_of returns true if no elements in the range satisfy the predicate
	// Here we check if any of the required layers are in none of the available layers
	// If so, we throw an error
	if (std::ranges::any_of(required_layers, [&layer_properties](auto const& required_layer) {
		return std::ranges::none_of(layer_properties,
									[required_layer](auto const& layer_property)
									{ return strcmp(layer_property.layerName, required_layer) == 0; });})
	) {
		throw std::runtime_error("One or more required validation layers are not supported!");
	}

	std::vector<vk::ExtensionProperties> available_extensions = m_context.enumerateInstanceExtensionProperties();
	VE_LOGD(available_extensions.size() << " available extensions:");
	for (const auto& extension : available_extensions) {
		VE_LOGD("\t" << extension.extensionName);
	}

	// Get the required instance extensions
	auto required_extensions = getRequiredInstanceExtensions();

	VE_LOGD(required_extensions.size() << " required extensions:");
	for (const auto& extension : required_extensions) {
		VE_LOGD("\t" << extension);
	}

	// Check if the required extensions are supported by the Vulkan implementation.
	auto extension_properties = m_context.enumerateInstanceExtensionProperties();
	for (uint32_t i = 0; i < required_extensions.size(); ++i)
	{
		// If none of the available extensions matches the required extension, throw an error
		if (std::ranges::none_of(extension_properties,
									[req_extension = required_extensions[i]](auto const& extension_property) {
								return strcmp(extension_property.extensionName, req_extension) == 0; }))
		{
			throw std::runtime_error("Required extension not supported: " + std::string(required_extensions[i]));
		}
	}

	vk::InstanceCreateInfo createInfo{
		.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
		.pApplicationInfo = &appInfo,
		.enabledLayerCount = static_cast<uint32_t>(required_layers.size()),
		.ppEnabledLayerNames = required_layers.data(),
		.enabledExtensionCount = static_cast<uint32_t>(required_extensions.size()),
		.ppEnabledExtensionNames = required_extensions.data()};

	m_instance = vk::raii::Instance(m_context, createInfo);
}

void VeDevice::setupDebugMessenger() {
	if (!enable_validation_layers)
		return;
	vk::DebugUtilsMessageSeverityFlagsEXT severity_flags(
		vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
		vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
		vk::DebugUtilsMessageSeverityFlagBitsEXT::eError );
	vk::DebugUtilsMessageTypeFlagsEXT	message_type_flags(
		vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
		vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
		vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation );
	vk::DebugUtilsMessengerCreateInfoEXT create_info{
		.messageSeverity = severity_flags,
		.messageType = message_type_flags,
		.pfnUserCallback = &debugCallback,
		.pUserData = nullptr // could be used to for example pass a pointer of the application class
	};
	m_debug_messenger = m_instance.createDebugUtilsMessengerEXT(create_info);
}

void VeDevice::createCommandPools() {
	assert(m_queue_index != UINT32_MAX && "Cannot create command pool: invalid queue index");
	vk::CommandPoolCreateInfo pool_info{
		.sType = vk::StructureType::eCommandPoolCreateInfo,
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		.queueFamilyIndex = m_queue_index
	};
	m_command_pool = vk::raii::CommandPool(m_device, pool_info);
	assert(m_transfer_queue_index != UINT32_MAX && "Cannot create command pool: invalid transfer queue index");
	vk::CommandPoolCreateInfo pool_info_transfer{
		.sType = vk::StructureType::eCommandPoolCreateInfo,
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
		.queueFamilyIndex = m_transfer_queue_index
	};
	m_command_pool_transfer = vk::raii::CommandPool(m_device, pool_info_transfer);
	assert(m_compute_queue_index != UINT32_MAX && "Cannot create command pool: invalid compute queue index");
	vk::CommandPoolCreateInfo pool_info_compute{
		.sType = vk::StructureType::eCommandPoolCreateInfo,
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
		.queueFamilyIndex = m_compute_queue_index
	};
	m_command_pool_compute = vk::raii::CommandPool(m_device, pool_info_compute);
}

void VeDevice::createSurface() {
	VkSurfaceKHR _surface; // glfw works with c api handles
	assert(m_window.getGLFWwindow() != VK_NULL_HANDLE && "GLFW window is null");
	if (glfwCreateWindowSurface(*m_instance, m_window.getGLFWwindow(), nullptr, &_surface) != VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
	}
	m_surface = vk::raii::SurfaceKHR(m_instance, _surface); // promote to RAII
}

// Checks if a physical device supports Vulkan 1.3,
// a graphics queue and the required extensions defined in ve_device.hpp
bool VeDevice::isDeviceSuitable(const vk::raii::PhysicalDevice& phyisical_device) {

	// First, the device must support at least Vulkan 1.3
	if (phyisical_device.getProperties().apiVersion < VK_API_VERSION_1_3) {
		return false;
	}

	// Second, it must have a queue family that supports graphics
	auto queue_families = phyisical_device.getQueueFamilyProperties();
	// return an iterator to the first queue family that supports graphics
	const auto qfp_iter = std::ranges::find_if(queue_families,
		[](vk::QueueFamilyProperties const& qfp) {
			return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
		});
	if (qfp_iter == queue_families.end()) {
		return false;
	}

	// Third, it must support the required device extensions
	auto physical_device_extensions = phyisical_device.enumerateDeviceExtensionProperties();
	bool found = true;
	// For every required device extension, check if it is in the list of available extensions
	for (auto const& r_extension : m_required_device_extensions) {
		auto extension_iter = std::ranges::find_if(physical_device_extensions,
			[r_extension](auto const& ext) {
				return strcmp(ext.extensionName, r_extension) == 0;
			});
		found = found && (extension_iter != physical_device_extensions.end());
	}
	if (!found) {
		return false;
	}

	// Todo centralize features to check
	// Fourth, it must support the required features
	auto features = phyisical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
											vk::PhysicalDeviceVulkan11Features,
											vk::PhysicalDeviceVulkan13Features,
											vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
											vk::PhysicalDeviceTimelineSemaphoreFeatures>();
	if (!features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy ||
		!features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering ||
		!features.get<vk::PhysicalDeviceVulkan13Features>().synchronization2 ||
		!features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState ||
		!features.get<vk::PhysicalDeviceTimelineSemaphoreFeatures>().timelineSemaphore) {
		return false;
	}

	// Finally, it must support swapchain for the given surface
	auto swap_chain_support = querySwapChainSupport(phyisical_device);
	return !swap_chain_support.formats.empty() && !swap_chain_support.presentModes.empty();
}

// TODO scoring system to select the best GPU
// Selects a physical device (GPU) that is suitable for the application's needs
// We require Vulkan 1.3, a graphics queue and the extensions defined in ve_device.hpp
void VeDevice::pickPhysicalDevice() {
	assert(*m_surface != VK_NULL_HANDLE && "Surface must be created before picking a physical device");
	auto devices = m_instance.enumeratePhysicalDevices();
	assert(devices.size() > 0 && "No GPU with Vulkan support found!");
	VE_LOGI("Found " << devices.size() << " physical device(s)");

	// Find the first suitable device
	const auto dev_iter = std::ranges::find_if(devices,
		[this](auto const& phyisical_device) {
			return isDeviceSuitable(phyisical_device);
		});
	if (dev_iter == devices.end()) {
		throw std::runtime_error("failed to find a suitable GPU!");
	}
	m_physical_device = *dev_iter;

	// print the name of the selected physical device
	VkPhysicalDeviceProperties properties = m_physical_device.getProperties();
	VE_LOGI("Using device: " << properties.deviceName);
}

void VeDevice::createLogicalDevice() {
	assert(*m_physical_device != VK_NULL_HANDLE && "Physical device must be selected before creating logical device");
	m_queue_index = findQueueFamilies(m_physical_device);
	assert(m_queue_index != UINT32_MAX && "Failed to find a valid queue family index");
	//TODO: transfer_queue_index = findTransferQueueFamilies(physical_device);
	// same for compute queue
	// For now we use the same queue for m1 machine
	m_transfer_queue_index = m_queue_index;
	m_compute_queue_index = m_queue_index;
	assert(m_transfer_queue_index != UINT32_MAX && "Failed to find a valid transfer queue family index");
	assert(m_compute_queue_index != UINT32_MAX && "Failed to find a valid compute queue family index");

	// Setup a chain of structures to enable required Vulkan features
	// Note: Slang-generated SPIR-V for VS uses DrawParameters (BaseVertex/VertexIndex),
	// so we must enable shaderDrawParameters from Vulkan 1.1 features.
	vk::StructureChain<vk::PhysicalDeviceFeatures2,
					vk::PhysicalDeviceVulkan11Features,
					vk::PhysicalDeviceVulkan13Features,
					vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
					vk::PhysicalDeviceTimelineSemaphoreFeatures> feature_chain = {
		{.features = {.samplerAnisotropy = true}},
		{.shaderDrawParameters = true},
		{.synchronization2 = true, .dynamicRendering = true},
		{.extendedDynamicState = true },
		{.timelineSemaphore = true}
	};

	assert(m_required_device_extensions.size() > 0 && "At least one device extension must be enabled");
	float queue_priority = 0.0f;
	vk::DeviceQueueCreateInfo device_queue_create_info {
		.queueFamilyIndex = m_queue_index,
		.queueCount = 1,
		.pQueuePriorities = &queue_priority
	};
	vk::DeviceQueueCreateInfo device_queue_create_info_transfer {
		.queueFamilyIndex = m_transfer_queue_index,
		.queueCount = 1,
		.pQueuePriorities = &queue_priority
	};
	vk::DeviceQueueCreateInfo device_queue_create_info_compute {
		.queueFamilyIndex = m_compute_queue_index,
		.queueCount = 1,
		.pQueuePriorities = &queue_priority
	};

	std::vector<vk::DeviceQueueCreateInfo> queue_create_infos{};
	queue_create_infos.push_back(device_queue_create_info);
	if (m_queue_index != m_transfer_queue_index)
		queue_create_infos.push_back(device_queue_create_info_transfer);
	if (m_queue_index != m_compute_queue_index)
		queue_create_infos.push_back(device_queue_create_info_compute);

	vk::DeviceCreateInfo device_create_info {
		.pNext = &feature_chain.get<vk::PhysicalDeviceFeatures2>(),
		.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
		.pQueueCreateInfos = queue_create_infos.data(),
		.enabledExtensionCount = static_cast<uint32_t>(m_required_device_extensions.size()),
		.ppEnabledExtensionNames = m_required_device_extensions.data()
	};

	m_device = vk::raii::Device(m_physical_device, device_create_info);
	m_queue = vk::raii::Queue(m_device, m_queue_index, 0);
	m_transfer_queue = vk::raii::Queue(m_device, m_transfer_queue_index, 0);
	m_compute_queue = vk::raii::Queue(m_device, m_compute_queue_index, 0);
}

// Finds a queue family that supports graphics, compute and present
// TODO: add support for separate graphics/transfer/compute/present queues and timeline semaphores
uint32_t VeDevice::findQueueFamilies(const vk::raii::PhysicalDevice& phyisical_device) {
assert(*m_surface != VK_NULL_HANDLE && "Surface must be valid when finding queue families");
	auto qf_properties = phyisical_device.getQueueFamilyProperties();
	assert(!qf_properties.empty() && "Physical device has no queue families");
	// get the first index into queueFamilyProperties which supports graphics, compute and present
	uint32_t _queue_index = UINT32_MAX;
	for (uint32_t qfp_index = 0; qfp_index < qf_properties.size(); qfp_index++) {
		if ((qf_properties[qfp_index].queueFlags & vk::QueueFlagBits::eGraphics) &&
			(qf_properties[qfp_index].queueFlags & vk::QueueFlagBits::eCompute) &&
			phyisical_device.getSurfaceSupportKHR(qfp_index, *m_surface)) {
			_queue_index = qfp_index;
			break;
		}
	}
	if (_queue_index == UINT32_MAX) {
		throw std::runtime_error("Could not find a queue for graphics and present");
	}
	return _queue_index;
}

// Not used for now as m1 machines do not have a dedicated transfer queue
uint32_t VeDevice::findTransferQueueFamilies(const vk::raii::PhysicalDevice& phyisical_device) {
	assert(*m_surface != VK_NULL_HANDLE && "Surface must be valid when finding queue families");
	auto qf_properties = phyisical_device.getQueueFamilyProperties();
	assert(!qf_properties.empty() && "Physical device has no queue families");
	// get the first index into queueFamilyProperties which transfer but NOT graphics
	uint32_t _queue_index = UINT32_MAX;
	for (uint32_t qfp_index = 0; qfp_index < qf_properties.size(); qfp_index++) {
		VE_LOGD("Queue family " << qfp_index << " supports flags: " << to_string(qf_properties[qfp_index].queueFlags));
		VE_LOGD("Queue family " << qfp_index << " has " << qf_properties[qfp_index].queueCount << " queues");
		if ((qf_properties[qfp_index].queueFlags & vk::QueueFlagBits::eTransfer) &&
			!(qf_properties[qfp_index].queueFlags & vk::QueueFlagBits::eGraphics)
			) {
			// found
			_queue_index = qfp_index;
			break;
		}
	}
	if (_queue_index == UINT32_MAX) {
		throw std::runtime_error("Could not find a queue for transfer");
	}
	return _queue_index;
}

std::vector<const char*> VeDevice::getRequiredInstanceExtensions() {
	uint32_t glfw_extensionCount = 0;
	auto glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extensionCount);

	assert(glfw_extensions != VK_NULL_HANDLE && glfw_extensionCount > 0 && "GLFW did not provide required instance extensions");

	// glfw extensions are always required
	std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_extensionCount);
	if (enable_validation_layers) {
		extensions.push_back(vk::EXTDebugUtilsExtensionName);
	}
	// add configured instance extensions
	extensions.insert(extensions.end(), ve::REQUIRED_INSTANCE_EXTENSIONS.begin(), ve::REQUIRED_INSTANCE_EXTENSIONS.end());

	return extensions;
}

SwapChainSupportDetails VeDevice::querySwapChainSupport(const vk::raii::PhysicalDevice& ve_device) {
	assert(*m_surface != VK_NULL_HANDLE && "Surface must be valid when querying swap chain support");
	SwapChainSupportDetails details;
	details.capabilities = ve_device.getSurfaceCapabilitiesKHR(*m_surface);
	details.formats = ve_device.getSurfaceFormatsKHR(*m_surface);
	details.presentModes = ve_device.getSurfacePresentModesKHR(*m_surface);
	return details;
}

// find a memory type index available that satisfies the requested properties
uint32_t VeDevice::findMemoryType(uint32_t type_filter, vk::MemoryPropertyFlags properties) {
	vk::PhysicalDeviceMemoryProperties mem_properties = m_physical_device.getMemoryProperties();
	for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
		// check if i'th bit of type_filter is set, this is equivalent to the i'th memory type being
		// suitable. We also require this memory type to support all the properties.
		if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	throw std::runtime_error("failed to find suitable memory type!");
}

// Finds a physical device supported format from a list of candidates
vk::Format VeDevice::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features) {
	assert(!candidates.empty() && "Candidates list must not be empty");
	for (vk::Format format : candidates) {
		vk::FormatProperties props = m_physical_device.getFormatProperties(format);
		if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) {
			return format;
		} else if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) {
			return format;
		}
	}
	throw std::runtime_error("failed to find supported format!");
}

vk::Format VeDevice::findDepthFormat() {
	return findSupportedFormat(
		{ vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
		vk::ImageTiling::eOptimal,
		vk::FormatFeatureFlagBits::eDepthStencilAttachment
	);
}

// sharing mode is hardcoded exclusive for now
void VeDevice::createBuffer(
		vk::DeviceSize size,
		vk::BufferUsageFlags usage,
		vk::MemoryPropertyFlags req_properties,
		vk::raii::Buffer& buffer,
		vk::raii::DeviceMemory& buffer_memory) {

	assert(size > 0 && "Buffer size must be greater than zero");
	assert(usage != static_cast<vk::BufferUsageFlags>(0) && "Buffer usage flags must not be empty");

	// Create buffer
	vk::BufferCreateInfo buffer_create_info {
		.sType = vk::StructureType::eBufferCreateInfo,
		.size = size,
		.usage = usage,
		.sharingMode = vk::SharingMode::eExclusive
	};
	buffer = vk::raii::Buffer(m_device, buffer_create_info);

	// Allocate and bind memory to buffer
	vk::MemoryRequirements mem_requirements = buffer.getMemoryRequirements();
	vk::MemoryAllocateInfo alloc_info {
		.sType = vk::StructureType::eMemoryAllocateInfo,
		.allocationSize = mem_requirements.size,
		.memoryTypeIndex = findMemoryType(mem_requirements.memoryTypeBits, req_properties)
	};
	buffer_memory = vk::raii::DeviceMemory(m_device, alloc_info);
	buffer.bindMemory(*buffer_memory, 0); // offset 0
}

void VeDevice::copyBuffer(vk::raii::Buffer& src_buffer, vk::raii::Buffer& dst_buffer, vk::DeviceSize size) {
	assert(size > 0 && "Buffer size must be greater than zero");
	assert(*src_buffer != VK_NULL_HANDLE && "Source buffer must be valid");
	assert(*dst_buffer != VK_NULL_HANDLE && "Destination buffer must be valid");
	auto cmd = beginSingleTimeCommands(QueueKind::Transfer);
	cmd->copyBuffer(*src_buffer, *dst_buffer, vk::BufferCopy{ 0, 0, size });
	endSingleTimeCommands(*cmd, QueueKind::Transfer);
}

// Assumes the image is already in eTransferDstOptimal layout
void VeDevice::copyBufferToImage(vk::raii::Buffer& src_buffer, const vk::raii::Image& dst_image, uint32_t width, uint32_t height, uint32_t array_layers) {
	assert(width > 0 && height > 0 && "Image width and height must be greater than zero");
	assert(*src_buffer != VK_NULL_HANDLE && "Source buffer must be valid");
	assert(*dst_image  != VK_NULL_HANDLE && "Destination image must be valid");
	auto cmd = beginSingleTimeCommands(QueueKind::Transfer);
	vk::BufferImageCopy copy_region{
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, array_layers },
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { width, height, 1 }
	};
	cmd->copyBufferToImage(*src_buffer, *dst_image, vk::ImageLayout::eTransferDstOptimal, copy_region);
	endSingleTimeCommands(*cmd, QueueKind::Transfer);
}

// Single-time command buffer helpers (select queue/pool)
[[nodiscard]] std::unique_ptr<vk::raii::CommandBuffer> VeDevice::beginSingleTimeCommands(QueueKind kind) {
	vk::CommandPool pool = (kind == QueueKind::Graphics) ? *m_command_pool : *m_command_pool_transfer;
	vk::CommandBufferAllocateInfo alloc_info{
		.sType = vk::StructureType::eCommandBufferAllocateInfo,
		.commandPool = pool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = 1
	};
	auto cmd = std::make_unique<vk::raii::CommandBuffer>(std::move(vk::raii::CommandBuffers(m_device, alloc_info).front()));
	cmd->begin(vk::CommandBufferBeginInfo{ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
	return cmd;
}

void VeDevice::endSingleTimeCommands(vk::raii::CommandBuffer& cmd, QueueKind kind) {
	cmd.end();
	vk::SubmitInfo submit_info{ .commandBufferCount = 1, .pCommandBuffers = &*cmd };
	if (kind == QueueKind::Graphics) {
		m_queue.submit(submit_info);
		m_queue.waitIdle();
	} else {
		m_transfer_queue.submit(submit_info);
		m_transfer_queue.waitIdle();
	}
}
}