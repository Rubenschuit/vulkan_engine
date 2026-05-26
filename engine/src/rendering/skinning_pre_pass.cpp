#include "pch.hpp"
#include "rendering/skinning_pre_pass.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_thread_pool.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "scene/ecs_event_dispatcher.hpp"
#include "resources/ve_mesh.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "utils/ve_log.hpp"

#include <cstring>
#include <algorithm>

namespace ve {

constexpr uint32_t MAX_PALETTE_MATRICES = 4096 * 2;
constexpr uint32_t WORKGROUP_SIZE = 64;

constexpr uint32_t MAX_WG_INFO_ENTRIES = (MAX_SKINNED_VERTICES_PER_FRAME / WORKGROUP_SIZE) * 2;

struct SkinWorkgroupInfo {
	uint32_t vertex_count;
	uint32_t palette_offset;
	uint32_t input_vertex_offset;
	uint32_t input_skin_offset;
	uint32_t output_vertex_offset;
	uint32_t local_vertex_base;
	uint32_t _pad0;
	uint32_t _pad1;
};
static_assert(sizeof(SkinWorkgroupInfo) == 32, "SkinWorkgroupInfo must match shader stride");

SkinningPrePass::SkinningPrePass(VeDevice& device, VeDescriptorPool& descriptor_pool,
                                 std::filesystem::path shader_path, EventBus& event_bus)
	: m_ve_device(device), m_descriptor_pool(descriptor_pool),
	  m_shader_path(std::move(shader_path)) {

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_palette_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(glm::mat4),
			MAX_PALETTE_MATRICES,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_palette_ssbos[i]->map();

		m_skinned_offset_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(uint32_t),
			MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_skinned_offset_ssbos[i]->map();
		std::memset(m_skinned_offset_ssbos[i]->getMappedMemory(), 0,
		            sizeof(uint32_t) * MAX_GPU_OBJECTS);

		m_wg_info_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(SkinWorkgroupInfo),
			MAX_WG_INFO_ENTRIES,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_wg_info_ssbos[i]->map();
	}

	createSetLayout();
	createPipelineLayout();
	createPipeline();
	resetAllocator();

	event_bus.subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
		subscribeToRegistry(*e.registry);
	});
	event_bus.subscribe<SceneUnloadedEvent>([this](const SceneUnloadedEvent&) {
		m_registry = nullptr;
		resetAllocator();
	});
	event_bus.subscribe<AssetLoadCompleteEvent>([this](const AssetLoadCompleteEvent&) {
		if (!m_registry)
			return;
		for (auto [entity, mc, sc] : m_registry->view<MeshComponent, SkinComponent>()) {
			if (m_slots.count(entity.index()))
				continue;
			allocateSlotForEntity(entity, mc.getMesh());
		}
	});
}

SkinningPrePass::~SkinningPrePass() = default;

void SkinningPrePass::createSetLayout() {
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // mega VBO (read bind-pose + write skinned output)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // mega skin VBO (read joints + weights)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // palette
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // mega shadow VBO (write skinned position-only)
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // per-workgroup info table
		.build();
}

void SkinningPrePass::createPipelineLayout() {
	std::array<vk::DescriptorSetLayout, 1> set_layouts{*m_set_layout->getDescriptorSetLayout()};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void SkinningPrePass::createPipeline() {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path, m_pipeline_layout);
}

void SkinningPrePass::subscribeToRegistry(Registry& registry) {
	m_registry = &registry;
	resetAllocator();

	// Seed slots for entities that already have Mesh+Skin at scene-load time
	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>())
		allocateSlotForEntity(entity, mc.getMesh());

	registry.events().subscribe<EntityDestroyedEvent>(
		[this](const EntityDestroyedEvent& e) { freeSlotForEntity(e.entity); });

	registry.events().subscribe<ComponentRemovedEvent<SkinComponent>>(
		[this](const ComponentRemovedEvent<SkinComponent>& e) { freeSlotForEntity(e.entity); });
	registry.events().subscribe<ComponentRemovedEvent<MeshComponent>>(
		[this](const ComponentRemovedEvent<MeshComponent>& e) { freeSlotForEntity(e.entity); });

	registry.events().subscribe<ComponentAddedEvent<SkinComponent>>(
		[this](const ComponentAddedEvent<SkinComponent>& e) {
			if (!m_registry)
				return;
			auto* mc = m_registry->getComponent<MeshComponent>(e.entity);
			if (mc && mc->hasMesh())
				allocateSlotForEntity(e.entity, mc->getMesh());
		});
	registry.events().subscribe<ComponentAddedEvent<MeshComponent>>(
		[this](const ComponentAddedEvent<MeshComponent>& e) {
			if (!m_registry || !e.component.hasMesh())
				return;
			if (m_registry->hasComponent<SkinComponent>(e.entity))
				allocateSlotForEntity(e.entity, e.component.getMesh());
		});

	registry.events().subscribe<MeshDataChangedEvent>(
		[this](const MeshDataChangedEvent& e) {
			if (!m_registry)
				return;
			if (!m_registry->hasComponent<SkinComponent>(e.entity))
				return;
			auto* mc = m_registry->getComponent<MeshComponent>(e.entity);
			if (!mc || !mc->hasMesh())
				return;
			freeSlotForEntity(e.entity);
			allocateSlotForEntity(e.entity, mc->getMesh());
		});
}

void SkinningPrePass::resetAllocator() {
	m_slots.clear();
	m_free_ranges.clear();
	m_free_ranges.push_back({0, MAX_SKINNED_VERTICES_PER_FRAME});
}

bool SkinningPrePass::allocateSlotForEntity(Entity entity, VeMesh* mesh) {
	if (!mesh || !mesh->hasSkinning())
		return false;
	const uint32_t key = entity.index();
	if (m_slots.count(key))
		return true;

	const uint32_t need = mesh->getVertexCount();
	if (need == 0)
		return false;

	for (auto it = m_free_ranges.begin(); it != m_free_ranges.end(); ++it) {
		if (it->vertex_count < need)
			continue;
		SubspaceSlot s{it->offset, need};
		m_slots[key] = s;
		if (it->vertex_count == need) {
			m_free_ranges.erase(it);
		} else {
			it->offset += need;
			it->vertex_count -= need;
		}
		return true;
	}

	VE_LOGW("SkinningPrePass: dynamic region overflow allocating " << need
		<< " vertices for entity " << entity.id()
		<< " (cap=" << MAX_SKINNED_VERTICES_PER_FRAME << "); entity will not render");
	return false;
}

void SkinningPrePass::freeSlotForEntity(Entity entity) {
	const uint32_t key = entity.index();
	auto it = m_slots.find(key);
	if (it == m_slots.end())
		return;
	SubspaceSlot s = it->second;
	m_slots.erase(it);

	auto pos = std::upper_bound(m_free_ranges.begin(), m_free_ranges.end(), s.offset,
		[](uint32_t off, const SubspaceSlot& r) { return off < r.offset; });
	pos = m_free_ranges.insert(pos, s);

	auto next = std::next(pos);
	if (next != m_free_ranges.end() && pos->offset + pos->vertex_count == next->offset) {
		pos->vertex_count += next->vertex_count;
		m_free_ranges.erase(next);
	}
	if (pos != m_free_ranges.begin()) {
		auto prev = std::prev(pos);
		if (prev->offset + prev->vertex_count == pos->offset) {
			prev->vertex_count += pos->vertex_count;
			m_free_ranges.erase(pos);
		}
	}
}

void SkinningPrePass::updatePalette(Registry& registry, uint32_t frame_index, VeThreadPool* thread_pool) {
	auto& dispatches = m_pending_dispatches[frame_index];
	dispatches.clear();

	auto* palette_ptr = static_cast<glm::mat4*>(m_palette_ssbos[frame_index]->getMappedMemory());
	uint32_t palette_cursor = 0;

	// Serial pass over all skinned meshes
	bool overflow_warned_this_frame = false;
	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>()) {
		VeMesh* mesh = mc.getMesh();
		if (!mesh || !mesh->hasSkinning())
			continue;
		const auto& joints = sc.getJointEntities();
		const auto& ibms = sc.getInverseBindMatrices();
		if (joints.empty() || joints.size() != ibms.size())
			continue;
		if (!m_slots.count(entity.index()))
			continue;

		uint32_t joint_count = static_cast<uint32_t>(joints.size());
		if (palette_cursor + joint_count > MAX_PALETTE_MATRICES) {
			if (!overflow_warned_this_frame) {
				VE_LOGW("SkinningPrePass: palette capacity exceeded (cap="
				        << MAX_PALETTE_MATRICES << "); some skinned meshes will be skipped this frame");
				overflow_warned_this_frame = true;
			}
			continue;
		}

		// Mark the mesh's world AABB stale once per frame so downstream getWorldAABB() calls
		// recompute exactly once
		mc.invalidateWorldAABB();

		registry.primeWorldTransform(entity);
		for (Entity je : joints) {
			if (!je.isNull() && registry.isAlive(je))
				registry.primeWorldTransform(je);
		}

		dispatches.push_back({
			.entity = entity,
			.vertex_count = mesh->getVertexCount(),
			.palette_offset = palette_cursor,
		});
		palette_cursor += joint_count;
	}

	m_palette_count[frame_index] = palette_cursor;

	if (dispatches.empty())
		return;

	// Compute palette matrices. Workers write disjoint slices of palette_ptr
	auto compute_one = [&](const SkinDispatch& d) {
		const auto* sc = registry.getComponent<SkinComponent>(d.entity);
		if (!sc)
			return;
		const auto& joints = sc->getJointEntities();
		const auto& ibms = sc->getInverseBindMatrices();
		const glm::mat4 inv_mesh_world = glm::inverse(registry.getWorldTransform(d.entity));
		uint32_t base = d.palette_offset;
		for (uint32_t j = 0; j < joints.size(); j++) {
			Entity je = joints[j];
			glm::mat4 joint_world = (!je.isNull() && registry.isAlive(je))
				? registry.getWorldTransform(je) : glm::mat4(1.0f);
			palette_ptr[base + j] = inv_mesh_world * joint_world * ibms[j];
		}
	};

	constexpr size_t MIN_PARALLEL_DISPATCHES = 8;
	uint32_t N = thread_pool ? thread_pool->workerCount() : 0;
	if (N > 0 && dispatches.size() >= MIN_PARALLEL_DISPATCHES) {
		thread_pool->dispatch([&, total = static_cast<uint32_t>(dispatches.size())](uint32_t wi, ThreadSlot) {
			uint32_t chunk = (total + N - 1) / N;
			uint32_t begin = wi * chunk;
			uint32_t end = std::min(begin + chunk, total);
			for (uint32_t i = begin; i < end; i++)
				compute_one(dispatches[i]);
		});
	} else {
		for (const auto& d : dispatches)
			compute_one(d);
	}
}

void SkinningPrePass::updateSkinnedOffsets(const GpuSceneManager& gpu_scene,
                                            const PbrMegaBuffer& mega_buffer,
                                            uint32_t frame_index) {
	if (!mega_buffer.isValid())
		return;

	auto* table = static_cast<uint32_t*>(m_skinned_offset_ssbos[frame_index]->getMappedMemory());
	auto& last_written = m_last_written_gpu_ids[frame_index];

	for (uint32_t prev_id : last_written)
		table[prev_id] = 0;
	last_written.clear();

	const uint32_t dynamic_base = mega_buffer.getDynamicRegionBase();
	const uint32_t dynamic_capacity = mega_buffer.getDynamicRegionCapacity();
	const uint32_t frame_subspace_offset = dynamic_base + frame_index * dynamic_capacity;

	for (const SkinDispatch& d : m_pending_dispatches[frame_index]) {
		if (!gpu_scene.hasGpuId(d.entity))
			continue;
		uint32_t gpu_id = gpu_scene.getGpuId(d.entity);
		if (gpu_id >= MAX_GPU_OBJECTS)
			continue;
		auto slot_it = m_slots.find(d.entity.index());
		if (slot_it == m_slots.end())
			continue;
		table[gpu_id] = frame_subspace_offset + slot_it->second.offset;
		last_written.push_back(gpu_id);
	}
}

void SkinningPrePass::refreshDescriptors(uint32_t frame_index, PbrMegaBuffer& mega_buffer) {
	VkBuffer vbo = mega_buffer.getMegaVbo()->getBuffer();
	VkBuffer skin = mega_buffer.getMegaSkinVbo()->getBuffer();
	VkBuffer shadow = mega_buffer.getMegaShadowVbo()->getBuffer();
	if (m_cached_mega_vbo[frame_index] == vbo
		&& m_cached_mega_skin_vbo[frame_index] == skin
		&& m_cached_mega_shadow_vbo[frame_index] == shadow)
		return;

	auto vbo_info = mega_buffer.getMegaVbo()->getDescriptorInfo();
	auto skin_info = mega_buffer.getMegaSkinVbo()->getDescriptorInfo();
	auto palette_info = m_palette_ssbos[frame_index]->getDescriptorInfo();
	auto shadow_info = mega_buffer.getMegaShadowVbo()->getDescriptorInfo();
	auto wg_info = m_wg_info_ssbos[frame_index]->getDescriptorInfo();

	VeDescriptorWriter(*m_set_layout, m_descriptor_pool)
		.writeBuffer(0, &vbo_info)
		.writeBuffer(1, &skin_info)
		.writeBuffer(2, &palette_info)
		.writeBuffer(3, &shadow_info)
		.writeBuffer(4, &wg_info)
		.build(m_descriptor_sets[frame_index]);

	m_cached_mega_vbo[frame_index] = vbo;
	m_cached_mega_skin_vbo[frame_index] = skin;
	m_cached_mega_shadow_vbo[frame_index] = shadow;
}

void SkinningPrePass::dispatch(VeFrameInfo& fi, PbrMegaBuffer& mega_buffer) {
	uint32_t frame = fi.current_frame;
	const auto& dispatches = m_pending_dispatches[frame];
	if (dispatches.empty() || !mega_buffer.isValid() || !mega_buffer.hasSkinData())
		return;

	refreshDescriptors(frame, mega_buffer);

	auto& cmd = fi.compute_command_buffer;
	auto& registry = *fi.registry;

	const uint32_t dynamic_base = mega_buffer.getDynamicRegionBase();
	const uint32_t dynamic_capacity = mega_buffer.getDynamicRegionCapacity();
	const uint32_t frame_subspace_offset = dynamic_base + frame * dynamic_capacity;

	auto* wg_info_ptr = static_cast<SkinWorkgroupInfo*>(m_wg_info_ssbos[frame]->getMappedMemory());
	uint32_t total_workgroups = 0;
	bool overflow_warned = false;

	for (const SkinDispatch& d : dispatches) {
		Entity entity = d.entity;
		if (!registry.isAlive(entity))
			continue;
		auto* mc = registry.getComponent<MeshComponent>(entity);
		if (!mc)
			continue;
		VeMesh* mesh = mc->getMesh();
		if (!mesh || !mesh->hasSkinning())
			continue;

		const auto* entry = mega_buffer.getEntry(mesh);
		if (!entry || entry->skin_vertex_offset == PbrMegaBuffer::NO_SKIN_OFFSET)
			continue;

		auto slot_it = m_slots.find(entity.index());
		if (slot_it == m_slots.end())
			continue;

		const uint32_t groups = (d.vertex_count + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE;
		if (total_workgroups + groups > MAX_WG_INFO_ENTRIES) {
			if (!overflow_warned) {
				VE_LOGW("SkinningPrePass: workgroup table overflow (cap="
				        << MAX_WG_INFO_ENTRIES << "); skinning truncated this frame");
				overflow_warned = true;
			}
			break;
		}

		const uint32_t out_offset = frame_subspace_offset + slot_it->second.offset;
		for (uint32_t g = 0; g < groups; g++) {
			wg_info_ptr[total_workgroups + g] = SkinWorkgroupInfo{
				.vertex_count        = d.vertex_count,
				.palette_offset      = d.palette_offset,
				.input_vertex_offset = entry->vertex_offset,
				.input_skin_offset   = entry->skin_vertex_offset,
				.output_vertex_offset = out_offset,
				.local_vertex_base   = g * WORKGROUP_SIZE,
			};
		}
		total_workgroups += groups;
	}

	if (total_workgroups == 0)
		return;

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_compute_pipeline->getPipeline());
	std::array<vk::DescriptorSet, 1> sets{*m_descriptor_sets[frame]};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, sets, {});
	cmd.dispatch(total_workgroups, 1, 1);

	// Make compute writes visible to subsequent vertex-input reads.
	vk::MemoryBarrier2 compute_done{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput,
		.dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead,
	};
	vk::DependencyInfo dep{
		.memoryBarrierCount = 1,
		.pMemoryBarriers = &compute_done,
	};
	cmd.pipelineBarrier2(dep);
}

uint32_t SkinningPrePass::getSkinnedVertexOffset(Entity entity, uint32_t frame_index,
                                                  const PbrMegaBuffer& mega_buffer) const {
	auto it = m_slots.find(entity.index());
	if (it == m_slots.end() || !mega_buffer.isValid())
		return INVALID_OFFSET;
	return mega_buffer.getDynamicRegionBase()
	     + frame_index * mega_buffer.getDynamicRegionCapacity()
	     + it->second.offset;
}

} // namespace ve