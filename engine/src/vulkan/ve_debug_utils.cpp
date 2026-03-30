#include "pch.hpp"
#include "vulkan/ve_debug_utils.hpp"
#include "vulkan/ve_device.hpp"

namespace ve {

#ifndef NDEBUG

void setDebugName(VeDevice& device, vk::ObjectType type, uint64_t handle, const char* name) {
	vk::DebugUtilsObjectNameInfoEXT info{
		.objectType = type,
		.objectHandle = handle,
		.pObjectName = name
	};
	device.getDevice().setDebugUtilsObjectNameEXT(info);
}

void beginDebugLabel(vk::raii::CommandBuffer& cmd, const char* name, std::array<float, 4> color) {
	vk::DebugUtilsLabelEXT label{
		.pLabelName = name,
		.color = color
	};
	cmd.beginDebugUtilsLabelEXT(label);
}

void endDebugLabel(vk::raii::CommandBuffer& cmd) {
	cmd.endDebugUtilsLabelEXT();
}

void insertDebugLabel(vk::raii::CommandBuffer& cmd, const char* name, std::array<float, 4> color) {
	vk::DebugUtilsLabelEXT label{
		.pLabelName = name,
		.color = color
	};
	cmd.insertDebugUtilsLabelEXT(label);
}

ScopedDebugLabel::ScopedDebugLabel(vk::raii::CommandBuffer& cmd, const char* name,
	std::array<float, 4> color)
	: m_cmd(cmd) {
	beginDebugLabel(cmd, name, color);
}

ScopedDebugLabel::~ScopedDebugLabel() {
	endDebugLabel(m_cmd);
}

#endif

} // namespace ve
