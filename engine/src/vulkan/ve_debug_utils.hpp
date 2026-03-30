/* Vulkan debug utilities for object naming and command buffer labeling.
 * All functions compile to no-ops in release builds. */
#pragma once
#include "ve_export.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <array>

namespace ve {

class VeDevice;

#ifndef NDEBUG

VENGINE_API void setDebugName(VeDevice& device, vk::ObjectType type, uint64_t handle, const char* name);

template <typename T>
void setDebugName(VeDevice& device, const T& object, const char* name) {
	setDebugName(device, T::objectType, reinterpret_cast<uint64_t>(static_cast<typename T::CType>(*object)), name);
}

VENGINE_API void beginDebugLabel(vk::raii::CommandBuffer& cmd, const char* name,
	std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f});
VENGINE_API void endDebugLabel(vk::raii::CommandBuffer& cmd);
VENGINE_API void insertDebugLabel(vk::raii::CommandBuffer& cmd, const char* name,
	std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f});

struct VENGINE_API ScopedDebugLabel {
	ScopedDebugLabel(vk::raii::CommandBuffer& cmd, const char* name,
		std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f});
	~ScopedDebugLabel();

	ScopedDebugLabel(const ScopedDebugLabel&) = delete;
	ScopedDebugLabel& operator=(const ScopedDebugLabel&) = delete;

private:
	vk::raii::CommandBuffer& m_cmd;
};

#else

inline void setDebugName(VeDevice&, vk::ObjectType, uint64_t, const char*) {}

template <typename T>
inline void setDebugName(VeDevice&, const T&, const char*) {}

inline void beginDebugLabel(vk::raii::CommandBuffer&, const char*,
	std::array<float, 4> = {}) {}
inline void endDebugLabel(vk::raii::CommandBuffer&) {}
inline void insertDebugLabel(vk::raii::CommandBuffer&, const char*,
	std::array<float, 4> = {}) {}

struct ScopedDebugLabel {
	ScopedDebugLabel(vk::raii::CommandBuffer&, const char*,
		std::array<float, 4> = {}) {}
};

#endif

} // namespace ve
