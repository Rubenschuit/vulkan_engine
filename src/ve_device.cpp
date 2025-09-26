#include "ve_device.hpp"


// std headers
#include <cstring>
#include <set>
#include <unordered_set>
#include <ranges>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <memory>


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

		// Check if the required layers are supported by the Vulkan implementation.
		auto layer_properties = context.enumerateInstanceLayerProperties();
		if (std::ranges::any_of(required_layers, [&layer_properties](auto const& required_layer) {
			return std::ranges::none_of(layer_properties,
									  [required_layer](auto const& layer_property)
									  { return strcmp(layer_property.layerName, required_layer) == 0; });})
		  ) {
			throw std::runtime_error("One or more required layers are not supported!");
		}

		std::vector<vk::ExtensionProperties> available_extensions = context.enumerateInstanceExtensionProperties();
		std::cout << available_extensions.size() << " available extensions:" << std::endl;
		for (const auto& extension : available_extensions) {
			std::cout << "\t" << extension.extensionName << std::endl;
		}

		// Get the required instance extensions
		auto extensions = getRequiredExtensions();

		std::cout << extensions.size() << " required extensions:" << std::endl;
		for (const auto& extension : extensions) {
			std::cout << "\t" << extension << std::endl;
		}

		// Check if the required extensions are supported by the Vulkan implementation.
		auto extension_properties = context.enumerateInstanceExtensionProperties();
		for (uint32_t i = 0; i < extensions.size(); ++i)
		{
			if (std::ranges::none_of(extension_properties,
								  [extension = extensions[i]](auto const& extension_property)
								  { return strcmp(extension_property.extensionName, extension) == 0; }))
			{
				throw std::runtime_error("Required extension not supported: " + std::string(extensions[i]));
			}
		}

		vk::InstanceCreateInfo createInfo{
			.flags = vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR,
			.pApplicationInfo = &appInfo,
			.enabledLayerCount = static_cast<uint32_t>(required_layers.size()),
			.ppEnabledLayerNames = required_layers.data(),
			.enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
			.ppEnabledExtensionNames = extensions.data()};

		instance = vk::raii::Instance(context, createInfo);
	}

	void VeDevice::setupDebugMessenger() {
		if (!enable_validation_layers) return;
		vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
			vk::DebugUtilsMessageSeverityFlagBitsEXT::eError );
		vk::DebugUtilsMessageTypeFlagsEXT	messageTypeFlags(
			vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
			vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
			vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation );
		vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfoEXT{
			.messageSeverity = severityFlags,
			.messageType = messageTypeFlags,
			.pfnUserCallback = &debugCallback,
			.pUserData = nullptr // could be used to for example pass a pointer of the application class
		};
		debug_messenger = instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCreateInfoEXT);
	}

	void VeDevice::createCommandPool() {
		uint32_t queue_index = findQueueFamilies();
		vk::CommandPoolCreateInfo poolInfo{
			.sType = vk::StructureType::eCommandPoolCreateInfo,
			.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
			.queueFamilyIndex = queue_index
		};

		command_pool = vk::raii::CommandPool(device, poolInfo);
	}

	void VeDevice::createSurface() {
		VkSurfaceKHR _surface; // glfw works with c api handles
		if (glfwCreateWindowSurface(*instance, window.getGLFWwindow(), nullptr, &_surface) != VK_SUCCESS) {
			throw std::runtime_error("failed to create window surface!");
		}
		surface = vk::raii::SurfaceKHR(instance, _surface); // promote to RAII
	}

	void VeDevice::pickPhysicalDevice() {
		auto devices = instance.enumeratePhysicalDevices();
		std::cout << "Found " << devices.size() << " physical ve_device(s)" << std::endl;
		const auto devIter = std::ranges::find_if(devices,
			[&](auto const & ve_device) {
				auto queueFamilies = ve_device.getQueueFamilyProperties();
				bool isSuitable = ve_device.getProperties().apiVersion >= VK_API_VERSION_1_3;
				const auto qfpIter = std::ranges::find_if(queueFamilies,
				[]( vk::QueueFamilyProperties const & qfp )
						{
							return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
						} );
				isSuitable = isSuitable && ( qfpIter != queueFamilies.end() );
				auto extensions = ve_device.enumerateDeviceExtensionProperties( );
				bool found = true;
				for (auto const & extension : requiredDeviceExtension) {
					auto extensionIter = std::ranges::find_if(extensions, [extension](auto const & ext) {return strcmp(ext.extensionName, extension) == 0;});
					found = found &&  extensionIter != extensions.end();
				}
				isSuitable = isSuitable && found;
				if (isSuitable) {
					physical_device = ve_device;
				}
				return isSuitable;} );
		if (devIter == devices.end()) {
			throw std::runtime_error("failed to find a suitable GPU!");
		}
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(*physical_device, &properties);
		std::cout << "Using ve_device: " << properties.deviceName << std::endl;
	}

	void VeDevice::createLogicalDevice() {
		uint32_t queue_index = findQueueFamilies();

		// Enable required Vulkan features
		// Note: Slang-generated SPIR-V for VS uses DrawParameters (BaseVertex/VertexIndex),
		// so we must enable shaderDrawParameters from Vulkan 1.1 features.
		vk::StructureChain<
			vk::PhysicalDeviceFeatures2,
			vk::PhysicalDeviceVulkan11Features,
			vk::PhysicalDeviceVulkan13Features,
			vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
		> featureChain = {
			{},                                 // vk::PhysicalDeviceFeatures2
			{.shaderDrawParameters = true},      // vk::PhysicalDeviceVulkan11Features
			{.dynamicRendering = true, .synchronization2 = true}, // vk::PhysicalDeviceVulkan13Features
			{.extendedDynamicState = true }      // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
		};

		// create a Device
		float				 queuePriority = 0.0f;
		vk::DeviceQueueCreateInfo deviceQueueCreateInfo{ .queueFamilyIndex = queue_index, .queueCount = 1, .pQueuePriorities = &queuePriority };
		vk::DeviceCreateInfo	  deviceCreateInfo{ .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
													 .queueCreateInfoCount = 1,
													 .pQueueCreateInfos = &deviceQueueCreateInfo,
													 .enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtension.size()),
													 .ppEnabledExtensionNames = requiredDeviceExtension.data() };

		device = vk::raii::Device(physical_device, deviceCreateInfo);
		queue = vk::raii::Queue(device, queue_index, 0);
	}

	uint32_t VeDevice::findQueueFamilies() {
		auto queueFamilyProperties = physical_device.getQueueFamilyProperties();
		// get the first index into queueFamilyProperties which supports both graphics and present
		uint32_t queue_index = UINT32_MAX;
		for (uint32_t qfp_index = 0; qfp_index < queueFamilyProperties.size(); qfp_index++)
		{
			if ((queueFamilyProperties[qfp_index].queueFlags & vk::QueueFlagBits::eGraphics) &&
				physical_device.getSurfaceSupportKHR(qfp_index, *surface))
			{
				// found a queue family that supports both graphics and present
				queue_index = qfp_index;
				break;
			}
		}
		if (queue_index == UINT32_MAX)
		{
			throw std::runtime_error("Could not find a queue for graphics and present -> terminating");
		}
		return queue_index;
	}

	std::vector<const char*> VeDevice::getRequiredExtensions() {
		uint32_t glfw_extensionCount = 0;
		auto glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extensionCount);

		// glfw extensions are always required
		std::vector extensions(glfw_extensions, glfw_extensions + glfw_extensionCount);
		if (enable_validation_layers) {
			extensions.push_back(vk::EXTDebugUtilsExtensionName );
		}

		// required for portability on macOS
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME); // required for VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME

		return extensions;
	}

	SwapChainSupportDetails VeDevice::querySwapChainSupport(vk::PhysicalDevice ve_device) {
		SwapChainSupportDetails details;
		details.capabilities = ve_device.getSurfaceCapabilitiesKHR(*surface);
		details.formats = ve_device.getSurfaceFormatsKHR(*surface);
		details.presentModes = ve_device.getSurfacePresentModesKHR(*surface);
		return details;
	}

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

	void VeDevice::createImageWithInfo(
		const vk::ImageCreateInfo& image_info,
		vk::MemoryPropertyFlags properties,
		vk::raii::Image* image,
		vk::raii::DeviceMemory* image_memory) {
		*image = vk::raii::Image(device, image_info);

		vk::MemoryRequirements memRequirements = (*image).getMemoryRequirements();

		vk::MemoryAllocateInfo allocInfo{
			.allocationSize = memRequirements.size,
			.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties)
		};

		*image_memory = vk::raii::DeviceMemory(device, allocInfo);

		(*image).bindMemory(**image_memory, 0);
	}

	// sharing mode is always exclusive
	void VeDevice::createBuffer(
			vk::DeviceSize size,
			vk::BufferUsageFlags usage,
			vk::MemoryPropertyFlags req_properties,
			vk::raii::Buffer& buffer,
			vk::raii::DeviceMemory& buffer_memory) {

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
}