/*
 *  Pass-through state for batching GPU uploads during model loading.
 *
 *  UploadContext: a non-owning view referencing storage owned elsewhere 
 *
 *  SyncUploadScope: RAII wrapper for callers that want a single submit-and-wait
 *  upload via graphics queue
 */
#pragma once
#include "ve_export.hpp"
#include "resources/internal/staging_arena.hpp"
#include "vulkan/ve_device.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ve {

struct UploadContext {
	StagingArena& arena;
	vk::raii::CommandBuffer& transfer_cmd;
	vk::raii::CommandBuffer& graphics_cmd;
	size_t& bytes_in_flight;
	bool& transfer_has_work;
	bool& graphics_has_work;
};

// Default arena size for one-shot synchronous uploads
inline constexpr vk::DeviceSize SYNC_UPLOAD_ARENA_BYTES = 64ull * 1024ull * 1024ull;

class SyncUploadScope {
public:
	explicit SyncUploadScope(VeDevice& device, vk::DeviceSize arena_bytes = SYNC_UPLOAD_ARENA_BYTES)
		: m_device(device),
		  m_arena(device, arena_bytes),
		  m_cmd(device.beginSingleTimeCommands(QueueKind::Graphics)),
		  ctx{m_arena, *m_cmd, *m_cmd, m_bytes, m_transfer_has_work, m_graphics_has_work} {}

	~SyncUploadScope() {
		if (m_transfer_has_work || m_graphics_has_work)
			m_device.endSingleTimeCommands(*m_cmd, QueueKind::Graphics);
	}

	SyncUploadScope(const SyncUploadScope&) = delete;
	SyncUploadScope& operator=(const SyncUploadScope&) = delete;

private:
	VeDevice& m_device;
	StagingArena m_arena;
	std::unique_ptr<vk::raii::CommandBuffer> m_cmd;
	size_t m_bytes = 0;
	bool m_transfer_has_work = false;
	bool m_graphics_has_work = false;

public:
	UploadContext ctx;
};

}