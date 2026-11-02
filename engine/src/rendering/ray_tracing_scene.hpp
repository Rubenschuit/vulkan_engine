/*
 * RayTracingScene: the acceleration structures the RC tracer's ray queries run
 * against. One BLAS per distinct piece of geometry in the mega buffer, built
 * from the 12 B shadow positions and the shared index buffer at LOD 0, and a
 * TLAS over the active objects' world transforms.
 *
 * Only constructed when VeDevice::supportsRayQuery()
 *
 * Deformed geometry (skinned / morphed) is absent.
 * So is blended and transmissive geometry.
 * Alpha-masked instances are forced non-opaque instead
 */

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "scene/ve_entity.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {
class VeDevice;
class VeBuffer;
class PbrMegaBuffer;
class GpuSceneManager;
class Registry;

// What a ray query needs to read from the surface it hit.
// Mirrors RcwInstanceGeometry in rc_world_common.slangh.
struct RtInstanceGeometry {
	uint32_t vertex_offset;
	uint32_t first_index;
	uint32_t material_index;
	uint32_t pad = 0; // keeps the 16 B stride
	bool operator==(const RtInstanceGeometry&) const = default;
};
static_assert(sizeof(RtInstanceGeometry) == 16);

constexpr uint32_t RT_INSTANCE_MASK_SURFACE = 0x01;
constexpr uint32_t RT_INSTANCE_MASK_EMITTER = 0x02;

class VENGINE_API RayTracingScene {
public:
	explicit RayTracingScene(VeDevice& device);
	~RayTracingScene();

	RayTracingScene(const RayTracingScene&) = delete;
	RayTracingScene& operator=(const RayTracingScene&) = delete;

	// Brings the structures up to date with the scene, rebuilding only what
	// changed: the BLAS set when the mega buffer was rebuilt or the geometry
	// set did, the instance set when an instance appeared, went, or was hidden
	// or rewritten (the GpuSceneManager generations). Instances that only
	// moved (GpuSceneManager::takeMovedTraceable) get their transforms patched
	// and the TLAS refitted. Records the builds into cmd but not the barrier
	// behind them: whatever queries the TLAS waits on
	// eAccelerationStructureBuildKHR / eAccelerationStructureWriteKHR itself,
	// leaving the builds free to overlap the work recorded in between. The
	// barriers ahead of the TLAS build wait only on earlier builds (see
	// buildTlas). False once an allocation has failed, the empty TLAS build at
	// construction included: everything is retired and the scene stays empty.
	bool update(vk::raii::CommandBuffer& cmd, uint32_t frame_index, const PbrMegaBuffer& mega_buffer,
		GpuSceneManager& gpu_scene, const Registry& registry);

	// Frees every structure and buffer at once, all but the empty TLAS.
	// Device idle: frames in flight may still be querying them.
	void reset();
	// Anything allocated, live or retired: whether reset() has work to do.
	bool holdsResources() const;

	bool isReady() const { return m_tlas[m_tlas_latest].handle != nullptr && m_instance_count > 0; }
	vk::AccelerationStructureKHR getTlas() const {
		const Structure& tlas = m_tlas[m_tlas_latest];
		return tlas.handle ? **tlas.handle : vk::AccelerationStructureKHR{};
	}
	// untouched by reset(), Null only when its build failed.
	vk::AccelerationStructureKHR getEmptyTlas() const {
		return m_empty_tlas.handle ? **m_empty_tlas.handle : vk::AccelerationStructureKHR{};
	}
	uint32_t getBlasCount() const { return static_cast<uint32_t>(m_blas.size()); }
	uint32_t getInstanceCount() const { return m_instance_count; }
	vk::DeviceSize getStoreBytes() const { return m_store_bytes; }
	// Per-instance surface lookup for the shading side of a hit. Null until the
	// first TLAS build; its elements line up with the TLAS instance array.
	// Written on the host every build, so it is per frame in flight.
	const VeBuffer* getGeometryBuffer(uint32_t frame_index) const {
		return m_geometry_buffers[frame_index].get();
	}
	// Advances whenever the per-frame instance and geometry buffers are
	// reallocated; what a descriptor cache keys on to avoid handling freed bufffers.
	uint64_t getGeometryBufferGeneration() const { return m_buffer_generation; }

private:
	// One built structure and the buffer its storage lives in. The scratch a
	// build needed is not kept: it is only live for the build itself.
	struct Structure {
		std::unique_ptr<VeBuffer> storage;
		std::unique_ptr<vk::raii::AccelerationStructureKHR> handle;
		vk::DeviceAddress address = 0;
	};
	struct GeometryKey {
		uint32_t vertex_offset;
		uint32_t first_index;
		uint32_t index_count;
		bool operator==(const GeometryKey&) const = default;
	};
	struct GeometryKeyHash {
		size_t operator()(const GeometryKey& k) const;
	};

	void buildBlas(vk::raii::CommandBuffer& cmd, const PbrMegaBuffer& mega_buffer,
		const GpuSceneManager& gpu_scene);
	// Rebuilds the instance array and side table from every registered object.
	void gatherInstances(const GpuSceneManager& gpu_scene, const Registry& registry);
	// Rewrites the transforms of the instances in m_moved; false when none of
	// them is in the TLAS.
	bool patchMovedInstances(const Registry& registry);
	// Brings this frame's instance buffer up to m_instances.
	void uploadInstances(uint32_t frame_index);
	// Builds the TLAS over m_instances, or refits the latest one when allowed.
	void buildTlas(vk::raii::CommandBuffer& cmd, uint32_t frame_index, bool allow_refit);
	// Allocates the storage buffer and creates the structure object in it.
	Structure createStructure(vk::DeviceSize size, vk::AccelerationStructureTypeKHR type, const char* name);
	void buildEmptyTlas();
	// Brings one frame's copy of the geometry table up to date; no-op when it
	// already holds the current generation. Only the current frame's copy is
	// written, so a build never writes over a frame still in flight.
	void uploadGeometryTable(uint32_t frame_index);
	// Hands every live structure and buffer to the retired lists and leaves
	// the scene empty, to be rebuilt from scratch.
	void retireAll();

	VeDevice& m_ve_device;
	vk::DeviceSize m_scratch_alignment;

	std::vector<Structure> m_blas;
	// GeometryKey -> index into m_blas.
	std::unordered_map<GeometryKey, uint32_t, GeometryKeyHash> m_geometry_to_blas;

	// Built round robin, each with its own scratch, so a build never writes what a frame in flight traces
	std::array<Structure, MAX_FRAMES_IN_FLIGHT> m_tlas;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_tlas_scratch;
	uint32_t m_tlas_latest = 0; // slot of the last build, what getTlas() names
	uint32_t m_tlas_refits = 0; // refits since the last full build
	vk::AccelerationStructureBuildSizesInfoKHR m_tlas_sizes{}; // for m_instance_count instances
	Structure m_empty_tlas;
	// Host copy of the instance array, in TLAS order; movement patches it in place
	std::vector<vk::AccelerationStructureInstanceKHR> m_instances;
	std::vector<Entity> m_instance_entities; // per instance, where a patch reads its transform
	std::vector<uint32_t> m_instance_of_gpu_id; // gpu_id -> instance, NO_INSTANCE when not traced
	std::vector<uint32_t> m_moved; // GpuSceneManager::takeMovedTraceable output
	// Host-written every build; one per frame in flight, as only this slot's last submission is fenced
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_instance_buffers;
	// Per frame in flight: instances patched since its buffer was written; empty while it is stale
	std::array<std::vector<uint32_t>, MAX_FRAMES_IN_FLIGHT> m_instance_patches;
	std::array<bool, MAX_FRAMES_IN_FLIGHT> m_instances_stale{}; // its next upload writes the whole array
	// Host copy of the per-instance side table, in instance order (see uploadGeometryTable)
	std::vector<RtInstanceGeometry> m_geometry_table;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_geometry_buffers;
	std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_geometry_uploaded{};
	uint64_t m_geometry_generation = 0;
	uint64_t m_buffer_generation = 0;
	// Replaced by a rebuild while frames in flight may still read them; update() frees them later
	std::vector<std::pair<uint32_t, Structure>> m_retired;
	std::vector<std::pair<uint32_t, std::unique_ptr<VeBuffer>>> m_retired_buffers;
	uint32_t m_frame_counter = 0;
	// Bounds the BLAS set was built against, so buildTlas rejects exactly what buildBlas did
	uint32_t m_vertex_capacity = 0;
	uint32_t m_index_capacity = 0;
	uint32_t m_instance_count = 0;
	uint32_t m_instance_capacity = 0;

	uint64_t m_built_generation = 0;
	uint64_t m_built_geometry_generation = 0;
	uint64_t m_built_instance_generation = 0;
	vk::DeviceSize m_store_bytes = 0;
	bool m_failed = false; // an allocation failed; never cleared, not even by reset()
};

} // namespace ve
