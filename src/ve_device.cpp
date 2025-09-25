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
    // commandPool, device, surface, debugMessenger and instance are RAII objects and will be cleaned up automatically
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
    if (enableValidationLayers) {
        required_layers.assign(validationLayers.begin(), validationLayers.end());
    }

    // Check if the required layers are supported by the Vulkan implementation.
    auto layerProperties = context.enumerateInstanceLayerProperties();
    if (std::ranges::any_of(required_layers, [&layerProperties](auto const& requiredLayer) {
        return std::ranges::none_of(layerProperties,
                                   [requiredLayer](auto const& layerProperty)
                                   { return strcmp(layerProperty.layerName, requiredLayer) == 0; });})
        ) {
        throw std::runtime_error("One or more required layers are not supported!");
    }

    std::vector<vk::ExtensionProperties> availableExtensions = context.enumerateInstanceExtensionProperties();
    std::cout << availableExtensions.size() << " available extensions:" << std::endl;
    for (const auto& extension : availableExtensions) {
        std::cout << "\t" << extension.extensionName << std::endl;
    }

    // Get the required instance extensions
    auto extensions = getRequiredExtensions();

    std::cout << extensions.size() << " required extensions:" << std::endl;
    for (const auto& extension : extensions) {
        std::cout << "\t" << extension << std::endl;
    }

    // Check if the required extensions are supported by the Vulkan implementation.
    auto extensionProperties = context.enumerateInstanceExtensionProperties();
    for (uint32_t i = 0; i < extensions.size(); ++i)
    {
        if (std::ranges::none_of(extensionProperties,
                                  [extension = extensions[i]](auto const& extensionProperty)
                                  { return strcmp(extensionProperty.extensionName, extension) == 0; }))
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
    if (!enableValidationLayers) return;
    vk::DebugUtilsMessageSeverityFlagsEXT severityFlags( 
      vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | 
      vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | 
      vk::DebugUtilsMessageSeverityFlagBitsEXT::eError );
    vk::DebugUtilsMessageTypeFlagsEXT    messageTypeFlags( 
      vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | 
      vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | 
      vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation );
    vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfoEXT{
      .messageSeverity = severityFlags,
      .messageType = messageTypeFlags,
      .pfnUserCallback = &debugCallback,
      .pUserData = nullptr // could be used to for example pass a pointer of the application class
    };
    debugMessenger = instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCreateInfoEXT);
  }

  void VeDevice::createCommandPool() {
    uint32_t queue_index = findQueueFamilies();
    vk::CommandPoolCreateInfo poolInfo{
        .sType = vk::StructureType::eCommandPoolCreateInfo,
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = queue_index
    };

    commandPool = vk::raii::CommandPool(device_, poolInfo);
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
    std::cout << "Found " << devices.size() << " physical device(s)" << std::endl;
    const auto devIter = std::ranges::find_if(devices,
      [&](auto const & device) {
            auto queueFamilies = device.getQueueFamilyProperties();
            bool isSuitable = device.getProperties().apiVersion >= VK_API_VERSION_1_3;
            const auto qfpIter = std::ranges::find_if(queueFamilies,
            []( vk::QueueFamilyProperties const & qfp )
                    {
                        return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0);
                    } );
            isSuitable = isSuitable && ( qfpIter != queueFamilies.end() );
            auto extensions = device.enumerateDeviceExtensionProperties( );
            bool found = true;
            for (auto const & extension : requiredDeviceExtension) {
                auto extensionIter = std::ranges::find_if(extensions, [extension](auto const & ext) {return strcmp(ext.extensionName, extension) == 0;});
                found = found &&  extensionIter != extensions.end();
            }
            isSuitable = isSuitable && found;
            if (isSuitable) {
                physicalDevice = device;
            }
            return isSuitable;} );
    if (devIter == devices.end()) {
        throw std::runtime_error("failed to find a suitable GPU!");
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(*physicalDevice, &properties);
    std::cout << "Using device: " << properties.deviceName << std::endl;
  }

  void VeDevice::createLogicalDevice() {
       
        uint32_t queue_index = findQueueFamilies();
        
        // query for Vulkan 1.3 features
        vk::StructureChain<vk::PhysicalDeviceFeatures2, 
                           vk::PhysicalDeviceVulkan13Features, 
                           vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT> featureChain = {
            {},                               // vk::PhysicalDeviceFeatures2
            {.dynamicRendering = true, .synchronization2 = true},      // vk::PhysicalDeviceVulkan13Features
            {.extendedDynamicState = true }   // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
        };

        // create a Device
        float                     queuePriority = 0.0f;
        vk::DeviceQueueCreateInfo deviceQueueCreateInfo{ .queueFamilyIndex = queue_index, .queueCount = 1, .pQueuePriorities = &queuePriority };
        vk::DeviceCreateInfo      deviceCreateInfo{ .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                                    .queueCreateInfoCount = 1,
                                                    .pQueueCreateInfos = &deviceQueueCreateInfo,
                                                    .enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtension.size()),
                                                    .ppEnabledExtensionNames = requiredDeviceExtension.data() };

        device_ = vk::raii::Device(physicalDevice, deviceCreateInfo);
        queue = vk::raii::Queue(device_, queue_index, 0);
  }
  
  uint32_t VeDevice::findQueueFamilies() {
    auto queueFamilyProperties = physicalDevice.getQueueFamilyProperties();
    // get the first index into queueFamilyProperties which supports both graphics and present
    uint32_t queue_index = UINT32_MAX;
    for (uint32_t qfp_index = 0; qfp_index < queueFamilyProperties.size(); qfp_index++)
    {
      if ((queueFamilyProperties[qfp_index].queueFlags & vk::QueueFlagBits::eGraphics) &&
          physicalDevice.getSurfaceSupportKHR(qfp_index, *surface))
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
    if (enableValidationLayers) {
        extensions.push_back(vk::EXTDebugUtilsExtensionName );
    }

    // required for portability on macOS
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME); // required for VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME

    return extensions;
  }

  SwapChainSupportDetails VeDevice::querySwapChainSupport(vk::PhysicalDevice device) {
    SwapChainSupportDetails details;
    details.capabilities = device.getSurfaceCapabilitiesKHR(*surface);
    details.formats = device.getSurfaceFormatsKHR(*surface);
    details.presentModes = device.getSurfacePresentModesKHR(*surface);
    return details;
  }

  uint32_t VeDevice::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) {
    vk::PhysicalDeviceMemoryProperties memProperties = physicalDevice.getMemoryProperties();
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
        return i;
      }
    }
    throw std::runtime_error("failed to find suitable memory type!");
  }

  vk::Format VeDevice::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features) {
    for (vk::Format format : candidates) {
      vk::FormatProperties props = physicalDevice.getFormatProperties(format);
      if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) {
        return format;
      } else if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) {
        return format;
      }
    }
    throw std::runtime_error("failed to find supported format!");
  }

  void VeDevice::createImageWithInfo(
      const vk::ImageCreateInfo& imageInfo, 
      vk::MemoryPropertyFlags properties, 
      vk::raii::Image* image, 
      vk::raii::DeviceMemory* imageMemory) {
    
    *image = vk::raii::Image(device_, imageInfo);

    vk::MemoryRequirements memRequirements = (*image).getMemoryRequirements();

    vk::MemoryAllocateInfo allocInfo{
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties)
    };

    *imageMemory = vk::raii::DeviceMemory(device_, allocInfo);

    (*image).bindMemory(**imageMemory, 0);
  }
}