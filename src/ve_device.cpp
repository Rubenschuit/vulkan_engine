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

  VeDevice::VeDevice(VeWindow &window) : window(window) {
    createInstance();
    //setupDebugMessenger();
    //createSurface(window);
    //pickPhysicalDevice();
    //createLogicalDevice();
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

    vk::InstanceCreateInfo createInfo{
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32_t>(required_layers.size()),
        .ppEnabledLayerNames = required_layers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data()};

    instance = vk::raii::Instance(context, createInfo);
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

  static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(
      vk::DebugUtilsMessageSeverityFlagBitsEXT severity, 
      vk::DebugUtilsMessageTypeFlagsEXT type, 
      const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData, 
      void*) {
    std::cerr << "validation layer: type " << to_string(type) 
              << " msg: " << pCallbackData->pMessage << std::endl;
    return vk::False;
  }

}

