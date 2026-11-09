/*
 * RcSystem: Split Radiance Cascades GI (Split RC), after Freeman &
 * Sannikov, "Split Radiance Cascades: Real-Time Global Illumination via
 * Sparse Radiance Probes" (https://arxiv.org/abs/2607.20384).
 *
 * Sparse world-anchored probes in per-cascade hashmaps, rebuilt every frame
 * from the depth buffer: cascade-0 insertion from surface pixels, parent
 * closure upward, TTL keepalive over the previous map, cross-lod seeding of
 * fresh probes. Ray generation then hands every
 * probe a contiguous run of the R2 sequence, and each spawn pixel traces
 * full-length rays from its surface, split into per-cascade interval deposits.
 *
 * If the device supports ray query and
 * RayTracingScene has a TLAS, rays query it and a hit is shaded from its
 * material. else
 * they march the ScreenRayResources min/max pyramid, and a hit takes its
 * radiance from the previous frame's HDR history.
 *
 * Deposits resolve into the persistent store, merge top-down, and are gathered
 * into per-frame outputs at render extent / resolution_div: diffuse
 * irradiance, sky visibility and rough-spec radiance, each with an alpha that
 * is 1 where the gather found probes and 0 where not (0 everywhere under
 * RcSkyOcclusion OFF, where the outputs carry bounce only). The PBR composite
 * blends its environment terms toward them by that alpha.
 *
 * Owns the hashmaps, the deposit scratch and persistent radiance pools, the
 * merge outputs, the per-probe irradiance and sky visibility tile
 * atlases, the rcw_* pipelines, the gathered output images, the scene
 * bindings a shaded hit reads, and a readback of the probe, overflow and ray
 * exit counters.
 */

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/rc_params.hpp"
#include "rendering/rc_world_mirrors.hpp"
#include "rendering/ve_frame_info.hpp"
#include "vulkan/ve_descriptors.hpp"

#include <glm/glm.hpp>
#include <memory>
#include <array>
#include <filesystem>

namespace ve {
class VeDevice;
class VeBuffer;
class VeImage;
class VeComputePipeline;
class EventBus;
class ScreenRayResources;
}

namespace ve {

class VENGINE_API RcSystem {
public:
	static constexpr uint32_t MAX_CASCADES = rcw::MAX_CASCADES;

	RcSystem(
		VeDevice& device,
		VeDescriptorPool& descriptor_pool,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& bindless_set_layout,
		const vk::raii::DescriptorSet& bindless_set,
		std::filesystem::path shader_path,
		vk::Extent2D render_extent,
		const vk::raii::ImageView& depth_image_view,
		const vk::raii::ImageView& normal_roughness_image_view,
		const ScreenRayResources& screen_rays,
		EventBus& event_bus);
	~RcSystem();

	RcSystem(const RcSystem&) = delete;
	RcSystem& operator=(const RcSystem&) = delete;

	// Records the full per-frame chain (map rebuild + keepalive + seed, ray
	// generation, trace/deposit with the tracer usingRayQuery() picks, resolve,
	// merge, irradiance tiles, gather), the counter readback copy and,
	// when history_copy on a march frame, the prev-depth copy. Requires
	// resolved depth in eDepthStencilReadOnlyOptimal, normals in
	// eShaderReadOnlyOptimal and, for the march, a valid history and this
	// frame's pyramid. Outputs are left in eGeneral for acquireForRead. May
	// record on the graphics or dedicated compute queue. Only while allocated.
	void dispatch(VeFrameInfo& frame_info, vk::raii::CommandBuffer& cmd, bool history_copy);

	// Call on every frame dispatch is skipped: the prev depth and counter
	// readbacks from before the gap no longer describe the next frame. The
	// store and the previous map's anchor stay valid.
	void markInactive();

	// Output image eGeneral -> eShaderReadOnlyOptimal for the PBR fragment
	// stage. Record on the graphics command buffer before scene render.
	void acquireForRead(vk::raii::CommandBuffer& cmd, uint32_t frame_index);

	// The outputs, prev depth, probe store, compute pipelines and descriptor
	// sets exist only while allocated; the dummies, sampler and layouts always
	// do. Both transitions need the caller to have waited for device idle.
	// allocate() while allocated recreates the extent-dependent parts and keeps
	// the probe store unless a store parameter changed.
	void allocate();
	void release();
	bool isAllocated() const { return m_allocated; }

	// Every frame, before dispatch.
	void setParams(const RcSettings& rc);

	// Drops the probe store and the prev-depth pairing, so the next dispatch
	// starts cold.
	void invalidateStore();

	// Recorded now, applied by the next allocate(); true when the request changed
	bool setQuality(uint32_t resolution_div, uint32_t cascades, uint32_t memory_mb);

	// Live outputs for the set-5 writer; only while allocated.
	const vk::raii::ImageView& getOutputImageView(uint32_t frame_index) const;
	const vk::raii::ImageView& getDummyImageView() const;
	const vk::raii::ImageView& getVisibilityImageView(uint32_t frame_index) const;
	const vk::raii::ImageView& getVisibilityDummyImageView() const;
	// Rough-spec target (set 5, binding 5). Its dummy is the RC dummy: same
	// format, same black/confidence-0 identity.
	const vk::raii::ImageView& getSpecImageView(uint32_t frame_index) const;
	const vk::raii::ImageView& getGeometryImageView(uint32_t frame_index) const;
	const vk::raii::Sampler& getOutputSampler() const { return m_linear_clamp_sampler; }

	// Probe-store counters (FrameStats / bench counter metrics). GPU readback
	// with a MAX_FRAMES_IN_FLIGHT-frame lag; every readout below is 0 until
	// the first readback lands and after every reset.
	uint32_t probeCount() const;
	uint32_t overflowCount() const;
	// Issued probes in cascade n, husks included; 0 beyond the active count.
	uint32_t cascadeProbeCount(uint32_t n) const;
	// Rays of the last traced frame that exited with rcw::RAY_* code
	uint32_t rayExitCount(uint32_t code) const;

	// The structure from RayTracingScene: its TLAS when
	// traceable, else its permanent empty one.
	void setTlas(vk::AccelerationStructureKHR tlas, bool traceable) {
		m_tlas = tlas;
		m_tlas_traceable = traceable;
	}

	// What a hit needs to become a shaded surface; read only by the ray query
	// trace. Every member may be null, in which case a dummy stands in and the
	// shaded path finds nothing there; set before dispatch, every frame.
	struct SceneBindings {
		const VeBuffer* instance_geometry = nullptr; // RayTracingScene, TLAS instance order
		const VeBuffer* mega_vertices = nullptr;
		const VeBuffer* mega_indices = nullptr;
		const VeBuffer* lights = nullptr; // ClusterLightSystem::getLightList(), this frame's slot, read in place
		uint32_t light_count = 0;         // point + spot lights at the head of lights
		uint64_t mega_generation = 0;     // PbrMegaBuffer::generation(), the descriptor-write key
		uint64_t geometry_generation = 0; // RayTracingScene::getGeometryBufferGeneration(), likewise
		uint64_t light_generation = 0;    // ClusterLightSystem::getLightListGeneration(), likewise
	};
	void setSceneBindings(const SceneBindings& bindings) { m_scene = bindings; }
	bool usingRayQuery() const { return m_tlas_traceable && m_rc.ray_query && m_trace_rayquery_pipeline != nullptr; }

	// Sizing readout for the Debug panel
	vk::DeviceSize storeBytes() const { return m_store_bytes; }
	bool storeClampedByDevice() const { return m_device_clamped; }
	uint32_t activeCascades() const { return m_cascades; }
	uint32_t cascadeCap(uint32_t n) const { return m_descs[n].probe_cap; }

private:
	// Mirrors RcwCascadeDesc in rc_world_common.slangh.
	struct CascadeDesc {
		uint32_t hash_base;
		uint32_t hash_cap;
		uint32_t probe_base;
		uint32_t probe_cap;
		uint32_t slot_base;
		uint32_t free_base;
		uint32_t pair_base;
		uint32_t dirmip_base;
	};
	static_assert(sizeof(CascadeDesc) == 8 * sizeof(uint32_t));

	void createOutputImages();
	void createDummyImage();
	void createSampler();
	void createPrevDepthImage();
	void computeLayout();
	// Lays the store out for a c0 capacity and returns its size in bytes
	vk::DeviceSize layOutStore(uint32_t cap0);
	void createBuffers();
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout,
		const vk::raii::DescriptorSetLayout& bindless_set_layout);
	void createComputePipelines();
	void createDescriptorSets();
	void writeSceneBindings(uint32_t frame_index);
	void releaseStore();
	void clearStoreState();
	void recordReset(vk::raii::CommandBuffer& cmd);
	void requestReset() { m_reset_pending = true; }

	VeDevice& m_ve_device;
	std::filesystem::path m_shader_path;
	vk::Extent2D m_extent{};        // RC output resolution (render extent / div)
	vk::Extent2D m_depth_extent{};  // depth buffer resolution
	VeDescriptorPool* m_descriptor_pool = nullptr;
	bool m_allocated = false;
	bool m_store_stale = true; // cascades or budget changed, or released

	RcSettings m_rc{};

	uint32_t m_resolution_div = 4;
	uint32_t m_cascades = 4;
	uint32_t m_memory_mb = 578;
	struct Quality {
		uint32_t resolution_div = 4;
		uint32_t cascades = 4;
		uint32_t memory_mb = 578;
		bool operator==(const Quality&) const = default;
	} m_pending;

	const ScreenRayResources& m_screen_rays;
	const vk::raii::DescriptorSet& m_bindless_set; // set 2, sampled by the shade path

	// Per-frame irradiance + confidence; the fragment stage reads one while the next frame rebuilds another
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_output_images;
	// Sky visibility, same lifecycle as m_output_images in every respect.
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_visibility_images;
	// Rough-spec radiance + confidence, same lifecycle
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_spec_images;
	// Per-texel surface (normal, depth) for the edge-aware upsample
	std::array<std::unique_ptr<VeImage>, MAX_FRAMES_IN_FLIGHT> m_geometry_images;
	// 1x1 black (confidence 0): the composite lerp is an identity when off.
	std::unique_ptr<VeImage> m_dummy_image;
	std::unique_ptr<VeImage> m_visibility_dummy_image;
	// Last march frame's min/max depth (pyramid mip 0); validates next frame's history reprojection
	std::unique_ptr<VeImage> m_prev_depth;
	bool m_prev_depth_primed = false;

	vk::raii::Sampler m_linear_clamp_sampler{nullptr};

	// Cached single-sample views for the descriptor writes
	vk::ImageView m_depth_image_view{};
	vk::ImageView m_normal_image_view{};

	// Probe-store layout (computeLayout), sized from m_cascades + m_memory_mb.
	std::array<CascadeDesc, MAX_CASCADES> m_descs{};
	vk::DeviceSize m_store_bytes = 0;
	vk::DeviceSize m_max_buffer_bytes = 0;
	uint32_t m_max_image_dimension = 0;
	bool m_device_clamped = false;         // capacity cut below the budget by device limits
	uint32_t m_hash_words_per_parity = 0;     // words per frame in hash_keys/hash_vals
	uint32_t m_probes_per_parity = 0;    // elements per frame in probes
	uint32_t m_slot_total = 0;
	uint32_t m_free_total = 0;
	uint32_t m_pair_total = 0;      // (probe, dir) pairs across cascades
	uint32_t m_merged_cap = 0;      // cascade 0 pairs
	uint32_t m_dirmip_cap = 0;      // every cascade above 0's pairs / 4, one region each

	std::unique_ptr<VeBuffer> m_hash_keys;
	std::unique_ptr<VeBuffer> m_hash_vals;
	std::unique_ptr<VeBuffer> m_probes;
	std::unique_ptr<VeBuffer> m_counters;
	std::unique_ptr<VeBuffer> m_slot_age;
	std::unique_ptr<VeBuffer> m_free_lists;
	std::unique_ptr<VeBuffer> m_cascade_descs;
	std::unique_ptr<VeBuffer> m_ray_cursor;

	std::unique_ptr<VeBuffer> m_scratch_radiance;
	std::unique_ptr<VeBuffer> m_scratch_mass;
	std::unique_ptr<VeBuffer> m_persist_j;
	std::unique_ptr<VeBuffer> m_persist_count;
	std::unique_ptr<VeBuffer> m_merged;
	std::unique_ptr<VeBuffer> m_dirmip;
	std::unique_ptr<VeBuffer> m_merged_lower;
	std::unique_ptr<VeBuffer> m_dirmip_lower;
	std::unique_ptr<VeBuffer> m_probe_sky_fill;
	std::unique_ptr<VeImage> m_irradiance_atlas;
	std::unique_ptr<VeImage> m_spec_atlas;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_readback;

	// Built once in the ctor; only the descriptor sets follow the extent.
	std::unique_ptr<VeDescriptorSetLayout> m_set_layout;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VeComputePipeline> m_insert_c0_pipeline;
	std::unique_ptr<VeComputePipeline> m_insert_parent_pipeline;
	std::unique_ptr<VeComputePipeline> m_keepalive_pipeline;
	std::unique_ptr<VeComputePipeline> m_seed_pipeline;
	std::unique_ptr<VeComputePipeline> m_raygen_count_c0_pipeline;
	std::unique_ptr<VeComputePipeline> m_raygen_count_up_pipeline;
	std::unique_ptr<VeComputePipeline> m_raygen_offset_top_pipeline;
	std::unique_ptr<VeComputePipeline> m_raygen_offset_down_pipeline;
	std::unique_ptr<VeComputePipeline> m_trace_pipeline;
	// Only where the device has ray query: its module needs the RayQueryKHR capability
	std::unique_ptr<VeComputePipeline> m_trace_rayquery_pipeline;
	std::unique_ptr<VeComputePipeline> m_resolve_pipeline;
	std::unique_ptr<VeComputePipeline> m_merge_pipeline;
	std::unique_ptr<VeComputePipeline> m_irradiance_pipeline;
	std::unique_ptr<VeComputePipeline> m_gather_pipeline;
	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptor_sets =
		makeNullArray<vk::raii::DescriptorSet>();

	vk::AccelerationStructureKHR m_tlas{};
	bool m_tlas_traceable = false;
	SceneBindings m_scene{};
	// What each frame set's scene bindings last pointed at (see writeSceneBindings)
	struct SceneWritten {
		uint64_t mega_generation = 0;     // 0 = dummy bound
		uint64_t geometry_generation = 0; // 0 = dummy bound
		uint64_t light_generation = 0;    // 0 = dummy bound
		bool operator==(const SceneWritten&) const = default;
	};
	// nullopt = bindings 32-35 unwritten in that frame's set
	std::array<std::optional<SceneWritten>, MAX_FRAMES_IN_FLIGHT> m_scene_written{};
	// Stands in for any scene buffer that is absent, so no binding goes unwritten; only with ray query.
	std::unique_ptr<VeBuffer> m_dummy_buffer;

	// Advances once per dispatch; seeds the ray jitter and lod dither, and its low bit is the map parity.
	uint32_t m_frame_counter = 0;
	// Which tracer the last dispatch ran; a switch resets the store
	bool m_last_ray_query = false;

	glm::ivec3 m_last_anchor0{0};
	bool m_has_prev = false;
	bool m_reset_pending = true;
	bool m_lod0_clamp_warned = false;

	std::array<bool, MAX_FRAMES_IN_FLIGHT> m_has_readback{};
	bool m_counters_valid = false;
	bool m_overflow_warned = false;
	uint32_t m_probe_count = 0;
	uint32_t m_overflow_count = 0;
	std::array<uint32_t, MAX_CASCADES> m_cascade_probes{};
	std::array<uint32_t, rcw::RAY_EXIT_CODES> m_ray_exits{};
};

} // namespace ve
