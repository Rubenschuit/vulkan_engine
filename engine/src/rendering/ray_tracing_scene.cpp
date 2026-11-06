#include "pch.hpp"
#include "rendering/ray_tracing_scene.hpp"
#include "rendering/managers/pbr_mega_buffer.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/deform_pre_pass.hpp"
#include "resources/ve_material_properties.hpp"
#include "scene/ve_registry.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_buffer.hpp"
#include "utils/ve_log.hpp"

#include <glm/glm.hpp>
#include <algorithm>
#include <utility>

namespace ve {

namespace {
// glm is column-major, VkTransformMatrixKHR a row-major 3x4.
vk::TransformMatrixKHR toTransformMatrix(const glm::mat4& m) {
	vk::TransformMatrixKHR out{};
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 4; col++)
			out.matrix[row][col] = m[col][row];
	return out;
}

// A refit keeps the tree the last full build chose and only moves its boxes,
// so traversal degrades as instances wander from where they were built; a full
// build every so many refits puts a bound on that.
constexpr uint32_t TLAS_REFITS_PER_BUILD = 64;

constexpr uint32_t NO_INSTANCE = ~0u;
constexpr vk::DeviceSize INSTANCE_STRIDE = sizeof(vk::AccelerationStructureInstanceKHR);

// A build scratch has to START at minAccelerationStructureScratchOffsetAlignment.
vk::DeviceAddress alignUp(vk::DeviceAddress address, vk::DeviceSize alignment) {
	return (address + alignment - 1) & ~(static_cast<vk::DeviceAddress>(alignment) - 1);
}

// Floored at 256, the largest value the spec allows this limit to report
vk::DeviceSize scratchAlignment(VeDevice& device) {
	auto chain = device.getPhysicalDevice()
		.getProperties2<vk::PhysicalDeviceProperties2,
			vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
	const auto& as_props = chain.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
	return std::max<vk::DeviceSize>(as_props.minAccelerationStructureScratchOffsetAlignment, 256);
}

// The triangle geometry one BLAS is built from. Indices in the mega buffer are
// mesh-local, so vertex_offset is applied through the build range as
// firstVertex. maxVertex bounds firstVertex + index, counted from vertexData,
// so it is the whole buffer, not the part past the offset.
vk::AccelerationStructureGeometryKHR trianglesFrom(vk::DeviceAddress vertices,
	vk::DeviceAddress indices, uint32_t vertex_capacity) {
	vk::AccelerationStructureGeometryKHR geometry{
		.geometryType = vk::GeometryTypeKHR::eTriangles,
		.flags = vk::GeometryFlagBitsKHR::eOpaque,
	};
	auto& triangles = geometry.geometry.triangles;
	triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
	triangles.vertexData.deviceAddress = vertices;
	triangles.vertexStride = sizeof(glm::vec3);
	triangles.maxVertex = vertex_capacity - 1;
	triangles.indexType = vk::IndexType::eUint32;
	triangles.indexData.deviceAddress = indices;
	return geometry;
}

vk::AccelerationStructureGeometryKHR instancesFrom(vk::DeviceAddress instances) {
	vk::AccelerationStructureGeometryKHR geometry{
		.geometryType = vk::GeometryTypeKHR::eInstances,
		.flags = vk::GeometryFlagBitsKHR::eOpaque,
	};
	geometry.geometry.instances = vk::AccelerationStructureGeometryInstancesDataKHR{};
	geometry.geometry.instances.arrayOfPointers = vk::False;
	geometry.geometry.instances.data.deviceAddress = instances;
	return geometry;
}

// The lod 0 range an object traces, when it sits inside the buffers.
bool lod0Range(const CpuObjectData& data, uint32_t vertex_capacity, uint32_t index_capacity,
	uint32_t& first_index, uint32_t& index_count) {
	if (data.lod_count == 0 || data.index_count[0] == 0)
		return false;
	if (data.vertex_offset >= vertex_capacity)
		return false;
	if (static_cast<uint64_t>(data.first_index[0]) + data.index_count[0] > index_capacity)
		return false;
	first_index = data.first_index[0];
	index_count = data.index_count[0];
	return true;
}

// Objects that belong in the geometry-keyed BLAS set, and the lod 0 range each
// traces.
bool tracedGeometry(const CpuObjectData& data, uint32_t vertex_capacity, uint32_t index_capacity,
	bool include_inactive, uint32_t& first_index, uint32_t& index_count) {
	uint32_t rejected = ObjectFlag::RT_EXCLUDED;
	if (!include_inactive)
		rejected |= ObjectFlag::INACTIVE;
	if ((data.object_flags & rejected) != 0)
		return false;
	return lod0Range(data, vertex_capacity, index_capacity, first_index, index_count);
}

// Deformed objects that get a BLAS of their own, refitted every frame.
bool deformedGeometry(const CpuObjectData& data, uint32_t vertex_capacity, uint32_t index_capacity,
	uint32_t& first_index, uint32_t& index_count) {
	if ((data.object_flags & ObjectFlag::DEFORMED) == 0)
		return false;
	if ((data.object_flags & (ObjectFlag::IS_TRANSPARENT | ObjectFlag::INACTIVE)) != 0)
		return false;
	return lod0Range(data, vertex_capacity, index_capacity, first_index, index_count);
}

// Refitted every frame, so the full builds the refits start from favour the trace
constexpr vk::BuildAccelerationStructureFlagsKHR DEFORMED_BLAS_FLAGS =
	vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
	| vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
constexpr uint32_t DEFORMED_REFITS_PER_BUILD = 64;
constexpr uint32_t NO_SLOT = ~0u;
} // namespace

size_t RayTracingScene::GeometryKeyHash::operator()(const GeometryKey& k) const {
	size_t h = k.vertex_offset;
	h = h * 0x9E3779B97F4A7C15ull + k.first_index;
	h = h * 0x9E3779B97F4A7C15ull + k.index_count;
	return h;
}

RayTracingScene::RayTracingScene(VeDevice& device)
	: m_ve_device(device), m_scratch_alignment(scratchAlignment(device)) {
	try {
		buildEmptyTlas();
	} catch (const std::exception& e) {
		// Failed from the start, the way update() fails: the ray query trace
		// never runs, so nothing reads binding 31 in its absence.
		VE_LOGE("RT: empty TLAS build failed, ray query disabled: " << e.what());
		m_empty_tlas = {};
		m_failed = true;
	}
}
RayTracingScene::~RayTracingScene() = default;

void RayTracingScene::buildEmptyTlas() {
	VeBuffer instances(m_ve_device, sizeof(vk::AccelerationStructureInstanceKHR), 1,
		vk::BufferUsageFlagBits::eShaderDeviceAddress
			| vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
		vk::MemoryPropertyFlagBits::eDeviceLocal);
	vk::AccelerationStructureGeometryKHR geometry = instancesFrom(instances.getDeviceAddress());
	vk::AccelerationStructureBuildGeometryInfoKHR build_info{
		.type = vk::AccelerationStructureTypeKHR::eTopLevel,
		.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
		.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
		.geometryCount = 1,
		.pGeometries = &geometry,
	};
	const uint32_t instance_count = 0;
	auto sizes = m_ve_device.getDevice().getAccelerationStructureBuildSizesKHR(
		vk::AccelerationStructureBuildTypeKHR::eDevice, build_info, instance_count);
	m_empty_tlas = createStructure(sizes.accelerationStructureSize,
		vk::AccelerationStructureTypeKHR::eTopLevel, "RT Empty TLAS");
	VeBuffer scratch(m_ve_device, sizes.buildScratchSize + m_scratch_alignment, 1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
		vk::MemoryPropertyFlagBits::eDeviceLocal);
	build_info.dstAccelerationStructure = **m_empty_tlas.handle;
	build_info.scratchData.deviceAddress = alignUp(scratch.getDeviceAddress(), m_scratch_alignment);
	vk::AccelerationStructureBuildRangeInfoKHR range{.primitiveCount = instance_count};
	auto cmd = m_ve_device.beginSingleTimeCommands(QueueKind::Compute);
	cmd->buildAccelerationStructuresKHR(build_info, &range);
	m_ve_device.endSingleTimeCommands(*cmd, QueueKind::Compute);
}

void RayTracingScene::reset() {
	m_blas.clear();
	m_geometry_to_blas.clear();
	m_deformed.clear();
	for (auto& buffer : m_deformed_scratch)
		buffer.reset();
	m_deformed_scratch_size = 0;
	m_static_instance_count = 0;
	m_deformed_bytes = 0;
	m_retired.clear();
	m_retired_buffers.clear();
	m_tlas = {};
	for (auto& buffer : m_tlas_scratch)
		buffer.reset();
	m_tlas_latest = 0;
	m_tlas_refits = 0;
	m_instances.clear();
	m_instance_entities.clear();
	m_instance_of_gpu_id.clear();
	for (auto& buffer : m_instance_buffers)
		buffer.reset();
	m_geometry_table.clear();
	for (auto& buffer : m_geometry_buffers)
		buffer.reset();
	m_geometry_uploaded = {};
	m_geometry_generation = 0;
	m_instance_count = 0;
	m_instance_capacity = 0;
	m_built_generation = 0;
	m_built_geometry_generation = 0;
	m_built_instance_generation = 0;
	m_store_bytes = 0;
}

bool RayTracingScene::holdsResources() const {
	if (!m_blas.empty() || !m_deformed.empty())
		return true;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		if (m_tlas[i].handle || m_tlas[i].storage || m_tlas_scratch[i] || m_instance_buffers[i] || m_geometry_buffers[i]
			|| m_deformed_scratch[i])
			return true;
	return !m_retired.empty() || !m_retired_buffers.empty();
}

void RayTracingScene::retireDeformed() {
	for (DeformedInstance& deformed : m_deformed)
		for (Structure& blas : deformed.blas)
			if (blas.handle)
				m_retired.emplace_back(m_frame_counter, std::move(blas));
	m_deformed.clear();
	m_deformed_bytes = 0;
}

void RayTracingScene::retireAll() {
	for (auto& blas : m_blas)
		m_retired.emplace_back(m_frame_counter, std::move(blas));
	m_blas.clear();
	m_geometry_to_blas.clear();
	retireDeformed();
	m_deformed_scratch_size = 0;
	m_static_instance_count = 0;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (m_deformed_scratch[i])
			m_retired_buffers.emplace_back(m_frame_counter, std::move(m_deformed_scratch[i]));
		if (m_tlas[i].handle || m_tlas[i].storage)
			m_retired.emplace_back(m_frame_counter, std::move(m_tlas[i]));
		m_tlas[i] = {};
		if (m_tlas_scratch[i])
			m_retired_buffers.emplace_back(m_frame_counter, std::move(m_tlas_scratch[i]));
		if (m_instance_buffers[i])
			m_retired_buffers.emplace_back(m_frame_counter, std::move(m_instance_buffers[i]));
		if (m_geometry_buffers[i])
			m_retired_buffers.emplace_back(m_frame_counter, std::move(m_geometry_buffers[i]));
	}
	m_geometry_table.clear();
	m_tlas_refits = 0;
	m_instances.clear();
	m_instance_entities.clear();
	m_instance_of_gpu_id.clear();
	m_instance_count = 0;
	m_instance_capacity = 0;
	m_built_generation = 0;
	m_store_bytes = 0;
}

RayTracingScene::Structure RayTracingScene::createStructure(
	vk::DeviceSize size, vk::AccelerationStructureTypeKHR type, const char* name) {
	Structure out;
	out.storage = std::make_unique<VeBuffer>(m_ve_device, size, 1,
		vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR
			| vk::BufferUsageFlagBits::eShaderDeviceAddress,
		vk::MemoryPropertyFlagBits::eDeviceLocal);
	out.storage->setDebugName(name);
	out.handle = std::make_unique<vk::raii::AccelerationStructureKHR>(
		m_ve_device.getDevice(),
		vk::AccelerationStructureCreateInfoKHR{
			.buffer = out.storage->getBuffer(),
			.offset = 0,
			.size = size,
			.type = type,
		});
	out.address = m_ve_device.getDevice().getAccelerationStructureAddressKHR(
		vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = **out.handle});
	return out;
}

void RayTracingScene::uploadGeometryTable(uint32_t frame_index) {
	if (!m_geometry_buffers[frame_index] || m_geometry_table.empty())
		return;
	if (m_geometry_uploaded[frame_index] == m_geometry_generation)
		return;
	m_geometry_buffers[frame_index]->writeToBuffer(m_geometry_table.data(),
		m_geometry_table.size() * sizeof(RtInstanceGeometry));
	m_geometry_uploaded[frame_index] = m_geometry_generation;
}

bool RayTracingScene::update(vk::raii::CommandBuffer& cmd, uint32_t frame_index,
	const PbrMegaBuffer& mega_buffer, GpuSceneManager& gpu_scene, const DeformPrePass& deform,
	bool trace_deformed, const Registry& registry) {
	// Free what an earlier update retired
	m_frame_counter++;
	auto expired = [this](const auto& entry) {
		return m_frame_counter - entry.first >= MAX_FRAMES_IN_FLIGHT;
	};
	std::erase_if(m_retired, expired);
	std::erase_if(m_retired_buffers, expired);
	if (m_failed)
		return false;
	if (!mega_buffer.isValid() || gpu_scene.getRegisteredObjects().empty()) {
		retireAll();
		return true;
	}

	// A mega buffer rebuild moves every geometry range, so the BLAS set goes
	// with it; so does a change in which geometry is registered. The instance
	// set follows any instance that appeared, went, or was hidden or
	// rewritten; one that only moved is patched in place.
	const bool rebuild_blas = mega_buffer.generation() != m_built_generation
		|| gpu_scene.geometrySetGeneration() != m_built_geometry_generation;
	const bool regather = rebuild_blas
		|| gpu_scene.instanceGeneration() != m_built_instance_generation
		|| trace_deformed != m_gathered_deformed;
	gpu_scene.takeMovedTraceable(m_moved);
	const bool moved = !regather && patchMovedInstances(registry);
	if (!regather && !moved && m_deformed.empty()) {
		// A frame that skipped the build still needs its own copy of whatever
		// the last build produced.
		uploadGeometryTable(frame_index);
		return true;
	}
	try {
		if (rebuild_blas) {
			buildBlas(cmd, mega_buffer, gpu_scene);
			m_built_generation = mega_buffer.generation();
			m_built_geometry_generation = gpu_scene.geometrySetGeneration();
		}
		if (regather) {
			gatherInstances(gpu_scene, registry, trace_deformed);
			m_built_instance_generation = gpu_scene.instanceGeneration();
		}
		buildDeformedBlas(cmd, frame_index, mega_buffer, deform, registry);
		buildTlas(cmd, frame_index, !regather);
	} catch (const std::exception& e) {
		// Builds already recorded into cmd still name what is retired here,
		// which is why it is retired rather than freed.
		VE_LOGE("RT: acceleration structure allocation failed, ray query disabled: " << e.what());
		retireAll();
		m_failed = true;
		return false;
	}
	return true;
}

void RayTracingScene::buildBlas(vk::raii::CommandBuffer& cmd, const PbrMegaBuffer& mega_buffer,
	const GpuSceneManager& gpu_scene) {
	for (auto& blas : m_blas)
		m_retired.emplace_back(m_frame_counter, std::move(blas));
	m_blas.clear();
	m_geometry_to_blas.clear();
	// The TLAS is not rebuilt here, so its share of the store stays.
	m_store_bytes = 0;
	for (const Structure& tlas : m_tlas)
		if (tlas.storage)
			m_store_bytes += tlas.storage->getBufferSize();

	const auto& object_data = gpu_scene.getCpuObjectData();
	// The BLAS traces the shadow positions, not the 64 B shading vertices: same
	// vertex_offset, a third of the bandwidth.
	const vk::DeviceAddress vertex_address = mega_buffer.getMegaShadowVbo()->getDeviceAddress();
	const vk::DeviceAddress index_address = mega_buffer.getMegaIbo()->getDeviceAddress();
	const uint32_t vertex_capacity = mega_buffer.getMegaShadowVbo()->getInstanceCount();
	const uint32_t index_capacity = mega_buffer.getMegaIbo()->getInstanceCount();
	m_vertex_capacity = vertex_capacity;
	m_index_capacity = index_capacity;

	std::vector<GeometryKey> keys;
	for (const auto& [gpu_id, entity] : gpu_scene.getRegisteredObjects()) {
		if (gpu_id >= object_data.size())
			continue;
		uint32_t first_index;
		uint32_t index_count;
		if (!tracedGeometry(object_data[gpu_id], vertex_capacity, index_capacity, true, first_index, index_count))
			continue;
		GeometryKey key{object_data[gpu_id].vertex_offset, first_index, index_count};
		if (m_geometry_to_blas.emplace(key, static_cast<uint32_t>(keys.size())).second)
			keys.push_back(key);
	}
	if (keys.empty()) {
		// Nothing to trace. The old TLAS names structures just retired, so it
		// goes with them rather than staying as the last good one.
		retireAll();
		return;
	}

	// Sizes first: the largest single build sets the scratch, which every build
	// then reuses with a barrier between.
	std::vector<vk::AccelerationStructureBuildSizesInfoKHR> sizes(keys.size());
	vk::DeviceSize max_scratch = 0;
	for (size_t i = 0; i < keys.size(); i++) {
		vk::AccelerationStructureGeometryKHR geometry =
			trianglesFrom(vertex_address, index_address, vertex_capacity);
		vk::AccelerationStructureBuildGeometryInfoKHR build_info{
			.type = vk::AccelerationStructureTypeKHR::eBottomLevel,
			.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
			.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
			.geometryCount = 1,
			.pGeometries = &geometry,
		};
		sizes[i] = m_ve_device.getDevice().getAccelerationStructureBuildSizesKHR(
			vk::AccelerationStructureBuildTypeKHR::eDevice, build_info, keys[i].index_count / 3);
		max_scratch = std::max(max_scratch, sizes[i].buildScratchSize);
	}

	auto scratch = std::make_unique<VeBuffer>(m_ve_device, max_scratch + m_scratch_alignment, 1,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
		vk::MemoryPropertyFlagBits::eDeviceLocal);
	scratch->setDebugName("RT BLAS Scratch");
	const vk::DeviceAddress blas_scratch_address = alignUp(scratch->getDeviceAddress(), m_scratch_alignment);
	// Only the builds below use it. Retired before they are recorded, so a
	// throw part-way through cannot free it under the ones already queued.
	m_retired_buffers.emplace_back(m_frame_counter, std::move(scratch));

	m_blas.reserve(keys.size());
	for (size_t i = 0; i < keys.size(); i++) {
		Structure structure = createStructure(sizes[i].accelerationStructureSize,
			vk::AccelerationStructureTypeKHR::eBottomLevel, "RT BLAS");
		m_store_bytes += sizes[i].accelerationStructureSize;
		vk::AccelerationStructureGeometryKHR geometry =
			trianglesFrom(vertex_address, index_address, vertex_capacity);
		vk::AccelerationStructureBuildGeometryInfoKHR build_info{
			.type = vk::AccelerationStructureTypeKHR::eBottomLevel,
			.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
			.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
			.dstAccelerationStructure = **structure.handle,
			.geometryCount = 1,
			.pGeometries = &geometry,
		};
		build_info.scratchData.deviceAddress = blas_scratch_address;
		vk::AccelerationStructureBuildRangeInfoKHR range{
			.primitiveCount = keys[i].index_count / 3,
			.primitiveOffset = keys[i].first_index * static_cast<uint32_t>(sizeof(uint32_t)),
			.firstVertex = keys[i].vertex_offset,
			.transformOffset = 0,
		};
		cmd.buildAccelerationStructuresKHR(build_info, &range);
		// One scratch serves every build, so each waits on the one before it.
		vk::MemoryBarrier2 barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
			.srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
			.dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
			.dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR
				| vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
		};
		cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});

		m_blas.push_back(std::move(structure));
	}

	VE_LOGD("RT: built " << m_blas.size() << " BLAS over " << gpu_scene.getRegisteredObjects().size()
		<< " objects (" << (m_store_bytes / (1024 * 1024)) << " MB)");
}

void RayTracingScene::gatherInstances(const GpuSceneManager& gpu_scene, const Registry& registry,
	bool trace_deformed) {
	const auto& object_data = gpu_scene.getCpuObjectData();
	m_instances.clear();
	m_instance_entities.clear();
	m_instance_of_gpu_id.assign(object_data.size(), NO_INSTANCE);
	m_instance_count = 0;
	retireDeformed();
	m_gathered_deformed = trace_deformed;
	std::vector<RtInstanceGeometry> geometry_table;
	m_instances.reserve(gpu_scene.getRegisteredObjects().size());
	m_instance_entities.reserve(gpu_scene.getRegisteredObjects().size());
	geometry_table.reserve(gpu_scene.getRegisteredObjects().size());

	auto add_instance = [&](uint32_t gpu_id, Entity entity, uint32_t first_index, vk::DeviceAddress blas_address) {
		const CpuObjectData& data = object_data[gpu_id];
		m_instance_of_gpu_id[gpu_id] = static_cast<uint32_t>(m_instances.size());
		m_instance_entities.push_back(entity);
		geometry_table.push_back(RtInstanceGeometry{
			.vertex_offset = data.vertex_offset,
			.first_index = first_index,
			.material_index = data.material_index,
		});
		// Double-sided everywhere: the engine draws plenty of single-sided
		// geometry a ray legitimately reaches from behind.
		vk::GeometryInstanceFlagsKHR instance_flags = vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable;
		// The BLAS is built opaque, so masking is per instance and costs no
		// rebuild; only these instances hand candidates to the ray queries.
		if ((data.material_flags & MaterialFlag::ALPHA_MODE_MASK) == static_cast<uint32_t>(AlphaMode::MASK))
			instance_flags |= vk::GeometryInstanceFlagBitsKHR::eForceNoOpaque;
		m_instances.push_back(vk::AccelerationStructureInstanceKHR{
			.transform = toTransformMatrix(registry.getWorldTransform(entity)),
			.instanceCustomIndex = 0,
			.mask = data.emissive ? RT_INSTANCE_MASK_EMITTER : RT_INSTANCE_MASK_SURFACE,
			.instanceShaderBindingTableRecordOffset = 0,
			.flags = static_cast<VkGeometryInstanceFlagsKHR>(instance_flags),
			.accelerationStructureReference = blas_address,
		});
	};

	struct DeformedSource {
		uint32_t gpu_id;
		Entity entity;
		uint32_t vertex_count;
		uint32_t first_index;
		uint32_t index_count;
	};
	std::vector<DeformedSource> deformed;
	for (const auto& [gpu_id, entity] : gpu_scene.getRegisteredObjects()) {
		if (gpu_id >= object_data.size() || entity.isNull())
			continue;
		const CpuObjectData& data = object_data[gpu_id];
		uint32_t first_index;
		uint32_t index_count;
		if (deformedGeometry(data, m_vertex_capacity, m_index_capacity, first_index, index_count)) {
			if (!trace_deformed)
				continue;
			const auto* mesh = registry.getComponent<MeshComponent>(entity);
			const uint32_t vertex_count = mesh && mesh->getMesh() ? mesh->getMesh()->getVertexCount() : 0;
			if (vertex_count > 0 && static_cast<uint64_t>(data.vertex_offset) + vertex_count <= m_vertex_capacity)
				deformed.push_back({gpu_id, entity, vertex_count, first_index, index_count});
			continue;
		}
		if (!tracedGeometry(data, m_vertex_capacity, m_index_capacity, false, first_index, index_count))
			continue;
		auto it = m_geometry_to_blas.find(GeometryKey{data.vertex_offset, first_index, index_count});
		if (it == m_geometry_to_blas.end())
			continue;
		add_instance(gpu_id, entity, first_index, m_blas[it->second].address);
	}
	m_static_instance_count = static_cast<uint32_t>(m_instances.size());

	// Their BLAS are allocated on first build, and their BLAS references and
	// side-table vertex offsets are written every frame (buildDeformedBlas).
	// The host table keeps the bind-pose offset, so a regather that changed
	// nothing still compares equal and uploads nothing.
	vk::DeviceSize scratch_size = 0;
	m_deformed.reserve(deformed.size());
	for (const DeformedSource& source : deformed) {
		vk::AccelerationStructureGeometryKHR geometry = trianglesFrom(0, 0, source.vertex_count);
		vk::AccelerationStructureBuildGeometryInfoKHR build_info{
			.type = vk::AccelerationStructureTypeKHR::eBottomLevel,
			.flags = DEFORMED_BLAS_FLAGS,
			.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
			.geometryCount = 1,
			.pGeometries = &geometry,
		};
		const auto sizes = m_ve_device.getDevice().getAccelerationStructureBuildSizesKHR(
			vk::AccelerationStructureBuildTypeKHR::eDevice, build_info, source.index_count / 3);
		m_deformed.push_back(DeformedInstance{
			.blas_size = sizes.accelerationStructureSize,
			.scratch_offset = scratch_size,
			.instance = static_cast<uint32_t>(m_instances.size()),
			.bind_vertex_offset = object_data[source.gpu_id].vertex_offset,
			.vertex_count = source.vertex_count,
			.first_index = source.first_index,
			.index_count = source.index_count,
		});
		// One build call runs them all at once, so their scratch must not overlap.
		scratch_size = alignUp(scratch_size + std::max(sizes.buildScratchSize, sizes.updateScratchSize),
			m_scratch_alignment);
		add_instance(source.gpu_id, source.entity, source.first_index, 0);
	}
	m_deformed_scratch_size = scratch_size;
	if (m_deformed.empty())
		for (auto& buffer : m_deformed_scratch)
			if (buffer)
				m_retired_buffers.emplace_back(m_frame_counter, std::move(buffer));
	m_instance_count = static_cast<uint32_t>(m_instances.size());
	m_instances_stale.fill(true);
	for (auto& patches : m_instance_patches)
		patches.clear();
	if (m_instance_count == 0)
		return;

	if (m_instance_count > m_instance_capacity) {
		// Grown together so one capacity check covers both. The buffers being
		// replaced may still be read by the frames in flight, so they are
		// retired rather than freed here.
		for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
			if (m_instance_buffers[i])
				m_retired_buffers.emplace_back(m_frame_counter, std::move(m_instance_buffers[i]));
			if (m_geometry_buffers[i])
				m_retired_buffers.emplace_back(m_frame_counter, std::move(m_geometry_buffers[i]));
			m_instance_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
				INSTANCE_STRIDE, m_instance_count,
				vk::BufferUsageFlagBits::eShaderDeviceAddress
					| vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			m_instance_buffers[i]->setDebugName("RT TLAS Instances");
			m_instance_buffers[i]->map();
			m_geometry_buffers[i] = std::make_unique<VeBuffer>(m_ve_device,
				sizeof(RtInstanceGeometry), m_instance_count,
				vk::BufferUsageFlagBits::eStorageBuffer,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
			m_geometry_buffers[i]->setDebugName("RT Instance Geometry");
			m_geometry_buffers[i]->map();
		}
		m_instance_capacity = m_instance_count;
		m_geometry_uploaded = {};
		m_buffer_generation++;
	}
	// A regather that changed no instance's surface leaves the table
	// identical, so the frames in flight keep whatever they already hold.
	if (geometry_table != m_geometry_table) {
		m_geometry_table = std::move(geometry_table);
		m_geometry_generation++;
	}

	vk::AccelerationStructureGeometryKHR geometry = instancesFrom(0);
	vk::AccelerationStructureBuildGeometryInfoKHR build_info{
		.type = vk::AccelerationStructureTypeKHR::eTopLevel,
		.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
			| vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
		.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
		.geometryCount = 1,
		.pGeometries = &geometry,
	};
	m_tlas_sizes = m_ve_device.getDevice().getAccelerationStructureBuildSizesKHR(
		vk::AccelerationStructureBuildTypeKHR::eDevice, build_info, m_instance_count);
}

void RayTracingScene::buildDeformedBlas(vk::raii::CommandBuffer& cmd, uint32_t frame_index,
	const PbrMegaBuffer& mega_buffer, const DeformPrePass& deform, const Registry& registry) {
	if (m_deformed.empty())
		return;
	// The bind pose unless DeformPrePass wrote an
	// offset for the object this frame.
	std::vector<uint32_t> vertex_offsets(m_deformed.size());
	for (size_t i = 0; i < m_deformed.size(); i++)
		vertex_offsets[i] = m_deformed[i].bind_vertex_offset;
	for (const DeformPrePass::DeformedOffset& written : deform.getDeformedOffsets(frame_index)) {
		const uint32_t instance = written.gpu_id < m_instance_of_gpu_id.size()
			? m_instance_of_gpu_id[written.gpu_id] : NO_INSTANCE;
		if (instance != NO_INSTANCE && instance >= m_static_instance_count)
			vertex_offsets[instance - m_static_instance_count] = written.vertex_offset;
	}

	auto& scratch = m_deformed_scratch[frame_index];
	if (!scratch || scratch->getBufferSize() < m_deformed_scratch_size + m_scratch_alignment) {
		if (scratch)
			m_retired_buffers.emplace_back(m_frame_counter, std::move(scratch));
		scratch = std::make_unique<VeBuffer>(m_ve_device, m_deformed_scratch_size + m_scratch_alignment, 1,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
			vk::MemoryPropertyFlagBits::eDeviceLocal);
		scratch->setDebugName("RT Deformed BLAS Scratch");
	}
	const vk::DeviceAddress scratch_address = alignUp(scratch->getDeviceAddress(), m_scratch_alignment);
	const vk::DeviceAddress vertex_address = mega_buffer.getMegaShadowVbo()->getDeviceAddress();
	const vk::DeviceAddress index_address = mega_buffer.getMegaIbo()->getDeviceAddress();

	const size_t count = m_deformed.size();
	std::vector<vk::AccelerationStructureGeometryKHR> geometries(count);
	std::vector<vk::AccelerationStructureBuildGeometryInfoKHR> build_infos(count);
	std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges(count);
	std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> range_pointers(count);
	for (size_t i = 0; i < count; i++) {
		DeformedInstance& deformed = m_deformed[i];
		Structure& blas = deformed.blas[frame_index];
		if (!blas.handle) {
			blas = createStructure(deformed.blas_size, vk::AccelerationStructureTypeKHR::eBottomLevel,
				"RT Deformed BLAS");
			m_deformed_bytes += deformed.blas_size;
		}
		// The frame's vertices are addressed directly rather than through
		// firstVertex, which a refit has to keep from the build it starts from;
		// maxVertex then bounds the mesh, not the whole buffer.
		geometries[i] = trianglesFrom(vertex_address + vertex_offsets[i] * sizeof(glm::vec3), index_address,
			deformed.vertex_count);
		build_infos[i] = vk::AccelerationStructureBuildGeometryInfoKHR{
			.type = vk::AccelerationStructureTypeKHR::eBottomLevel,
			.flags = DEFORMED_BLAS_FLAGS,
			.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
			.dstAccelerationStructure = **blas.handle,
			.geometryCount = 1,
			.pGeometries = &geometries[i],
		};
		// Refitted from whichever slot was written last, which is this one
		// only when the frame between skipped the update.
		const bool refit = deformed.latest_slot != NO_SLOT && deformed.refits < DEFORMED_REFITS_PER_BUILD;
		if (refit) {
			build_infos[i].mode = vk::BuildAccelerationStructureModeKHR::eUpdate;
			build_infos[i].srcAccelerationStructure = **deformed.blas[deformed.latest_slot].handle;
		}
		deformed.refits = refit ? deformed.refits + 1 : 0;
		deformed.latest_slot = frame_index;
		build_infos[i].scratchData.deviceAddress = scratch_address + deformed.scratch_offset;
		ranges[i] = vk::AccelerationStructureBuildRangeInfoKHR{
			.primitiveCount = deformed.index_count / 3,
			.primitiveOffset = deformed.first_index * static_cast<uint32_t>(sizeof(uint32_t)),
			.firstVertex = 0,
			.transformOffset = 0,
		};
		range_pointers[i] = &ranges[i];

		// Deformed objects never reach m_moved, so the transform is rewritten here too.
		vk::AccelerationStructureInstanceKHR& instance = m_instances[deformed.instance];
		instance.transform = toTransformMatrix(registry.getWorldTransform(m_instance_entities[deformed.instance]));
		instance.accelerationStructureReference = blas.address;
		if (!m_instances_stale[frame_index])
			m_instance_patches[frame_index].push_back(deformed.instance);
	}
	// A refit reads the slot the previous frame wrote, in an earlier
	// submission to the same queue, as the TLAS refit does.
	vk::MemoryBarrier2 previous_builds{
		.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
		.srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
		.dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
		.dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
	};
	cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &previous_builds});
	cmd.buildAccelerationStructuresKHR(build_infos, range_pointers);

	uploadGeometryTable(frame_index);
	VeBuffer& geometry_buffer = *m_geometry_buffers[frame_index];
	for (size_t i = 0; i < count; i++) {
		const uint32_t instance = m_deformed[i].instance;
		RtInstanceGeometry entry = m_geometry_table[instance];
		entry.vertex_offset = vertex_offsets[i];
		geometry_buffer.writeToBuffer(&entry, sizeof(entry), instance * sizeof(RtInstanceGeometry));
	}
}

bool RayTracingScene::patchMovedInstances(const Registry& registry) {
	bool patched = false;
	for (uint32_t gpu_id : m_moved) {
		const uint32_t instance = gpu_id < m_instance_of_gpu_id.size() ? m_instance_of_gpu_id[gpu_id] : NO_INSTANCE;
		if (instance == NO_INSTANCE)
			continue;
		m_instances[instance].transform = toTransformMatrix(registry.getWorldTransform(m_instance_entities[instance]));
		// A frame index whose frames skip the build keeps collecting; once its
		// list is as long as the array, one whole write is cheaper than a flush.
		for (uint32_t f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
			if (m_instances_stale[f])
				continue;
			if (m_instance_patches[f].size() >= m_instance_count) {
				m_instances_stale[f] = true;
				m_instance_patches[f].clear();
				continue;
			}
			m_instance_patches[f].push_back(instance);
		}
		patched = true;
	}
	return patched;
}

void RayTracingScene::uploadInstances(uint32_t frame_index) {
	VeBuffer& buffer = *m_instance_buffers[frame_index];
	if (m_instances_stale[frame_index]) {
		buffer.writeToBuffer(m_instances.data(), m_instances.size() * INSTANCE_STRIDE);
		m_instances_stale[frame_index] = false;
	} else {
		for (uint32_t instance : m_instance_patches[frame_index])
			buffer.writeToBuffer(&m_instances[instance], INSTANCE_STRIDE, instance * INSTANCE_STRIDE);
	}
	m_instance_patches[frame_index].clear();
}

void RayTracingScene::buildTlas(vk::raii::CommandBuffer& cmd, uint32_t frame_index, bool allow_refit) {
	if (m_instance_count == 0)
		return;
	uploadInstances(frame_index);
	uploadGeometryTable(frame_index);

	vk::AccelerationStructureGeometryKHR geometry =
		instancesFrom(m_instance_buffers[frame_index]->getDeviceAddress());
	vk::AccelerationStructureBuildGeometryInfoKHR build_info{
		.type = vk::AccelerationStructureTypeKHR::eTopLevel,
		.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
			| vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
		.mode = vk::BuildAccelerationStructureModeKHR::eBuild,
		.geometryCount = 1,
		.pGeometries = &geometry,
	};
	const vk::DeviceSize scratch_size = std::max(m_tlas_sizes.buildScratchSize, m_tlas_sizes.updateScratchSize);

	// Refitted from the latest slot into this one
	const Structure& previous = m_tlas[m_tlas_latest];
	const bool refit = allow_refit && previous.handle && m_tlas_refits < TLAS_REFITS_PER_BUILD;
	if (refit) {
		build_info.mode = vk::BuildAccelerationStructureModeKHR::eUpdate;
		build_info.srcAccelerationStructure = **previous.handle;
	}

	const uint32_t slot = (m_tlas_latest + 1) % MAX_FRAMES_IN_FLIGHT;
	Structure& tlas = m_tlas[slot];
	if (!tlas.handle || tlas.storage->getBufferSize() < m_tlas_sizes.accelerationStructureSize) {
		if (tlas.handle) {
			m_store_bytes -= tlas.storage->getBufferSize();
			m_retired.emplace_back(m_frame_counter, std::move(tlas));
		}
		tlas = createStructure(m_tlas_sizes.accelerationStructureSize,
			vk::AccelerationStructureTypeKHR::eTopLevel, "RT TLAS");
		m_store_bytes += m_tlas_sizes.accelerationStructureSize;
	}
	auto& scratch = m_tlas_scratch[slot];
	if (!scratch || scratch->getBufferSize() < scratch_size + m_scratch_alignment) {
		if (scratch)
			m_retired_buffers.emplace_back(m_frame_counter, std::move(scratch));
		scratch = std::make_unique<VeBuffer>(m_ve_device,
			scratch_size + m_scratch_alignment, 1,
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress,
			vk::MemoryPropertyFlagBits::eDeviceLocal);
		scratch->setDebugName("RT TLAS Scratch");
	}
	build_info.dstAccelerationStructure = **tlas.handle;
	build_info.scratchData.deviceAddress = alignUp(scratch->getDeviceAddress(), m_scratch_alignment);

	// Ahead of every build, not only refits. The slot written here was last
	// traced at least MAX_FRAMES_IN_FLIGHT frames back, which the frame fence
	// covers, but it may also be what the previous build refitted from, as
	// recently as the frame before, on the same queue. A refit in turn reads
	// the latest slot, which the previous build wrote. The instance writes
	// above need no barrier: they land in host-coherent memory before the
	// submit, which makes them visible to the device.
	vk::MemoryBarrier2 previous_builds{
		.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
		.srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
		.dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
		.dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR
			| vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
	};
	cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &previous_builds});
	vk::AccelerationStructureBuildRangeInfoKHR range{.primitiveCount = m_instance_count};
	cmd.buildAccelerationStructuresKHR(build_info, &range);
	m_tlas_latest = slot;
	m_tlas_refits = refit ? m_tlas_refits + 1 : 0;
}

} // namespace ve
