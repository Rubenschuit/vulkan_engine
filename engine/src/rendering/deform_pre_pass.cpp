#include "pch.hpp"
#include "rendering/deform_pre_pass.hpp"
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


static constexpr uint32_t DEFORM_HAS_SKIN  = 0x1;
static constexpr uint32_t DEFORM_HAS_MORPH = 0x2;

struct DeformWorkgroupInfo {
	uint32_t vertex_count;
	uint32_t palette_offset;
	uint32_t input_vertex_offset;
	uint32_t input_skin_offset;
	uint32_t output_vertex_offset;
	uint32_t local_vertex_base;
	uint32_t deform_flags;
	uint32_t target_count;
	uint32_t weight_offset;
	uint32_t morph_base;
	uint32_t _pad0;
	uint32_t _pad1;
};
static_assert(sizeof(DeformWorkgroupInfo) == 48, "DeformWorkgroupInfo must match shader stride");

DeformPrePass::DeformPrePass(VeDevice& device, VeDescriptorPool& descriptor_pool,
                                 std::filesystem::path shader_path, EventBus& event_bus)
	: m_ve_device(device), m_descriptor_pool(descriptor_pool),
	  m_shader_path(std::move(shader_path)) {

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_palette_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(glm::mat4),
			MAX_SKINNING_PALETTE_MATRICES,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_palette_ssbos[i]->map();

		m_deformed_offset_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(uint32_t),
			MAX_GPU_OBJECTS,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_deformed_offset_ssbos[i]->map();
		std::memset(m_deformed_offset_ssbos[i]->getMappedMemory(), 0,
		            sizeof(uint32_t) * MAX_GPU_OBJECTS);

		m_wg_info_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(DeformWorkgroupInfo),
			MAX_SKINNING_WG_INFO_ENTRIES,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_wg_info_ssbos[i]->map();

		m_morph_weight_ssbos[i] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(float),
			MAX_MORPH_WEIGHTS,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
		m_morph_weight_ssbos[i]->map();
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
		if (m_registry)
			seedDeformableSlots(*m_registry);
	});
}

DeformPrePass::~DeformPrePass() = default;

void DeformPrePass::createSetLayout() {
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // mega VBO (read bind-pose + write skinned output)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // mega skin VBO (read joints + weights)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // palette
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // mega shadow VBO (write position-only)
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // per-workgroup info table
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // morph position deltas
		.addBinding(6, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // morph normal deltas
		.addBinding(7, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // morph weights (per-frame)
		.build();
}

void DeformPrePass::createPipelineLayout() {
	std::array<vk::DescriptorSetLayout, 1> set_layouts{*m_set_layout->getDescriptorSetLayout()};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void DeformPrePass::createPipeline() {
	m_compute_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path, m_pipeline_layout);
}

void DeformPrePass::subscribeToRegistry(Registry& registry) {
	m_registry = &registry;
	resetAllocator();

	// Seed slots for entities that already need deform (skin or morph) at scene-load time.
	seedDeformableSlots(registry);

	registry.events().subscribe<EntityDestroyedEvent>(
		[this](const EntityDestroyedEvent& e) { freeSlotForEntity(e.entity); });

	// A mesh entity keeps its slot while ANY deform source remains.
	auto stillNeedsDeform = [this](Entity e) -> bool {
		if (!m_registry)
			return false;
		auto* mc = m_registry->getComponent<MeshComponent>(e);
		VeMesh* mesh = mc ? mc->getMesh() : nullptr;
		if (!mesh)
			return false;
		if (mesh->hasSkinning() && m_registry->hasComponent<SkinComponent>(e))
			return true;
		if (mesh->hasMorphTargets() && m_registry->hasComponent<MorphComponent>(e))
			return true;
		return false;
	};

	registry.events().subscribe<ComponentRemovedEvent<MeshComponent>>(
		[this](const ComponentRemovedEvent<MeshComponent>& e) { freeSlotForEntity(e.entity); });
	registry.events().subscribe<ComponentRemovedEvent<SkinComponent>>(
		[this, stillNeedsDeform](const ComponentRemovedEvent<SkinComponent>& e) {
			if (!stillNeedsDeform(e.entity)) freeSlotForEntity(e.entity);
		});
	registry.events().subscribe<ComponentRemovedEvent<MorphComponent>>(
		[this, stillNeedsDeform](const ComponentRemovedEvent<MorphComponent>& e) {
			if (!stillNeedsDeform(e.entity)) freeSlotForEntity(e.entity);
		});

	auto allocFromMesh = [this](Entity e) {
		if (!m_registry)
			return;
		auto* mc = m_registry->getComponent<MeshComponent>(e);
		if (mc && mc->hasMesh())
			allocateSlotForEntity(e, mc->getMesh());
	};
	registry.events().subscribe<ComponentAddedEvent<SkinComponent>>(
		[allocFromMesh](const ComponentAddedEvent<SkinComponent>& e) { allocFromMesh(e.entity); });
	registry.events().subscribe<ComponentAddedEvent<MorphComponent>>(
		[allocFromMesh](const ComponentAddedEvent<MorphComponent>& e) { allocFromMesh(e.entity); });
	registry.events().subscribe<ComponentAddedEvent<MeshComponent>>(
		[this](const ComponentAddedEvent<MeshComponent>& e) {
			if (e.component.hasMesh())
				allocateSlotForEntity(e.entity, e.component.getMesh());
		});

	registry.events().subscribe<MeshDataChangedEvent>(
		[this](const MeshDataChangedEvent& e) {
			if (!m_registry)
				return;
			auto* mc = m_registry->getComponent<MeshComponent>(e.entity);
			if (!mc || !mc->hasMesh())
				return;
			freeSlotForEntity(e.entity);
			allocateSlotForEntity(e.entity, mc->getMesh());
		});
}

void DeformPrePass::seedDeformableSlots(Registry& registry) {
	// allocateSlotForEntity is idempotent, so a skin+morph entity visited
	// by both views allocates once.
	for (auto [entity, mc, sc] : registry.view<MeshComponent, SkinComponent>())
		allocateSlotForEntity(entity, mc.getMesh());
	for (auto [entity, mc, morph] : registry.view<MeshComponent, MorphComponent>())
		allocateSlotForEntity(entity, mc.getMesh());
}

void DeformPrePass::resetAllocator() {
	m_slots.clear();
	m_free_ranges.clear();
	m_free_ranges.push_back({0, MAX_DEFORMED_VERTICES_PER_FRAME});
}

bool DeformPrePass::allocateSlotForEntity(Entity entity, VeMesh* mesh) {
	if (!mesh || (!mesh->hasSkinning() && !mesh->hasMorphTargets()))
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

	VE_LOGW("DeformPrePass: dynamic region overflow allocating " << need
		<< " vertices for entity " << entity.id()
		<< " (cap=" << MAX_DEFORMED_VERTICES_PER_FRAME << "); entity will not render");
	return false;
}

void DeformPrePass::freeSlotForEntity(Entity entity) {
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

void DeformPrePass::updatePalette(Registry& registry, uint32_t frame_index, VeThreadPool* thread_pool) {
	auto& dispatches = m_pending_dispatches[frame_index];
	dispatches.clear();

	auto* palette_ptr = static_cast<glm::mat4*>(m_palette_ssbos[frame_index]->getMappedMemory());
	auto* weight_ptr = static_cast<float*>(m_morph_weight_ssbos[frame_index]->getMappedMemory());
	uint32_t palette_cursor = 0;
	uint32_t weight_cursor = 0;

	bool palette_overflow_warned = false;
	bool weight_overflow_warned = false;
	// Only skinned/morph entities need deform work
	m_deform_scratch.clear();
	for (auto [entity, sc] : registry.view<SkinComponent>())
		m_deform_scratch.push_back(entity);
	for (auto [entity, morph] : registry.view<MorphComponent>())
		if (!registry.hasComponent<SkinComponent>(entity))
			m_deform_scratch.push_back(entity);

	for (Entity entity : m_deform_scratch) {
		auto* mcp = registry.getComponent<MeshComponent>(entity);
		if (!mcp)
			continue;
		MeshComponent& mc = *mcp;
		VeMesh* mesh = mc.getMesh();
		if (!mesh || !m_slots.count(entity.index()))
			continue;

		const auto* sc = registry.getComponent<SkinComponent>(entity);
		const bool wants_skin = mesh->hasSkinning() && sc;
		const bool wants_morph = mesh->hasMorphTargets() && registry.hasComponent<MorphComponent>(entity);
		if (!wants_skin && !wants_morph)
			continue;

		uint32_t deform_flags = 0;
		uint32_t palette_offset = 0;
		uint32_t target_count = 0;
		uint32_t weight_offset = 0;

		if (wants_skin) {
			const auto& joints = sc->getJointEntities();
			const auto& ibms = sc->getInverseBindMatrices();
			// A skinned entity that cannot be skinned this frame renders bind pose
			if (joints.empty() || joints.size() != ibms.size())
				continue;
			uint32_t joint_count = static_cast<uint32_t>(joints.size());
			if (palette_cursor + joint_count > MAX_SKINNING_PALETTE_MATRICES) {
				if (!palette_overflow_warned) {
					VE_LOGW("DeformPrePass: palette capacity exceeded (cap="
					        << MAX_SKINNING_PALETTE_MATRICES << "); some skinned meshes will render bind pose this frame");
					palette_overflow_warned = true;
				}
				continue;
			}
			deform_flags |= DEFORM_HAS_SKIN;
			palette_offset = palette_cursor;
			palette_cursor += joint_count;
			registry.primeWorldTransform(entity);
			for (Entity je : joints)
				if (!je.isNull() && registry.isAlive(je))
					registry.primeWorldTransform(je);
		}

		if (wants_morph) {
			const auto& weights = registry.getComponent<MorphComponent>(entity)->getWeights();
			uint32_t n = mesh->getMorphTargetCount();
			if (weight_cursor + n > MAX_MORPH_WEIGHTS) {
				if (!weight_overflow_warned) {
					VE_LOGW("DeformPrePass: morph weight capacity exceeded (cap="
					        << MAX_MORPH_WEIGHTS << "); some morph meshes will be skipped this frame");
					weight_overflow_warned = true;
				}
			} else {
				deform_flags |= DEFORM_HAS_MORPH;
				target_count = n;
				weight_offset = weight_cursor;
				for (uint32_t t = 0; t < n; t++)
					weight_ptr[weight_cursor + t] = (t < weights.size()) ? weights[t] : 0.0f;
				weight_cursor += n;
			}
		}

		if (deform_flags == 0)
			continue;

		// Mark the mesh's world AABB stale once per frame so downstream getWorldAABB()
		// recomputes exactly once.
		mc.invalidateWorldAABB();

		dispatches.push_back({
			.entity = entity,
			.vertex_count = mesh->getVertexCount(),
			.palette_offset = palette_offset,
			.deform_flags = deform_flags,
			.target_count = target_count,
			.weight_offset = weight_offset,
		});
	}

	if (dispatches.empty())
		return;

	// Compute palette matrices. Workers write disjoint slices of palette_ptr
	auto compute_one = [&](const DeformDispatch& d) {
		if ((d.deform_flags & DEFORM_HAS_SKIN) == 0)
			return;
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
	} else { // fallback to single-threaded if too few dispatches or no thread pool
		for (const auto& d : dispatches)
			compute_one(d);
	}
}

void DeformPrePass::updateDeformedOffsets(const GpuSceneManager& gpu_scene,
                                            const PbrMegaBuffer& mega_buffer,
                                            uint32_t frame_index) {
	if (!mega_buffer.isValid())
		return;

	auto* table = static_cast<uint32_t*>(m_deformed_offset_ssbos[frame_index]->getMappedMemory());
	auto& last_written = m_last_written_gpu_ids[frame_index];

	for (uint32_t prev_id : last_written)
		table[prev_id] = 0;
	last_written.clear();

	const uint32_t dynamic_base = mega_buffer.getDynamicRegionBase();
	const uint32_t dynamic_capacity = mega_buffer.getDynamicRegionCapacity();
	const uint32_t frame_subspace_offset = dynamic_base + frame_index * dynamic_capacity;

	for (const DeformDispatch& d : m_pending_dispatches[frame_index]) {
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

void DeformPrePass::refreshDescriptors(uint32_t frame_index, PbrMegaBuffer& mega_buffer) {
	// Rewrite only when the mega buffer actually (re)built.
	if (m_cached_mega_generation[frame_index] == mega_buffer.generation())
		return;

	// Skin / morph regions may be absent; fall back to the mega VBO (a valid storage
	// buffer) so the descriptor stays valid. The shader never reads an absent region.
	VeBuffer* skin_buf = mega_buffer.hasSkinData() ? mega_buffer.getMegaSkinVbo() : mega_buffer.getMegaVbo();
	VeBuffer* morph_pos_buf = mega_buffer.hasMorphData() ? mega_buffer.getMegaMorphPosition() : mega_buffer.getMegaVbo();
	VeBuffer* morph_nrm_buf = mega_buffer.hasMorphData() ? mega_buffer.getMegaMorphNormal() : mega_buffer.getMegaVbo();

	auto vbo_info = mega_buffer.getMegaVbo()->getDescriptorInfo();
	auto skin_info = skin_buf->getDescriptorInfo();
	auto palette_info = m_palette_ssbos[frame_index]->getDescriptorInfo();
	auto shadow_info = mega_buffer.getMegaShadowVbo()->getDescriptorInfo();
	auto wg_info = m_wg_info_ssbos[frame_index]->getDescriptorInfo();
	auto morph_pos_info = morph_pos_buf->getDescriptorInfo();
	auto morph_nrm_info = morph_nrm_buf->getDescriptorInfo();
	auto weight_info = m_morph_weight_ssbos[frame_index]->getDescriptorInfo();

	VeDescriptorWriter(*m_set_layout, m_descriptor_pool)
		.writeBuffer(0, &vbo_info)
		.writeBuffer(1, &skin_info)
		.writeBuffer(2, &palette_info)
		.writeBuffer(3, &shadow_info)
		.writeBuffer(4, &wg_info)
		.writeBuffer(5, &morph_pos_info)
		.writeBuffer(6, &morph_nrm_info)
		.writeBuffer(7, &weight_info)
		.build(m_descriptor_sets[frame_index]);

	m_cached_mega_generation[frame_index] = mega_buffer.generation();
}

void DeformPrePass::dispatch(VeFrameInfo& fi, PbrMegaBuffer& mega_buffer) {
	uint32_t frame = fi.current_frame;
	const auto& dispatches = m_pending_dispatches[frame];
	if (dispatches.empty() || !mega_buffer.isValid()
		|| (!mega_buffer.hasSkinData() && !mega_buffer.hasMorphData()))
		return;

	refreshDescriptors(frame, mega_buffer);

	auto& cmd = fi.compute_command_buffer;
	auto& registry = *fi.registry;

	const uint32_t dynamic_base = mega_buffer.getDynamicRegionBase();
	const uint32_t dynamic_capacity = mega_buffer.getDynamicRegionCapacity();
	const uint32_t frame_subspace_offset = dynamic_base + frame * dynamic_capacity;

	auto* wg_info_ptr = static_cast<DeformWorkgroupInfo*>(m_wg_info_ssbos[frame]->getMappedMemory());
	uint32_t total_workgroups = 0;
	bool overflow_warned = false;

	for (const DeformDispatch& d : dispatches) {
		Entity entity = d.entity;
		if (!registry.isAlive(entity))
			continue;
		auto* mc = registry.getComponent<MeshComponent>(entity);
		if (!mc)
			continue;
		VeMesh* mesh = mc->getMesh();
		if (!mesh)
			continue;

		const auto* entry = mega_buffer.getEntry(mesh);
		if (!entry)
			continue;

		// Drop deform sources whose mega region is missing for this mesh.
		bool has_skin = (d.deform_flags & DEFORM_HAS_SKIN)
			&& entry->skin_vertex_offset != PbrMegaBuffer::NO_SKIN_OFFSET;
		bool has_morph = (d.deform_flags & DEFORM_HAS_MORPH)
			&& entry->morph_offset != PbrMegaBuffer::NO_MORPH_OFFSET;
		uint32_t flags = (has_skin ? DEFORM_HAS_SKIN : 0u) | (has_morph ? DEFORM_HAS_MORPH : 0u);
		if (flags == 0)
			continue;

		auto slot_it = m_slots.find(entity.index());
		if (slot_it == m_slots.end())
			continue;

		const uint32_t groups = (d.vertex_count + SKINNING_WORKGROUP_SIZE - 1) / SKINNING_WORKGROUP_SIZE;
		if (total_workgroups + groups > MAX_SKINNING_WG_INFO_ENTRIES) {
			if (!overflow_warned) {
				VE_LOGW("DeformPrePass: workgroup table overflow (cap="
				        << MAX_SKINNING_WG_INFO_ENTRIES << "); deform truncated this frame");
				overflow_warned = true;
			}
			break;
		}

		const uint32_t out_offset = frame_subspace_offset + slot_it->second.offset;
		for (uint32_t g = 0; g < groups; g++) {
			wg_info_ptr[total_workgroups + g] = DeformWorkgroupInfo{
				.vertex_count        = d.vertex_count,
				.palette_offset      = d.palette_offset,
				.input_vertex_offset = entry->vertex_offset,
				.input_skin_offset   = has_skin ? entry->skin_vertex_offset : 0u,
				.output_vertex_offset = out_offset,
				.local_vertex_base   = g * SKINNING_WORKGROUP_SIZE,
				.deform_flags        = flags,
				.target_count        = has_morph ? d.target_count : 0u,
				.weight_offset       = d.weight_offset,
				.morph_base          = has_morph ? entry->morph_offset : 0u,
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
}

uint32_t DeformPrePass::getDeformedVertexOffset(Entity entity, uint32_t frame_index,
                                                  const PbrMegaBuffer& mega_buffer) const {
	auto it = m_slots.find(entity.index());
	if (it == m_slots.end() || !mega_buffer.isValid())
		return INVALID_OFFSET;
	return mega_buffer.getDynamicRegionBase()
	     + frame_index * mega_buffer.getDynamicRegionCapacity()
	     + it->second.offset;
}

} // namespace ve