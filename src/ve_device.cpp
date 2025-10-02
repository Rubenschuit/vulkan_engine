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
		// Basic ANSI color mapping (can be disabled by exporting NO_COLOR or if terminal does not support it)
		const bool enableColor = (std::getenv("NO_COLOR") == nullptr);
		const char* reset   = enableColor ? "\033[0m"  : "";
		const char* red     = enableColor ? "\033[31m" : "";
		const char* yellow  = enableColor ? "\033[33m" : "";
		const char* blue    = enableColor ? "\033[34m" : "";
		const char* magenta = enableColor ? "\033[35m" : "";
		const char* gray    = enableColor ? "\033[90m" : "";

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

	VeDevice::VeDevice(VeWindow &window) : window(window) {
		createInstance();
		setupDebugMessenger();
		createSurface();
		pickPhysicalDevice();
		createLogicalDevice();
		createCommandPool();
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
			required_layers.assign(validation_layers.begin(), validation_layers.end());
		}

		// Check if the required layers are supported by the device
		auto layer_properties = context.enumerateInstanceLayerProperties();
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

		std::vector<vk::ExtensionProperties> available_extensions = context.enumerateInstanceExtensionProperties();
		std::cout << available_extensions.size() << " available extensions:" << std::endl;
		for (const auto& extension : available_extensions) {
			std::cout << "\t" << extension.extensionName << std::endl;
		}

		// Get the required instance extensions
		auto required_extensions = getRequiredInstanceExtensions();

		std::cout << required_extensions.size() << " required extensions:" << std::endl;
		for (const auto& extension : required_extensions) {
			std::cout << "\t" << extension << std::endl;
		}

		// Check if the required extensions are supported by the Vulkan implementation.
		auto extension_properties = context.enumerateInstanceExtensionProperties();
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

		instance = vk::raii::Instance(context, createInfo);
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
		debug_messenger = instance.createDebugUtilsMessengerEXT(create_info);
	}

	void VeDevice::createCommandPool() {
		assert(queue_index != UINT32_MAX && "Cannot create command pool: invalid queue index");
		vk::CommandPoolCreateInfo pool_info{
			.sType = vk::StructureType::eCommandPoolCreateInfo,
			.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			.queueFamilyIndex = queue_index
		};
		command_pool = vk::raii::CommandPool(device, pool_info);
		assert(transfer_queue_index != UINT32_MAX && "Cannot create command pool: invalid transfer queue index");
		vk::CommandPoolCreateInfo pool_info_transfer{
			.sType = vk::StructureType::eCommandPoolCreateInfo,
			.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
			.queueFamilyIndex = transfer_queue_index
		};
		command_pool_transfer = vk::raii::CommandPool(device, pool_info_transfer);
	}

	void VeDevice::createSurface() {
		VkSurfaceKHR _surface; // glfw works with c api handles
		assert(window.getGLFWwindow() != nullptr && "GLFW window is null");
		if (glfwCreateWindowSurface(*instance, window.getGLFWwindow(), nullptr, &_surface) != VK_SUCCESS) {
			throw std::runtime_error("failed to create window surface!");
		}
		surface = vk::raii::SurfaceKHR(instance, _surface); // promote to RAII
	}

	// Checks if a physical device supports Vulkan 1.3,
	// a graphics queue and the required extensions defined in ve_device.hpp
	bool VeDevice::isDeviceSuitable(const vk::raii::PhysicalDevice& p_device) {

		// First, the device must support at least Vulkan 1.3
		if (p_device.getProperties().apiVersion < VK_API_VERSION_1_3) {
			return false;
		}

		// Second, it must have a queue family that supports graphics
		auto queue_families = p_device.getQueueFamilyProperties();
		// return an iterator to the first queue family that supports graphics
		const auto qfp_iter = std::ranges::find_if(queue_families,
			[](vk::QueueFamilyProperties const& qfp) {
				return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
			});
		if (qfp_iter == queue_families.end()) {
			return false;
		}

		// Third, it must support the required device extensions
		auto d_extensions = p_device.enumerateDeviceExtensionProperties();
		bool found = true;
		// For every required device extension, check if it is in the list of available extensions
		for (auto const& r_extension : required_device_extensions) {
			auto extension_iter = std::ranges::find_if(d_extensions,
				[r_extension](auto const& ext) {
					return strcmp(ext.extensionName, r_extension) == 0;
				});
			found = found && (extension_iter != d_extensions.end());
		}
		if (!found) {
			return false;
		}

		// Todo centralize features to check
		// Fourth, it must support the required features (anisotropy, dynamic rendering, synchronization2, extended dynamic state)
		auto features = p_device.getFeatures2<vk::PhysicalDeviceFeatures2,
											  vk::PhysicalDeviceVulkan11Features,
											  vk::PhysicalDeviceVulkan13Features,
											  vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();
		if (!features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy ||
			!features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering ||
			!features.get<vk::PhysicalDeviceVulkan13Features>().synchronization2 ||
			!features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState) {
			return false;
		}

		// Finally, it must support swapchain for the given surface
		auto swap_chain_support = querySwapChainSupport(p_device);
		return !swap_chain_support.formats.empty() && !swap_chain_support.presentModes.empty();
	}

	// TODO scoring system to select the best GPU
	// Selects a physical device (GPU) that is suitable for the application's needs
	// We require Vulkan 1.3, a graphics queue and the extensions defined in ve_device.hpp
	void VeDevice::pickPhysicalDevice() {
		assert(*surface != VK_NULL_HANDLE && "Surface must be created before picking a physical device");
		auto devices = instance.enumeratePhysicalDevices();
		assert(devices.size() > 0 && "No GPU with Vulkan support found!");
		std::cout << "Found " << devices.size() << " physical device(s)" << std::endl;

		// Find the first suitable device
		const auto dev_iter = std::ranges::find_if(devices,
			[this](auto const& p_device) {
				return isDeviceSuitable(p_device);
			});
		if (dev_iter == devices.end()) {
			throw std::runtime_error("failed to find a suitable GPU!");
		}
		physical_device = *dev_iter;

		// print the name of the selected physical device
		VkPhysicalDeviceProperties properties = physical_device.getProperties();
		std::cout << "Using device: " << properties.deviceName << std::endl;
	}

	void VeDevice::createLogicalDevice() {
		assert(*physical_device != VK_NULL_HANDLE && "Physical device must be selected before creating logical device");
		queue_index = findQueueFamilies(physical_device);
		assert(queue_index != UINT32_MAX && "Failed to find a valid queue family index");
		//transfer_queue_index = findTransferQueueFamilies(physical_device);
		// For now we use the same queue for graphics and transfer as m1 machines do not have a separate transfer queue
		transfer_queue_index = queue_index;
		assert(transfer_queue_index != UINT32_MAX && "Failed to find a valid transfer queue family index");

		// Setup a chain of structures to enable required Vulkan features
		// Note: Slang-generated SPIR-V for VS uses DrawParameters (BaseVertex/VertexIndex),
		// so we must enable shaderDrawParameters from Vulkan 1.1 features.
		vk::StructureChain<vk::PhysicalDeviceFeatures2,
						   vk::PhysicalDeviceVulkan11Features,
						   vk::PhysicalDeviceVulkan13Features,
						   vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT> feature_chain = {
			{.features = {.samplerAnisotropy = true}},
			{.shaderDrawParameters = true},
			{.dynamicRendering = true, .synchronization2 = true},
			{.extendedDynamicState = true }
		};

		assert(required_device_extensions.size() > 0 && "At least one device extension must be enabled");
		float queue_priority = 0.0f;
		vk::DeviceQueueCreateInfo device_queue_create_info {
			.queueFamilyIndex = queue_index,
			.queueCount = 1,
			.pQueuePriorities = &queue_priority };
		vk::DeviceQueueCreateInfo device_queue_create_info_transfer {
			.queueFamilyIndex = transfer_queue_index,
			.queueCount = 1,
			.pQueuePriorities = &queue_priority };

		std::vector<vk::DeviceQueueCreateInfo> queue_create_infos{};
		queue_create_infos.push_back(device_queue_create_info);
		if (queue_index != transfer_queue_index)
			queue_create_infos.push_back(device_queue_create_info_transfer);

		vk::DeviceCreateInfo device_create_info {
			.pNext = &feature_chain.get<vk::PhysicalDeviceFeatures2>(),
			.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
			.pQueueCreateInfos = queue_create_infos.data(),
			.enabledExtensionCount = static_cast<uint32_t>(required_device_extensions.size()),
			.ppEnabledExtensionNames = required_device_extensions.data() };

		device = vk::raii::Device(physical_device, device_create_info);
		queue = vk::raii::Queue(device, queue_index, 0);
		transfer_queue = vk::raii::Queue(device, transfer_queue_index, 0);
	}

	// Finds a queue family that supports both graphics and present
	// Todo: add support for separate graphics and present queues and timeline semaphores?
	uint32_t VeDevice::findQueueFamilies(const vk::raii::PhysicalDevice& p_device) {
		assert(*surface != VK_NULL_HANDLE && "Surface must be valid when finding queue families");
		auto qf_properties = p_device.getQueueFamilyProperties();
		assert(!qf_properties.empty() && "Physical device has no queue families");
		// get the first index into queueFamilyProperties which supports both graphics and present
		uint32_t _queue_index = UINT32_MAX;
		for (uint32_t qfp_index = 0; qfp_index < qf_properties.size(); qfp_index++) {
			if ((qf_properties[qfp_index].queueFlags & vk::QueueFlagBits::eGraphics) &&
				p_device.getSurfaceSupportKHR(qfp_index, *surface)) {
				// found a queue family that supports both graphics and present
				_queue_index = qfp_index;
				break;
			}
		}
		if (_queue_index == UINT32_MAX) {
			throw std::runtime_error("Could not find a queue for graphics and present");
		}
		return _queue_index;
	}

	// Not used for now as m1 machines do not have a separate transfer queue
	uint32_t VeDevice::findTransferQueueFamilies(const vk::raii::PhysicalDevice& p_device) {
		assert(*surface != VK_NULL_HANDLE && "Surface must be valid when finding queue families");
		auto qf_properties = p_device.getQueueFamilyProperties();
		assert(!qf_properties.empty() && "Physical device has no queue families");
		// get the first index into queueFamilyProperties which transfer but NOT graphics
		uint32_t _queue_index = UINT32_MAX;
		for (uint32_t qfp_index = 0; qfp_index < qf_properties.size(); qfp_index++) {
			std::cout << "Queue family " << qfp_index << " supports flags: " << to_string(qf_properties[qfp_index].queueFlags) << std::endl;
			std::cout << "Queue family " << qfp_index << " has " << qf_properties[qfp_index].queueCount << " queues" << std::endl;
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

		assert(glfw_extensions != nullptr && glfw_extensionCount > 0 && "GLFW did not provide required instance extensions");

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
		assert(*surface != VK_NULL_HANDLE && "Surface must be valid when querying swap chain support");
		SwapChainSupportDetails details;
		details.capabilities = ve_device.getSurfaceCapabilitiesKHR(*surface);
		details.formats = ve_device.getSurfaceFormatsKHR(*surface);
		details.presentModes = ve_device.getSurfacePresentModesKHR(*surface);
		return details;
	}

	// find a memory type index available that satisfies   the requested properties
	uint32_t VeDevice::findMemoryType(uint32_t type_filter, vk::MemoryPropertyFlags properties) {
		vk::PhysicalDeviceMemoryProperties mem_properties = physical_device.getMemoryProperties();
		for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
			// check if i'th bit of type_filter is set, this is equivalent to the i'th memory type being
			// suitable. We also require this memory type to support all the properties.
			if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
				return i;
			}
		}
		throw std::runtime_error("failed to find suitable memory type!");
	}

	vk::Format VeDevice::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features) {
		assert(!candidates.empty() && "Candidates list must not be empty");
		for (vk::Format format : candidates) {
			vk::FormatProperties props = physical_device.getFormatProperties(format);
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
		buffer = vk::raii::Buffer(device, buffer_create_info);

		// Allocate and bind memory to buffer
		vk::MemoryRequirements mem_requirements = buffer.getMemoryRequirements();
		vk::MemoryAllocateInfo alloc_info {
			.sType = vk::StructureType::eMemoryAllocateInfo,
			.allocationSize = mem_requirements.size,
			.memoryTypeIndex = findMemoryType(mem_requirements.memoryTypeBits, req_properties)
		};
		buffer_memory = vk::raii::DeviceMemory(device, alloc_info);
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
	void VeDevice::copyBufferToImage(vk::raii::Buffer& src_buffer, vk::raii::Image& dst_image, uint32_t width, uint32_t height) {
		assert(width > 0 && height > 0 && "Image width and height must be greater than zero");
		assert(*src_buffer != VK_NULL_HANDLE && "Source buffer must be valid");
		assert(*dst_image != VK_NULL_HANDLE && "Destination image must be valid");
		assert(src_buffer.getMemoryRequirements().size < dst_image.getMemoryRequirements().size && "Source buffer size is smaller than the image size");
		auto cmd = beginSingleTimeCommands(QueueKind::Transfer);
		vk::BufferImageCopy region{
			.bufferOffset = 0,
			.bufferRowLength = 0, // tightly packed
			.bufferImageHeight = 0,
			.imageSubresource = vk::ImageSubresourceLayers{
				.aspectMask = vk::ImageAspectFlagBits::eColor,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.imageOffset = vk::Offset3D{0, 0, 0},
			.imageExtent = vk::Extent3D{width, height, 1}
		};
		cmd->copyBufferToImage(*src_buffer, *dst_image, vk::ImageLayout::eTransferDstOptimal, region);
		endSingleTimeCommands(*cmd, QueueKind::Transfer);
	}

	// Single-time command buffer helpers (select queue/pool)
	std::unique_ptr<vk::raii::CommandBuffer> VeDevice::beginSingleTimeCommands(QueueKind kind) {
		vk::CommandPool pool = (kind == QueueKind::Graphics) ? *command_pool : *command_pool_transfer;
		vk::CommandBufferAllocateInfo alloc_info{
			.sType = vk::StructureType::eCommandBufferAllocateInfo,
			.commandPool = pool,
			.level = vk::CommandBufferLevel::ePrimary,
			.commandBufferCount = 1
		};
		auto cmd = std::make_unique<vk::raii::CommandBuffer>(std::move(vk::raii::CommandBuffers(device, alloc_info).front()));
		cmd->begin(vk::CommandBufferBeginInfo{ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
		return cmd;
	}

	void VeDevice::endSingleTimeCommands(vk::raii::CommandBuffer& cmd, QueueKind kind) {
		cmd.end();
		vk::SubmitInfo submit_info{ .commandBufferCount = 1, .pCommandBuffers = &*cmd };
		if (kind == QueueKind::Graphics) {
			queue.submit(submit_info);
			queue.waitIdle();
		} else {
			transfer_queue.submit(submit_info);
			transfer_queue.waitIdle();
		}
	}
}