#pragma once

// Central precompiled header for the engine.
// Keep this stable to avoid frequent full rebuilds.
// Only put headers that are: (1) widely used, (2) rarely changed.

// Vulkan-Hpp configuration macros must appear before including <vulkan/...>
#ifndef VULKAN_HPP_ENABLE_RAII
#define VULKAN_HPP_ENABLE_RAII
#endif
#ifndef VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#endif

#include <vulkan/vulkan_raii.hpp>

// Standard library headers commonly used across the project
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <iostream>
#include <cassert>
#include <cstdint>
#include <limits>
#include <algorithm>

// Add more headers cautiously; each addition can increase initial build time.
