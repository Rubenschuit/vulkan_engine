#include "pch.hpp"
#include "vulkan/ve_command_resource_manager.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_debug_utils.hpp"

namespace ve {

CommandResourceManager::CommandResourceManager(VeDevice& device) : m_device(device) {
	createPrimaryResources();
	m_main_thread_slot = registerThread();
}

CommandResourceManager::~CommandResourceManager() {}

void CommandResourceManager::createPrimaryResources() {
	auto& dev = m_device.getDevice();

	// Graphics primary pool
	vk::CommandPoolCreateInfo gfx_pool_info{
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		.queueFamilyIndex = m_device.getGraphicsQueueFamilyIndex()
	};
	m_graphics_pool = vk::raii::CommandPool(dev, gfx_pool_info);

	// Compute primary pool
	vk::CommandPoolCreateInfo comp_pool_info{
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		.queueFamilyIndex = m_device.getComputeQueueFamilyIndex()
	};
	m_compute_pool = vk::raii::CommandPool(dev, comp_pool_info);

	// UI primary pool (graphics queue family)
	vk::CommandPoolCreateInfo ui_pool_info{
		.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		.queueFamilyIndex = m_device.getGraphicsQueueFamilyIndex()
	};
	m_ui_pool = vk::raii::CommandPool(dev, ui_pool_info);

	// Allocate primary CBs (one per frame-in-flight)
	vk::CommandBufferAllocateInfo gfx_alloc{
		.commandPool = *m_graphics_pool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = MAX_FRAMES_IN_FLIGHT
	};
	m_graphics_primaries = vk::raii::CommandBuffers(dev, gfx_alloc);

	vk::CommandBufferAllocateInfo comp_alloc{
		.commandPool = *m_compute_pool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = MAX_FRAMES_IN_FLIGHT
	};
	m_compute_primaries = vk::raii::CommandBuffers(dev, comp_alloc);

	// Graphics phase 2 CBs (for split async path)
	vk::CommandBufferAllocateInfo gfx2_alloc{
		.commandPool = *m_graphics_pool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = MAX_FRAMES_IN_FLIGHT
	};
	m_graphics2_primaries = vk::raii::CommandBuffers(dev, gfx2_alloc);

	// Compute phase 2 CBs (for split async path)
	vk::CommandBufferAllocateInfo comp2_alloc{
		.commandPool = *m_compute_pool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = MAX_FRAMES_IN_FLIGHT
	};
	m_compute2_primaries = vk::raii::CommandBuffers(dev, comp2_alloc);

	vk::CommandBufferAllocateInfo ui_alloc{
		.commandPool = *m_ui_pool,
		.level = vk::CommandBufferLevel::ePrimary,
		.commandBufferCount = MAX_FRAMES_IN_FLIGHT
	};
	m_ui_primaries = vk::raii::CommandBuffers(dev, ui_alloc);

	setDebugName(m_device, m_graphics_pool, "Graphics Primary Pool");
	setDebugName(m_device, m_compute_pool, "Compute Primary Pool");
	setDebugName(m_device, m_ui_pool, "UI Pool");
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
		auto idx = std::to_string(i);
		setDebugName(m_device, m_graphics_primaries[i], ("Graphics CB [" + idx + "]").c_str());
		setDebugName(m_device, m_graphics2_primaries[i], ("Graphics2 CB [" + idx + "]").c_str());
		setDebugName(m_device, m_compute_primaries[i], ("Compute CB [" + idx + "]").c_str());
		setDebugName(m_device, m_compute2_primaries[i], ("Compute2 CB [" + idx + "]").c_str());
		setDebugName(m_device, m_ui_primaries[i], ("UI CB [" + idx + "]").c_str());
	}
}

vk::raii::CommandBuffer& CommandResourceManager::getGraphicsPrimary(uint32_t frame_index) {
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	return m_graphics_primaries[frame_index];
}

vk::raii::CommandBuffer& CommandResourceManager::getGraphics2Primary(uint32_t frame_index) {
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	return m_graphics2_primaries[frame_index];
}

vk::raii::CommandBuffer& CommandResourceManager::getComputePrimary(uint32_t frame_index) {
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	return m_compute_primaries[frame_index];
}

vk::raii::CommandBuffer& CommandResourceManager::getCompute2Primary(uint32_t frame_index) {
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	return m_compute2_primaries[frame_index];
}

vk::raii::CommandBuffer& CommandResourceManager::getUIPrimary(uint32_t frame_index) {
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	return m_ui_primaries[frame_index];
}

void CommandResourceManager::resetPrimaries(uint32_t frame_index) {
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	m_graphics_primaries[frame_index].reset();
	m_graphics2_primaries[frame_index].reset();
	m_compute_primaries[frame_index].reset();
	m_compute2_primaries[frame_index].reset();
	m_ui_primaries[frame_index].reset();
}

ThreadSlot CommandResourceManager::registerThread() {
	std::lock_guard<std::mutex> lock(m_registration_mutex);

	uint32_t id = static_cast<uint32_t>(m_slots.size());
	m_slots.emplace_back();
	auto& slot = m_slots.back();

	auto& dev = m_device.getDevice();
	for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; ++f) {
		vk::CommandPoolCreateInfo pool_info{
			.flags = vk::CommandPoolCreateFlagBits::eTransient,
			.queueFamilyIndex = m_device.getGraphicsQueueFamilyIndex()
		};
		slot.frames[f].pool = vk::raii::CommandPool(dev, pool_info);
	}

	return ThreadSlot{id};
}

uint32_t CommandResourceManager::threadSlotCount() const {
	std::lock_guard<std::mutex> lock(m_registration_mutex);
	return static_cast<uint32_t>(m_slots.size());
}

// ── Secondary CB management ────────────────────────────────────────

void CommandResourceManager::resetThreadFrame(ThreadSlot slot, uint32_t frame_index) {
	assert(slot.valid() && slot.id < m_slots.size());
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);

	// Mark for lazy reset on next acquire
	m_slots[slot.id].frames[frame_index].needs_reset = true;
}

void CommandResourceManager::ensureReset(PerFrameState& state) {
	if (!state.needs_reset)
		return;
	state.pool.reset({});
	state.active_count = 0;
	state.needs_reset = false;
}

vk::raii::CommandBuffer& CommandResourceManager::acquireSecondary(
	ThreadSlot slot, uint32_t frame_index) {

	assert(slot.valid() && slot.id < m_slots.size());
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);

	auto& state = m_slots[slot.id].frames[frame_index];
	ensureReset(state);

	// Reuse existing buffer
	if (state.active_count < static_cast<uint32_t>(state.buffers.size())) {
		auto& cb = state.buffers[state.active_count];
		vk::CommandBufferBeginInfo begin_info{
			.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
		};
		cb.begin(begin_info);
		state.active_count++;
		return cb;
	}

	// Otherwise allocate a new secondary from the pool
	vk::CommandBufferAllocateInfo alloc_info{
		.commandPool = *state.pool,
		.level = vk::CommandBufferLevel::eSecondary,
		.commandBufferCount = 1
	};
	auto new_buffers = vk::raii::CommandBuffers(m_device.getDevice(), alloc_info);
	state.buffers.push_back(std::move(new_buffers.front()));

	auto& cb = state.buffers.back();
	vk::CommandBufferBeginInfo begin_info{
		.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
	};
	cb.begin(begin_info);
	state.active_count++;
	return cb;
}

vk::raii::CommandBuffer& CommandResourceManager::acquireSecondary(
	ThreadSlot slot, uint32_t frame_index,
	const vk::CommandBufferInheritanceRenderingInfo& rendering_info) {

	assert(slot.valid() && slot.id < m_slots.size());
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);

	auto& state = m_slots[slot.id].frames[frame_index];
	ensureReset(state);

	vk::CommandBufferInheritanceInfo inheritance{
		.pNext = &rendering_info
	};
	vk::CommandBufferBeginInfo begin_info{
		.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
		       | vk::CommandBufferUsageFlagBits::eRenderPassContinue,
		.pInheritanceInfo = &inheritance
	};

	// Reuse existing buffer
	if (state.active_count < static_cast<uint32_t>(state.buffers.size())) {
		auto& cb = state.buffers[state.active_count];
		cb.begin(begin_info);
		state.active_count++;
		return cb;
	}

	// Allocate a new secondary from the pool
	vk::CommandBufferAllocateInfo alloc_info{
		.commandPool = *state.pool,
		.level = vk::CommandBufferLevel::eSecondary,
		.commandBufferCount = 1
	};
	auto new_buffers = vk::raii::CommandBuffers(m_device.getDevice(), alloc_info);
	state.buffers.push_back(std::move(new_buffers.front()));

	auto& cb = state.buffers.back();
	cb.begin(begin_info);
	state.active_count++;
	return cb;
}

uint32_t CommandResourceManager::secondaryCount(ThreadSlot slot, uint32_t frame_index) const {
	assert(slot.valid() && slot.id < m_slots.size());
	assert(frame_index < MAX_FRAMES_IN_FLIGHT);
	return m_slots[slot.id].frames[frame_index].active_count;
}

} // namespace ve