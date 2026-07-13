#include "pch.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_debug_utils.hpp"
#include "utils/ve_path.hpp"

#include <atomic>

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>


namespace ve {

static std::atomic<uint32_t> s_validation_errors{0};
static std::atomic<uint32_t> s_validation_warnings{0};

uint32_t VeDevice::validationErrorCount() { return s_validation_errors.load(); }
uint32_t VeDevice::validationWarningCount() { return s_validation_warnings.load(); }

#if defined(__x86_64__) && defined(__APPLE__)
// macOS Intel: C-style types required
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT type,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void*) {
	// Basic ANSI color mapping
	const char* reset   =  "\033[0m";  // reset color
	const char* red     = "\033[31m";  // red
	const char* yellow  = "\033[33m";  // yellow
	const char* blue    = "\033[34m";  // blue
	const char* magenta = "\033[35m";  // magenta
	const char* gray    = "\033[90m";  // gray

	const char* sev_color = gray;
	const char* sev_label = "VERBOSE";
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   { sev_color = red;    sev_label = "ERROR"; ++s_validation_errors; }
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT){ sev_color = yellow; sev_label = "WARNING"; ++s_validation_warnings; }
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)   { sev_color = blue;   sev_label = "INFO"; }
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT){ sev_color = gray;   sev_label = "VERBOSE"; }

	const char* type_color = magenta;
	std::string type_str = vk::to_string(static_cast<vk::DebugUtilsMessageTypeFlagsEXT>(type));

	std::cerr << sev_color << "[VULKAN]" << '[' << sev_label << "] "
				<< type_color << '[' << type_str << "] "
				<< reset << pCallbackData->pMessage << '\n';
	return VK_FALSE; // don't abort
}
#else // want c++ style types
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

	const char* sev_color = gray;
	const char* sev_label = "VERBOSE";
	if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)   { sev_color = red;    sev_label = "ERROR"; ++s_validation_errors; }
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning){ sev_color = yellow; sev_label = "WARNING"; ++s_validation_warnings; }
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo)   { sev_color = blue;   sev_label = "INFO"; }
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose){ sev_color = gray;   sev_label = "VERBOSE"; }


	const char* type_color = magenta;
	std::string type_str = to_string(type);

	std::cerr << sev_color << "[VULKAN]" << '[' << sev_label << "] "
				<< type_color << '[' << type_str << "] "
				<< reset << pCallbackData->pMessage << '\n';
	return vk::False; // don't abort
}
#endif


VeDevice::VeDevice(VeWindow &window) : m_window(window) {
	createInstance();
	setupDebugMessenger();
	createSurface();
	pickPhysicalDevice();
	createLogicalDevice();
	createCommandPools();
	createAllocator();
	m_pipeline_cache = vk::raii::PipelineCache(m_device, vk::PipelineCacheCreateInfo{});
}

VeDevice::~VeDevice() {
	savePipelineCache();
	if (m_allocator)
		vmaDestroyAllocator(m_allocator);
	// command pool, ve_device, surface, debug messenger and instance are RAII objects and will be cleaned up automatically
}

void VeDevice::loadPipelineCache(const std::filesystem::path& file) {
	m_pipeline_cache_path = file;

	std::ifstream in(file, std::ios::binary | std::ios::ate);
	if (!in.is_open())
		return;
	auto size = static_cast<size_t>(in.tellg());
	if (size < sizeof(vk::PipelineCacheHeaderVersionOne))
		return;
	std::vector<uint8_t> data(size);
	in.seekg(0);
	in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
	if (!in.good())
		return;

	vk::PipelineCacheHeaderVersionOne header{};
	std::memcpy(&header, data.data(), sizeof(header));
	auto props = m_physical_device.getProperties();
	if (header.headerVersion != vk::PipelineCacheHeaderVersion::eOne
		|| header.vendorID != props.vendorID
		|| header.deviceID != props.deviceID
		|| std::memcmp(header.pipelineCacheUUID.data(), props.pipelineCacheUUID.data(), VK_UUID_SIZE) != 0) {
		VE_LOGI("Pipeline cache on disk does not match this device, starting empty");
		return;
	}

	try {
		vk::PipelineCacheCreateInfo create_info{
			.initialDataSize = data.size(),
			.pInitialData = data.data(),
		};
		m_pipeline_cache = vk::raii::PipelineCache(m_device, create_info);
		VE_LOGI("Pipeline cache loaded (" << data.size() << " bytes)");
	} catch (const vk::Error& e) {
		VE_LOGW("Failed to load pipeline cache, starting empty: " << e.what());
		m_pipeline_cache = vk::raii::PipelineCache(m_device, vk::PipelineCacheCreateInfo{});
	}
}

void VeDevice::savePipelineCache() noexcept {
	if (m_pipeline_cache_path.empty() || *m_pipeline_cache == vk::PipelineCache{})
		return;
	try {
		std::vector<uint8_t> data = m_pipeline_cache.getData();
		if (data.empty())
			return;
		std::error_code ec;
		std::filesystem::create_directories(m_pipeline_cache_path.parent_path(), ec);
		std::ofstream out(m_pipeline_cache_path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		if (!out.good())
			VE_LOGW("Failed to write pipeline cache to " << pathToUtf8(m_pipeline_cache_path));
	} catch (const std::exception& e) {
		VE_LOGW("Failed to save pipeline cache: " << e.what());
	}
}

void VeDevice::createInstance() {
	constexpr vk::ApplicationInfo app_info{
			.pApplicationName   = "VeApp",
			.applicationVersion = VK_MAKE_VERSION( 1, 0, 0 ),
			.pEngineName        = "VEngine",
			.engineVersion      = VK_MAKE_VERSION( 1, 0, 0 ),
			.apiVersion         = vk::ApiVersion13
	};

	// MoltenVK and async compute: by default MoltenVK exposes 4 queue families all
	// flagged Graphics+Compute+Transfer. Setting MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES=1
	// makes MoltenVK expose one family as compute-only, in theory allowing for async
	// compute. A quick test showed that setting this and enabling the async compute path
	// slightly regressed performance.
	//
	// #ifdef __APPLE__
	// 	setenv("MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES", "1", 0);
	// #endif

	// Get the required layers
	std::vector<char const*> required_layers;
	if (enable_validation_layers) {
		required_layers.assign(m_validation_layers.begin(), m_validation_layers.end());
	}

	// Error out if any required layer is missing from the available layers
	auto layer_properties = m_context.enumerateInstanceLayerProperties();
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
	const auto required_extensions = getRequiredInstanceExtensions();

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

	vk::InstanceCreateInfo create_info{
		.flags = m_has_portability_enumeration ? vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR : vk::InstanceCreateFlags{},
		.pApplicationInfo = &app_info,
		.enabledLayerCount = static_cast<uint32_t>(required_layers.size()),
		.ppEnabledLayerNames = required_layers.data(),
		.enabledExtensionCount = static_cast<uint32_t>(required_extensions.size()),
		.ppEnabledExtensionNames = required_extensions.data()};

	m_instance = vk::raii::Instance(m_context, create_info);
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
	vk::CommandPoolCreateInfo pool_info_graphics_transient{
		.sType = vk::StructureType::eCommandPoolCreateInfo,
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
		.queueFamilyIndex = m_queue_index
	};
	m_command_pool_graphics_transient = vk::raii::CommandPool(m_device, pool_info_graphics_transient);
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
	setDebugName(*this, m_command_pool, "Device Graphics Pool");
	setDebugName(*this, m_command_pool_graphics_transient, "Device Graphics Transient Pool");
	setDebugName(*this, m_command_pool_transfer, "Device Transfer Pool");
	setDebugName(*this, m_command_pool_compute, "Device Compute Pool");
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
bool VeDevice::isDeviceSuitable(const vk::raii::PhysicalDevice& physical_device) const {

	// First, the device must support at least Vulkan 1.3
	if (physical_device.getProperties().apiVersion < VK_API_VERSION_1_3) {
		return false;
	}

	// Second, it must have a queue family that supports graphics
	auto queue_families = physical_device.getQueueFamilyProperties();
	// return an iterator to the first queue family that supports graphics
	const auto qfp_iter = std::ranges::find_if(queue_families,
		[](vk::QueueFamilyProperties const& qfp) {
			return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
		});
	if (qfp_iter == queue_families.end()) {
		return false;
	}

	// Third, it must support the required device extensions
	auto physical_device_extensions = physical_device.enumerateDeviceExtensionProperties();
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

	// Fourth, it must support the required features.
	// Keep this list in sync with the feature chain enabled in createLogicalDevice().
	auto features = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
											vk::PhysicalDeviceVulkan11Features,
											vk::PhysicalDeviceVulkan12Features,
											vk::PhysicalDeviceVulkan13Features,
											vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();
	if (!features.get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy ||
		!features.get<vk::PhysicalDeviceFeatures2>().features.shaderStorageImageExtendedFormats ||
		!features.get<vk::PhysicalDeviceVulkan11Features>().multiview ||
		!features.get<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore ||
		!features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering ||
		!features.get<vk::PhysicalDeviceVulkan13Features>().synchronization2 ||
		!features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState) {
		return false;
	}

	// Finally, it must support swapchain for the given surface
	const auto swap_chain_support = querySwapChainSupport(physical_device);
	return !swap_chain_support.formats.empty() && !swap_chain_support.present_modes.empty();
}

// Selects a suitable physical device (Vulkan 1.3, required queues/extensions/features,
// swapchain support), preferring discrete over integrated
void VeDevice::pickPhysicalDevice() {
	assert(*m_surface != VK_NULL_HANDLE && "Surface must be created before picking a physical device");
	auto p_devices = m_instance.enumeratePhysicalDevices();
	assert(p_devices.size() > 0 && "No GPU with Vulkan support found!");
	VE_LOGI("Found " << p_devices.size() << " physical device(s)");

	auto type_score = [](vk::PhysicalDeviceType type) {
		switch (type) {
			case vk::PhysicalDeviceType::eDiscreteGpu:   return 2;
			case vk::PhysicalDeviceType::eIntegratedGpu: return 1;
			default:                                     return 0;
		}
	};

	int best_score = -1;
	for (auto const& physical_device : p_devices) {
		if (!isDeviceSuitable(physical_device))
			continue;
		int score = type_score(physical_device.getProperties().deviceType);
		if (score > best_score) {
			best_score = score;
			m_physical_device = physical_device;
		}
	}
	if (best_score < 0)
		throw std::runtime_error("No suitable GPU found");

	if (!hasHdrColorSpaceExtension()) {
		VE_LOGW("HDR Support: VK_EXT_swapchain_colorspace NOT found.");
	}

	// set the maximum msaa samples
	m_max_msaa_samples = queryMaxUsableSampleCount();

	// query optional feature support
	auto dev_features = m_physical_device.getFeatures();
	m_supports_bc   = dev_features.textureCompressionBC;
	m_supports_astc = dev_features.textureCompressionASTC_LDR;
	m_supports_etc2 = dev_features.textureCompressionETC2;
	// print the name of the selected physical device
	auto properties = m_physical_device.getProperties();
	VE_LOGI("Using device: " << properties.deviceName);
	VE_LOGI("Texture compression: BC=" << m_supports_bc << " ASTC=" << m_supports_astc << " ETC2=" << m_supports_etc2);
}

std::string VeDevice::getDriverName() const {
	auto chain = m_physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
	return chain.get<vk::PhysicalDeviceDriverProperties>().driverName.data();
}

void VeDevice::createLogicalDevice() {
	assert(*m_physical_device != VK_NULL_HANDLE && "Physical device must be selected before creating logical device");

	m_queue_family_indices = findAllQueueFamilies(m_physical_device);
	assert(m_queue_family_indices.isComplete() && "Failed to find all required queue families");

	// Check if the compute queue family is a dedicated async compute engine
	// (compute without graphics), meaning real parallel overlap is possible.
	auto qf_props = m_physical_device.getQueueFamilyProperties();
	auto compute_flags = qf_props[m_queue_family_indices.compute_family].queueFlags;
	m_has_dedicated_compute = (compute_flags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{}
		&& (compute_flags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{};
	VE_LOGI("Dedicated async compute: " << (m_has_dedicated_compute ? "yes" : "no")
		<< " (compute family " << m_queue_family_indices.compute_family
		<< " flags: " << vk::to_string(compute_flags) << ")");

	m_queue_index          = m_queue_family_indices.graphics_family;
	m_compute_queue_index  = m_queue_family_indices.compute_family;
	m_transfer_queue_index = m_queue_family_indices.transfer_family;

	// Query drawIndirectCount support
	{
		auto chain = m_physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
		m_supports_draw_indirect_count = chain.get<vk::PhysicalDeviceVulkan12Features>().drawIndirectCount;
	}

	// Setup a chain of structures to enable required Vulkan features
	// Note: Slang-generated SPIR-V for VS uses DrawParameters (BaseVertex/VertexIndex),
	// so we must enable shaderDrawParameters from Vulkan 1.1 features.
	vk::StructureChain<vk::PhysicalDeviceFeatures2,
					vk::PhysicalDeviceVulkan11Features,
					vk::PhysicalDeviceVulkan12Features,
					vk::PhysicalDeviceVulkan13Features,
					vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT> feature_chain = {
		{.features = {.independentBlend = true, .multiDrawIndirect = true, .drawIndirectFirstInstance = true, .depthClamp = true, .depthBiasClamp = true, .samplerAnisotropy = true, .shaderStorageImageExtendedFormats = true}},
		{.multiview = true, .shaderDrawParameters = true},
		{	// Vulkan 1.2 features (descriptor indexing + timeline semaphore)
			.drawIndirectCount = m_supports_draw_indirect_count,
			.shaderFloat16 = true,
			.descriptorIndexing = true,
			.shaderSampledImageArrayNonUniformIndexing = true,
			.descriptorBindingSampledImageUpdateAfterBind = true,
			.descriptorBindingPartiallyBound = true,
			.descriptorBindingVariableDescriptorCount = true,
			.runtimeDescriptorArray = true,
			.timelineSemaphore = true,
		},
		{.synchronization2 = true, .dynamicRendering = true},
		{.extendedDynamicState = true },
	};

	assert(m_required_device_extensions.size() > 0 && "At least one device extension must be enabled");

	// Create one DeviceQueueCreateInfo per unique queue family
	float queue_priority = 1.0f;
	auto unique_families = m_queue_family_indices.uniqueFamilies();
	std::vector<vk::DeviceQueueCreateInfo> queue_create_infos;
	for (uint32_t family : unique_families) {
		queue_create_infos.push_back({
			.queueFamilyIndex = family,
			.queueCount = 1,
			.pQueuePriorities = &queue_priority
		});
	}

	std::vector<const char*> enabled_extensions = m_required_device_extensions;

	// Enable optional extensions if supported
	auto available_extensions = m_physical_device.enumerateDeviceExtensionProperties();
	auto hasExtension = [&](const char* name) {
		return std::ranges::any_of(available_extensions, [name](const auto& ext) {
			return strcmp(ext.extensionName, name) == 0;
		});
	};
	if (hasExtension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
		enabled_extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
		m_supports_calibrated_timestamps = true;
		VE_LOGI("Enabled optional extension: " << VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
	}

	// MoltenVK exposes VK_KHR_portability_subset, KosmicKrisp does not (fully compliant)
	VkPhysicalDevicePortabilitySubsetFeaturesKHR portability_features{};
	if (hasExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
		enabled_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
		portability_features.sType = static_cast<VkStructureType>(1000163000);
		portability_features.imageViewFormatSwizzle = VK_TRUE;
		feature_chain.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().pNext = &portability_features;
		VE_LOGI("Portability: VK_KHR_portability_subset enabled (MoltenVK).");
	}

	vk::DeviceCreateInfo device_create_info {
		.pNext = &feature_chain.get<vk::PhysicalDeviceFeatures2>(),
		.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size()),
		.pQueueCreateInfos = queue_create_infos.data(),
		.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size()),
		.ppEnabledExtensionNames = enabled_extensions.data()
	};

	m_device = vk::raii::Device(m_physical_device, device_create_info);
	m_queue          = vk::raii::Queue(m_device, m_queue_index, 0);
	m_compute_queue  = vk::raii::Queue(m_device, m_compute_queue_index, 0);
	m_transfer_queue = vk::raii::Queue(m_device, m_transfer_queue_index, 0);
	setDebugName(*this, m_queue, "Graphics Queue");
	setDebugName(*this, m_compute_queue, "Compute Queue");
	setDebugName(*this, m_transfer_queue, "Transfer Queue");
}

// Finds separate queue families for graphics+present, async compute, and async transfer.
// Prefers distinct families for each role to enable parallel execution.
// Falls back to the graphics family when no separate family is available.
QueueFamilyIndices VeDevice::findAllQueueFamilies(const vk::raii::PhysicalDevice& physical_device) const {
	assert(*m_surface != VK_NULL_HANDLE && "Surface must be valid when finding queue families");
	auto qf_props = physical_device.getQueueFamilyProperties();
	assert(!qf_props.empty() && "Physical device has no queue families");

	QueueFamilyIndices indices;

	// Log all available queue families
	for (uint32_t i = 0; i < static_cast<uint32_t>(qf_props.size()); i++) {
		VE_LOGD("Queue family " << i
			<< ": flags=" << vk::to_string(qf_props[i].queueFlags)
			<< " count=" << qf_props[i].queueCount);
	}

	// Pass 1: find graphics+present family
	for (uint32_t i = 0; i < static_cast<uint32_t>(qf_props.size()); i++) {
		bool has_graphics = (qf_props[i].queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{};
		bool has_compute  = (qf_props[i].queueFlags & vk::QueueFlagBits::eCompute)  != vk::QueueFlags{};
		bool has_present  = physical_device.getSurfaceSupportKHR(i, *m_surface);

		if (has_graphics && has_compute && has_present) {
			indices.graphics_family = i;
			break;
		}
	}
	if (indices.graphics_family == UINT32_MAX) {
		throw std::runtime_error("Could not find a queue family for graphics and present");
	}

	// Pass 2: find a compute family different from graphics
	for (uint32_t i = 0; i < static_cast<uint32_t>(qf_props.size()); i++) {
		bool has_compute = (qf_props[i].queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{};
		if (has_compute && i != indices.graphics_family) {
			indices.compute_family = i;
			break;
		}
	}
	if (indices.compute_family == UINT32_MAX) {
		indices.compute_family = indices.graphics_family;
		VE_LOGW("No separate compute queue family found, sharing graphics family " << indices.graphics_family);
	}

	// Pass 3: find a transfer family different from graphics and compute
	for (uint32_t i = 0; i < static_cast<uint32_t>(qf_props.size()); i++) {
		bool has_transfer = (qf_props[i].queueFlags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{};
		if (has_transfer && i != indices.graphics_family && i != indices.compute_family) {
			indices.transfer_family = i;
			break;
		}
	}
	if (indices.transfer_family == UINT32_MAX) {
		indices.transfer_family = indices.graphics_family;
		VE_LOGW("No separate transfer queue family found, sharing graphics family " << indices.graphics_family);
	}

	// Summary
	auto unique = indices.uniqueFamilies();
	if (indices.allSameFamily()) {
		VE_LOGI("All queues on single family " << indices.graphics_family);
	} else {
		VE_LOGI("Using " << unique.size() << " separate queue families: "
			<< "Graphics=" << indices.graphics_family
			<< " Compute=" << indices.compute_family
			<< " Transfer=" << indices.transfer_family);
	}

	return indices;
}

std::vector<const char *> VeDevice::getRequiredInstanceExtensions() {
	uint32_t glfw_extension_count = 0;
	auto glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

	assert(glfw_extensions != VK_NULL_HANDLE && glfw_extension_count > 0 && "GLFW did not provide required instance extensions");

	// glfw extensions are always required
	std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_extension_count);
#ifndef NDEBUG
	extensions.push_back(vk::EXTDebugUtilsExtensionName);
#endif
	// add configured instance extensions
	extensions.insert(extensions.end(), ve::REQUIRED_INSTANCE_EXTENSIONS.begin(), ve::REQUIRED_INSTANCE_EXTENSIONS.end());

	// check for optional instance extensions
	auto available_extensions = m_context.enumerateInstanceExtensionProperties();
	for (const auto& ext : available_extensions) {
		if (strcmp(ext.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
			extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
			m_has_hdr_instance_extension = true;
			VE_LOGI("HDR Support: VK_EXT_swapchain_colorspace found at Instance level.");
		}
		if (strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
			extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			m_has_portability_enumeration = true;
			VE_LOGI("Portability: VK_KHR_portability_enumeration enabled.");
		}
	}

	return extensions;
}

// Query the swap chain support details for a given physical device
SwapChainSupportDetails VeDevice::querySwapChainSupport(const vk::raii::PhysicalDevice& ve_device) const {
	assert(*m_surface != VK_NULL_HANDLE && "Surface must be valid when querying swap chain support");
	SwapChainSupportDetails details;
	details.capabilities = ve_device.getSurfaceCapabilitiesKHR(*m_surface);
	details.formats = ve_device.getSurfaceFormatsKHR(*m_surface);
	details.present_modes = ve_device.getSurfacePresentModesKHR(*m_surface);
	return details;
}

// Query the maximum usable sample count for MSAA for m_physical_device
vk::SampleCountFlagBits VeDevice::queryMaxUsableSampleCount() const {
	if constexpr (ve::MSAA_ENABLED == false)
		return vk::SampleCountFlagBits::e1;

    vk::PhysicalDeviceProperties properties = m_physical_device.getProperties();

    vk::SampleCountFlags counts = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
    if (counts & vk::SampleCountFlagBits::e64) return vk::SampleCountFlagBits::e64;
    if (counts & vk::SampleCountFlagBits::e32) return vk::SampleCountFlagBits::e32;
    if (counts & vk::SampleCountFlagBits::e16) return vk::SampleCountFlagBits::e16;
    if (counts & vk::SampleCountFlagBits::e8) return vk::SampleCountFlagBits::e8;
    if (counts & vk::SampleCountFlagBits::e4) return vk::SampleCountFlagBits::e4;
    if (counts & vk::SampleCountFlagBits::e2) return vk::SampleCountFlagBits::e2;

    return vk::SampleCountFlagBits::e1;
}

void VeDevice::createAllocator() {
	VmaAllocatorCreateInfo alloc_info {
		.physicalDevice   = *m_physical_device,
		.device           = *m_device,
		.instance         = *m_instance,
		.vulkanApiVersion = VK_API_VERSION_1_3,
	};

	if (vmaCreateAllocator(&alloc_info, &m_allocator) != VK_SUCCESS)
		throw std::runtime_error("Failed to create VMA allocator");
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

void VeDevice::copyBuffer(vk::raii::CommandBuffer& cmd,
	vk::Buffer src_buffer, vk::Buffer dst_buffer,
	vk::DeviceSize size, vk::DeviceSize src_offset, vk::DeviceSize dst_offset) {
	assert(size > 0 && "Buffer size must be greater than zero");
	assert(src_buffer && "Source buffer must be valid");
	assert(dst_buffer && "Destination buffer must be valid");
	cmd.copyBuffer(src_buffer, dst_buffer, vk::BufferCopy{ src_offset, dst_offset, size });
}

void VeDevice::copyBufferToImage(vk::raii::CommandBuffer& cmd,
	vk::Buffer src_buffer, vk::Image dst_image,
	uint32_t width, uint32_t height, uint32_t array_layers, vk::DeviceSize src_offset) {
	assert(width > 0 && height > 0 && "Image width and height must be greater than zero");
	assert(src_buffer && "Source buffer must be valid");
	assert(dst_image && "Destination image must be valid");
	vk::BufferImageCopy copy_region{
		.bufferOffset = src_offset,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, array_layers },
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { width, height, 1 }
	};
	cmd.copyBufferToImage(src_buffer, dst_image, vk::ImageLayout::eTransferDstOptimal, copy_region);
}

void VeDevice::copyBufferToImageWithMipmaps(vk::raii::CommandBuffer& cmd,
	vk::Buffer src_buffer, vk::Image dst_image,
	uint32_t array_layers, uint32_t mip_levels,
	const std::vector<vk::DeviceSize>& buffer_offsets,
	const std::vector<vk::Extent3D>& extents) {
	assert(mip_levels > 0 && buffer_offsets.size() >= mip_levels && extents.size() >= mip_levels);
	std::vector<vk::BufferImageCopy> copy_regions;
	copy_regions.reserve(mip_levels);
	for (uint32_t level = 0; level < mip_levels; level++) {
		copy_regions.push_back({
			.bufferOffset = buffer_offsets[level],
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = { vk::ImageAspectFlagBits::eColor, level, 0, array_layers },
			.imageOffset = { 0, 0, 0 },
			.imageExtent = extents[level]
		});
	}
	cmd.copyBufferToImage(src_buffer, dst_image, vk::ImageLayout::eTransferDstOptimal, copy_regions);
}

void VeDevice::copyBuffer(vk::Buffer src_buffer, vk::Buffer dst_buffer, vk::DeviceSize size) {
	auto cmd = beginSingleTimeCommands(QueueKind::Transfer);
	copyBuffer(*cmd, src_buffer, dst_buffer, size);
	endSingleTimeCommands(*cmd, QueueKind::Transfer);
}

void VeDevice::copyBufferToImage(vk::Buffer src_buffer, vk::Image dst_image, uint32_t width, uint32_t height, uint32_t array_layers) {
	auto cmd = beginSingleTimeCommands(QueueKind::Transfer);
	copyBufferToImage(*cmd, src_buffer, dst_image, width, height, array_layers);
	endSingleTimeCommands(*cmd, QueueKind::Transfer);
}

void VeDevice::copyBufferToImageWithMipmaps(vk::Buffer src_buffer, vk::Image dst_image,
	uint32_t array_layers, uint32_t mip_levels,
	const std::vector<vk::DeviceSize>& buffer_offsets,
	const std::vector<vk::Extent3D>& extents) {
	auto cmd = beginSingleTimeCommands(QueueKind::Transfer);
	copyBufferToImageWithMipmaps(*cmd, src_buffer, dst_image, array_layers, mip_levels, buffer_offsets, extents);
	endSingleTimeCommands(*cmd, QueueKind::Transfer);
}

// Single-time command buffer helpers (select queue/pool)
[[nodiscard]] std::unique_ptr<vk::raii::CommandBuffer> VeDevice::beginSingleTimeCommands(QueueKind kind) {
	vk::CommandPool pool;
	switch (kind) {
		case QueueKind::Graphics: pool = *m_command_pool_graphics_transient; break;
		case QueueKind::Compute:  pool = *m_command_pool_compute; break;
		case QueueKind::Transfer: pool = *m_command_pool_transfer; break;
	}
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

	vk::raii::Fence fence(m_device, vk::FenceCreateInfo{});
	vk::SubmitInfo submit_info{ .commandBufferCount = 1, .pCommandBuffers = &*cmd };

	switch (kind) {
		case QueueKind::Graphics: m_queue.submit(submit_info, *fence); break;
		case QueueKind::Compute:  m_compute_queue.submit(submit_info, *fence); break;
		case QueueKind::Transfer: m_transfer_queue.submit(submit_info, *fence); break;
	}

	// Wait only for this specific submission
	while (vk::Result::eTimeout == m_device.waitForFences(*fence, vk::True, UINT64_MAX));
}
}