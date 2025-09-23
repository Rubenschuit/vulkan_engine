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
    std::cerr << "validation layer: type " << to_string(type) 
              << " msg: " << pCallbackData->pMessage << std::endl;
    return vk::False;
  }

  VeDevice::VeDevice(VeWindow &window) : window(window) {
    createInstance();
    setupDebugMessenger();
    pickPhysicalDevice();
    createLogicalDevice();
    //createSurface(window);
    //createCommandPool();
  }

  VeDevice::~VeDevice() {
    // Cleanup resources
    //vkDestroyCommandPool(device, commandPool, nullptr);
    //vkDestroyDevice(device, nullptr);
    //vkDestroySurfaceKHR(instance, surface, nullptr);
    //if (enableValidationLayers) {
    //  DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
    //}
    //vkDestroyInstance(instance, nullptr);

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

    // Get the required instance extensions
    auto extensions = getRequiredExtensions();

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

  void VeDevice::pickPhysicalDevice() {
    auto devices = instance.enumeratePhysicalDevices();
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
            for (auto const & extension : deviceExtensions) {
                auto extensionIter = std::ranges::find_if(extensions, [extension](auto const & ext) {return strcmp(ext.extensionName, extension) == 0;});
                found = found &&  extensionIter != extensions.end();
            }
            isSuitable = isSuitable && found;
            printf("\n");
            if (isSuitable) {
                physicalDevice = device;
            }
            return isSuitable;} );
    if (devIter == devices.end()) {
        throw std::runtime_error("failed to find a suitable GPU!");
    }
  }

  void VeDevice::createLogicalDevice() {
        // find the index of the first queue family that supports graphics
        std::vector<vk::QueueFamilyProperties> queueFamilyProperties = physicalDevice.getQueueFamilyProperties();

        // get the first index into queueFamilyProperties which supports graphics
        auto graphicsQueueFamilyProperty = std::ranges::find_if( queueFamilyProperties, []( auto const & qfp )
                        { return (qfp.queueFlags & vk::QueueFlagBits::eGraphics) != static_cast<vk::QueueFlags>(0); } );
        assert(graphicsQueueFamilyProperty != queueFamilyProperties.end() && "No graphics queue family found!");

        auto graphicsIndex = static_cast<uint32_t>( std::distance( queueFamilyProperties.begin(), graphicsQueueFamilyProperty ) );

        // query for Vulkan 1.3 features
        vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT> featureChain = {
            {},                               // vk::PhysicalDeviceFeatures2
            {.dynamicRendering = true },      // vk::PhysicalDeviceVulkan13Features
            {.extendedDynamicState = true }   // vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT
        };

        // create a Device
        float                     queuePriority = 0.0f;
        vk::DeviceQueueCreateInfo deviceQueueCreateInfo{ .queueFamilyIndex = graphicsIndex, .queueCount = 1, .pQueuePriorities = &queuePriority };
        vk::DeviceCreateInfo      deviceCreateInfo{ .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                                    .queueCreateInfoCount = 1,
                                                    .pQueueCreateInfos = &deviceQueueCreateInfo,
                                                    .enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtension.size()),
                                                    .ppEnabledExtensionNames = requiredDeviceExtension.data() };

        device_ = vk::raii::Device(physicalDevice, deviceCreateInfo);
        graphicsQueue = vk::raii::Queue(device_, graphicsIndex, 0);
  }
  
  uint32_t VeDevice::findQueueFamilies(vk::PhysicalDevice device) {
    // find the index of the first queue family that supports graphics
    std::vector<vk::QueueFamilyProperties> queueFamilyProperties = physicalDevice.getQueueFamilyProperties();

    // get the first index into queueFamilyProperties which supports graphics
    auto graphicsQueueFamilyProperty =
      std::find_if( queueFamilyProperties.begin(),
                    queueFamilyProperties.end(),
                    []( vk::QueueFamilyProperties const & qfp ) { return qfp.queueFlags & vk::QueueFlagBits::eGraphics; } );

    return static_cast<uint32_t>( std::distance( queueFamilyProperties.begin(), graphicsQueueFamilyProperty ) );
  }


  std::vector<const char*> VeDevice::getRequiredExtensions() {
    uint32_t glfw_extensionCount = 0;
    auto glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extensionCount);

    // glfw extensions are always required
    std::vector extensions(glfw_extensions, glfw_extensions + glfw_extensionCount);
    if (enableValidationLayers) {
        extensions.push_back(vk::EXTDebugUtilsExtensionName );
    }

    return extensions;
  }

}
