#include "pch.hpp"
#include "rendering/rc_system.hpp"
#include "rendering/screen_ray_resources.hpp"
#include "rendering/frame_profiler.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_compute_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_path.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

#include <bit>
#include <cmath>
#include <cstddef>

namespace ve {

using namespace rcw;

namespace {
// Mirrors RcwPush in rc_world_common.slangh
struct RcwPushConstant {
	glm::ivec4 anchor0_curr; // xyz: floor(camera / ds0); w: frame parity
	glm::ivec4 anchor0_prev; // xyz: anchor of the prev map's dispatch; w: probe TTL
	glm::vec2 out_size;
	glm::vec2 depth_size;
	float ds0;
	float lod0_dist;
	float proj_22;
	float proj_32;
	float luma_clamp;
	uint32_t hash_words_per_parity;
	uint32_t probes_per_parity;
	uint32_t cascade_index;
	uint32_t n_cascades;
	uint32_t debug_gather_mode;
	uint32_t probe_j_cascade;
	uint32_t frame_counter;
	uint32_t rays_per_pixel;
	uint32_t flags;
	uint32_t hiz_mip_count;
	uint32_t light_count;
	uint32_t history_length;
};
static_assert(sizeof(RcwPushConstant) == 116, "must match RcwPush in rc_world_common.slangh");

// Tile-atlas extent from capacity; mirrors rcwTilesPerRow. See shader comments.
vk::Extent2D atlasExtent(uint32_t capacity) {
	uint32_t tiles_per_row = 1u << ((std::bit_width(capacity) - 1 + 1) >> 1);
	uint32_t rows = (capacity + tiles_per_row - 1) / tiles_per_row;
	return vk::Extent2D{tiles_per_row * TILE, rows * TILE};
}
constexpr vk::DeviceSize WORD = sizeof(uint32_t);
// Per-cascade capacity as a share of cascade 0's
constexpr double CASCADE_SHAPE[MAX_CASCADES] = {1.0, 0.2769, 0.0731, 0.0197, 0.0053, 0.0014};
// Scratch radiance + mass, persistent J + count
constexpr uint32_t BYTES_PER_PAIR = (SCRATCH_RADIANCE_WORDS + 2) * WORD + 2 * WORD + WORD;

// Directions per probe in cascade n: 2 * THETA0^2 * 4^n (32, 128, 512, ...).
// Mirrors rcwDirCount.
constexpr uint32_t dirCount(uint32_t cascade) {
	return (2u * THETA0 * THETA0) << (2 * cascade);
}

// The gather view an RC view selects; 0 for the views that display an output as it is
uint32_t gatherMode(RcDebugView view) {
	switch (view) {
		case RcDebugView::PROBE_J:
		case RcDebugView::TRACE_KIND:
		case RcDebugView::CONVERGENCE:
		case RcDebugView::PROBE_GRID:
		case RcDebugView::SPEC:
			return static_cast<uint32_t>(view);
		default:
			return 0;
	}
}

// The push.flags sky-mode bits for the tracer that runs
uint32_t skyModeBits(RcSkyOcclusion mode, bool ray_query) {
	switch (mode) {
		case RcSkyOcclusion::AUTO:
			return (ray_query ? SKY_MODE_PROBE : SKY_MODE_OFF) << SKY_MODE_SHIFT;
		case RcSkyOcclusion::OFF:
			return SKY_MODE_OFF << SKY_MODE_SHIFT;
		case RcSkyOcclusion::UNKNOWN_SKY:
			return SKY_MODE_UNKNOWN_SKY << SKY_MODE_SHIFT;
		case RcSkyOcclusion::UNKNOWN_OBSTRUCTED:
			return SKY_MODE_UNKNOWN_OBSTRUCTED << SKY_MODE_SHIFT;
		default:
			return SKY_MODE_PROBE << SKY_MODE_SHIFT;
	}
}

} // namespace

RcSystem::RcSystem(
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
	EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_depth_extent(render_extent), m_descriptor_pool(&descriptor_pool), m_screen_rays(screen_rays),
	  m_bindless_set(bindless_set) {

	// RenderPipeline recreates the shared history and pyramid before it
	// broadcasts, so the views fetched during allocate() are fresh. Released,
	// only the inputs are cached.
	event_bus.subscribe<ResolutionChangedEvent>([this](const ResolutionChangedEvent& e) {
		m_descriptor_pool = &e.pool;
		m_depth_extent = e.extent;
		m_depth_image_view = *e.depth_image_view;
		m_normal_image_view = *e.normal_roughness_image_view;
		if (m_allocated)
			allocate();
	});

	auto limits = m_ve_device.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2,
		vk::PhysicalDeviceMaintenance3Properties, vk::PhysicalDeviceMaintenance4Properties>();
	m_max_buffer_bytes = std::min({
		static_cast<vk::DeviceSize>(limits.get<vk::PhysicalDeviceProperties2>().properties.limits.maxStorageBufferRange),
		limits.get<vk::PhysicalDeviceMaintenance3Properties>().maxMemoryAllocationSize,
		limits.get<vk::PhysicalDeviceMaintenance4Properties>().maxBufferSize});
	m_max_image_dimension = limits.get<vk::PhysicalDeviceProperties2>().properties.limits.maxImageDimension2D;

	m_depth_image_view = *depth_image_view;
	m_normal_image_view = *normal_roughness_image_view;
	createDummyImage();
	createSampler();
	// Outside the store: it backs scene bindings that have to hold something
	// on every ray query frame, scene or no scene.
	if (m_ve_device.supportsRayQuery()) {
		m_dummy_buffer = std::make_unique<VeBuffer>(m_ve_device, sizeof(uint32_t), 1,
			vk::BufferUsageFlagBits::eStorageBuffer, vk::MemoryPropertyFlagBits::eDeviceLocal);
		m_dummy_buffer->setDebugName("RCW Dummy Storage");
	}
	createPipelineLayout(global_set_layout, bindless_set_layout);
	computeLayout();
}

RcSystem::~RcSystem() = default;

void RcSystem::invalidateStore() {
	requestReset();
	// A dropped/aborted frame voids the "prev depth == the frame the history
	// shows" pairing; run the next trace frame unvalidated. The other callers
	// lose nothing by it: the store they start is empty.
	m_prev_depth_primed = false;
}

void RcSystem::setParams(const RcSettings& rc) {
	// Structural params re-key every probe
	bool structural = rc.ds0 != m_rc.ds0 || rc.lod0_dist != m_rc.lod0_dist;
	m_rc = rc;
	if (structural)
		requestReset();
}

bool RcSystem::setQuality(uint32_t resolution_div, uint32_t cascades, uint32_t memory_mb) {
	Quality request{
		.resolution_div = std::clamp(resolution_div, 1u, 8u),
		.cascades = static_cast<uint32_t>(std::clamp(static_cast<int>(cascades), 1,
			static_cast<int>(MAX_CASCADES))),
		.memory_mb = static_cast<uint32_t>(std::clamp(static_cast<int>(memory_mb), 16, 4096)),
	};
	if (request == m_pending)
		return false;
	m_pending = request;
	return true;
}

void RcSystem::createOutputImages() {
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_output_images[i] = std::make_unique<VeImage>(
			m_ve_device,
			m_extent.width,
			m_extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR16G16B16A16Sfloat,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		// Rests in eShaderReadOnlyOptimal (read by fragment between frames)
		m_output_images[i]->transitionImageLayout(
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eFragmentShader);
		m_output_images[i]->setDebugName(("RC Output [" + std::to_string(i) + "]").c_str());

		m_visibility_images[i] = std::make_unique<VeImage>(
			m_ve_device,
			m_extent.width,
			m_extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR16Sfloat,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		m_visibility_images[i]->transitionImageLayout(
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eFragmentShader);
		m_visibility_images[i]->setDebugName(("RC Sky Visibility [" + std::to_string(i) + "]").c_str());

		m_spec_images[i] = std::make_unique<VeImage>(
			m_ve_device,
			m_extent.width,
			m_extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR16G16B16A16Sfloat,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		m_spec_images[i]->transitionImageLayout(
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eFragmentShader);
		m_spec_images[i]->setDebugName(("RC Spec [" + std::to_string(i) + "]").c_str());

		m_geometry_images[i] = std::make_unique<VeImage>(
			m_ve_device,
			m_extent.width,
			m_extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR16G16B16A16Sfloat,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		m_geometry_images[i]->transitionImageLayout(
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eFragmentShader);
		m_geometry_images[i]->setDebugName(("RC Geometry [" + std::to_string(i) + "]").c_str());
	}
}

void RcSystem::createDummyImage() {
	m_dummy_image = std::make_unique<VeImage>(
		m_ve_device, 1, 1,
		vk::SampleCountFlagBits::e1, vk::Format::eR16G16B16A16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1, 1);

	constexpr uint32_t bytes = 8; // one RGBA16F texel
	std::array<uint8_t, bytes> black{};
	VeBuffer staging(m_ve_device, bytes, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	staging.map();
	staging.writeToBuffer(black.data(), bytes);

	auto cmd = m_ve_device.beginSingleTimeCommands(QueueKind::Graphics);
	m_dummy_image->transitionImageLayout(*cmd,
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);
	VeDevice::copyBufferToImage(*cmd, staging.getBuffer(), m_dummy_image->getImage(), 1, 1, 1);
	m_dummy_image->transitionImageLayout(*cmd,
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);
	m_ve_device.endSingleTimeCommands(*cmd, QueueKind::Graphics);
	m_dummy_image->setDebugName("RC Dummy");

	m_visibility_dummy_image = std::make_unique<VeImage>(
		m_ve_device, 1, 1,
		vk::SampleCountFlagBits::e1, vk::Format::eR16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1, 1);
	// The staging path copies raw bytes, so this is half(1.0) written directly.
	constexpr uint16_t HALF_ONE = 0x3C00;
	VeBuffer visibility_staging(m_ve_device, sizeof(HALF_ONE), 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	visibility_staging.map();
	visibility_staging.writeToBuffer(&HALF_ONE, sizeof(HALF_ONE));

	auto visibility_cmd = m_ve_device.beginSingleTimeCommands(QueueKind::Graphics);
	m_visibility_dummy_image->transitionImageLayout(*visibility_cmd,
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);
	VeDevice::copyBufferToImage(*visibility_cmd, visibility_staging.getBuffer(), m_visibility_dummy_image->getImage(), 1, 1, 1);
	m_visibility_dummy_image->transitionImageLayout(*visibility_cmd,
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);
	m_ve_device.endSingleTimeCommands(*visibility_cmd, QueueKind::Graphics);
	m_visibility_dummy_image->setDebugName("RC Sky Visibility Dummy");
}

void RcSystem::createSampler() {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = vk::LodClampNone,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,
		.unnormalizedCoordinates = VK_FALSE,
	};
	m_linear_clamp_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void RcSystem::createPrevDepthImage() {
	// Prev-frame min/max depth at the pyramid's mip-0 extent.
	vk::Extent2D pyr = m_screen_rays.pyramidMip0Extent();
	m_prev_depth = std::make_unique<VeImage>(m_ve_device,
		pyr.width, pyr.height,
		vk::SampleCountFlagBits::e1,
		vk::Format::eR32G32Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_prev_depth->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eComputeShader);
	m_prev_depth->setDebugName("RC Prev Depth");
	m_prev_depth_primed = false;
}

void RcSystem::computeLayout() {
	// The budget covers the whole store: the largest c0 capacity whose store
	// fits. 65,535 * LINEAR_WG keeps the per-probe indirect (ceil(probe_cap /
	// LINEAR_WG) groups) within limits.
	const vk::DeviceSize budget = static_cast<vk::DeviceSize>(m_memory_mb) << 20;
	uint32_t cap0 = 1024;
	uint32_t cap0_max = 65535 * LINEAR_WG;
	while (cap0 < cap0_max) {
		uint32_t mid = cap0 + (cap0_max - cap0 + 1) / 2;
		if (layOutStore(mid) <= budget)
			cap0 = mid;
		else
			cap0_max = mid - 1;
	}
	// Device limits: the radiance scratch, the largest buffer at
	// SCRATCH_RADIANCE_WORDS words per pair, must fit one allocation and one
	// whole storage binding, and the c0 tile atlases one image.
	double pairs_per_cap0 = 0.0;
	for (uint32_t n = 0; n < m_cascades; n++)
		pairs_per_cap0 += CASCADE_SHAPE[n] * dirCount(n);
	double buffer_cap0 = static_cast<double>(m_max_buffer_bytes)
		/ (SCRATCH_RADIANCE_WORDS * WORD * pairs_per_cap0);
	uint32_t tiles_per_side = m_max_image_dimension / TILE;
	uint32_t tiles_per_row = std::bit_floor(tiles_per_side);
	double atlas_cap0 = std::min(2.0 * tiles_per_row * tiles_per_row - 1.0,
		static_cast<double>(tiles_per_row) * tiles_per_side);
	double device_cap0 = std::min(buffer_cap0, atlas_cap0);
	m_device_clamped = cap0 > device_cap0;
	if (m_device_clamped)
		cap0 = static_cast<uint32_t>(device_cap0);
	layOutStore(cap0);
	if (static_cast<vk::DeviceSize>(m_pair_total) * SCRATCH_RADIANCE_WORDS * WORD > m_max_buffer_bytes)
		VE_LOGE("RC store: the per-cascade probe floors exceed the device buffer limit; lower Cascades");
}

vk::DeviceSize RcSystem::layOutStore(uint32_t cap0) {
	uint32_t hash_off = 0;
	uint32_t probe_off = 0;
	uint32_t slot_off = 0;
	uint32_t free_off = 0;
	uint32_t pair_off = 0;
	uint32_t dirmip_off = 0;
	m_descs = {};
	for (uint32_t n = 0; n < m_cascades; n++) {
		uint32_t cap = std::max(256u, static_cast<uint32_t>(std::llround(cap0 * CASCADE_SHAPE[n])));
		m_descs[n] = CascadeDesc{
			.hash_base = hash_off,
			.hash_cap = std::bit_ceil(cap * 2),
			.probe_base = probe_off,
			.probe_cap = cap,
			.slot_base = slot_off,
			.free_base = free_off,
			.pair_base = pair_off,
			.dirmip_base = dirmip_off,
		};
		hash_off += m_descs[n].hash_cap;
		probe_off += cap;
		slot_off += cap;
		free_off += 2 + cap;
		pair_off += cap * dirCount(n);
		// Every cascade above 0 keeps its own dirmip region: the gather's
		// specular walk reads them all after the merge.
		if (n > 0)
			dirmip_off += cap * dirCount(n) / 4;
	}
	m_merged_cap = m_descs[0].probe_cap * dirCount(0);
	m_dirmip_cap = std::max(dirmip_off, 1u);
	m_hash_words_per_parity = hash_off;
	m_probes_per_parity = probe_off;
	m_slot_total = slot_off;
	m_free_total = free_off;
	m_pair_total = pair_off;

	vk::Extent2D atlas = atlasExtent(m_descs[0].probe_cap);
	m_store_bytes = static_cast<vk::DeviceSize>(m_pair_total) * BYTES_PER_PAIR
		+ static_cast<vk::DeviceSize>(m_merged_cap + m_dirmip_cap) * 2 * WORD
		+ static_cast<vk::DeviceSize>(m_merged_cap + m_dirmip_cap) * WORD
		+ static_cast<vk::DeviceSize>(m_hash_words_per_parity) * 2 * 2 * WORD
		+ static_cast<vk::DeviceSize>(m_probes_per_parity) * 2 * 2 * WORD
		+ static_cast<vk::DeviceSize>(m_slot_total + m_free_total) * WORD
		+ static_cast<vk::DeviceSize>(m_cascades) * 2 * sizeof(CountersGpu)
		+ static_cast<vk::DeviceSize>(m_descs[0].probe_cap) * WORD
		// RGBA16F irradiance + RGBA16F specular tile atlases.
		+ static_cast<vk::DeviceSize>(atlas.width) * atlas.height * (8 + 8);
	return m_store_bytes;
}

void RcSystem::releaseStore() {
	for (auto& slot : m_readback)
		slot.reset();
	m_spec_atlas.reset();
	m_irradiance_atlas.reset();
	m_ray_cursor.reset();
	m_dirmip_lower.reset();
	m_probe_sky_fill.reset();
	m_merged_lower.reset();
	m_dirmip.reset();
	m_merged.reset();
	m_persist_count.reset();
	m_persist_j.reset();
	m_scratch_mass.reset();
	m_scratch_radiance.reset();
	m_cascade_descs.reset();
	m_free_lists.reset();
	m_slot_age.reset();
	m_counters.reset();
	m_probes.reset();
	m_hash_vals.reset();
	m_hash_keys.reset();
}

void RcSystem::createBuffers() {
	// One store's worth of memory at a time: the previous store goes first.
	releaseStore();

	auto device_local = vk::MemoryPropertyFlagBits::eDeviceLocal;
	m_hash_keys = std::make_unique<VeBuffer>(m_ve_device, WORD, m_hash_words_per_parity * 2,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_hash_keys->setDebugName("RCW Hash Keys");
	m_hash_vals = std::make_unique<VeBuffer>(m_ve_device, WORD, m_hash_words_per_parity * 2,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_hash_vals->setDebugName("RCW Hash Vals");
	m_probes = std::make_unique<VeBuffer>(m_ve_device, 2 * WORD, m_probes_per_parity * 2,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_probes->setDebugName("RCW Entries");
	m_counters = std::make_unique<VeBuffer>(m_ve_device, sizeof(CountersGpu), m_cascades * 2,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst
			| vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eIndirectBuffer,
		device_local);
	m_counters->setDebugName("RCW Counters");
	m_slot_age = std::make_unique<VeBuffer>(m_ve_device, WORD, m_slot_total,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_slot_age->setDebugName("RCW Slot Age");
	m_free_lists = std::make_unique<VeBuffer>(m_ve_device, WORD, m_free_total,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_free_lists->setDebugName("RCW Free Lists");
	m_ray_cursor = std::make_unique<VeBuffer>(m_ve_device, WORD, m_probes_per_parity,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_ray_cursor->setDebugName("RCW Ray Cursor");
	m_scratch_radiance = std::make_unique<VeBuffer>(m_ve_device, WORD, m_pair_total * SCRATCH_RADIANCE_WORDS,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_scratch_radiance->setDebugName("RCW Deposit Radiance");
	m_scratch_mass = std::make_unique<VeBuffer>(m_ve_device, 2 * WORD, m_pair_total,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_scratch_mass->setDebugName("RCW Deposit Mass");
	m_persist_j = std::make_unique<VeBuffer>(m_ve_device, 2 * WORD, m_pair_total,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_persist_j->setDebugName("RCW Persistent J");
	m_persist_count = std::make_unique<VeBuffer>(m_ve_device, WORD, m_pair_total,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_persist_count->setDebugName("RCW Persistent Count");
	m_merged = std::make_unique<VeBuffer>(m_ve_device, 2 * WORD, m_merged_cap,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_merged->setDebugName("RCW Merged I");
	m_dirmip = std::make_unique<VeBuffer>(m_ve_device, 2 * WORD, m_dirmip_cap,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_dirmip->setDebugName("RCW Dir Mip");
	m_merged_lower = std::make_unique<VeBuffer>(m_ve_device, WORD, m_merged_cap,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_merged_lower->setDebugName("RCW Merged Lower");
	m_dirmip_lower = std::make_unique<VeBuffer>(m_ve_device, WORD, m_dirmip_cap,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_dirmip_lower->setDebugName("RCW Dir Mip Lower");
	m_probe_sky_fill = std::make_unique<VeBuffer>(m_ve_device, WORD, m_descs[0].probe_cap,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_probe_sky_fill->setDebugName("RCW Probe Sky Fill");

	// Irradiance tile atlas: one TILE^2 tile per c0 probe.
	vk::Extent2D atlas = atlasExtent(m_descs[0].probe_cap);
	m_irradiance_atlas = std::make_unique<VeImage>(
		m_ve_device, atlas.width, atlas.height,
		vk::SampleCountFlagBits::e1, vk::Format::eR16G16B16A16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	// Written and sampled within the dispatch chain each frame; rests in eGeneral.
	m_irradiance_atlas->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eComputeShader);
	m_irradiance_atlas->setDebugName("RCW Irradiance Atlas");

	// Specular radiance + sky visibility, same tile geometry.
	m_spec_atlas = std::make_unique<VeImage>(
		m_ve_device, atlas.width, atlas.height,
		vk::SampleCountFlagBits::e1, vk::Format::eR16G16B16A16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_spec_atlas->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eComputeShader);
	m_spec_atlas->setDebugName("RCW Specular Atlas");

	m_cascade_descs = std::make_unique<VeBuffer>(m_ve_device, sizeof(CascadeDesc), MAX_CASCADES,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	m_cascade_descs->map();
	m_cascade_descs->writeToBuffer(m_descs.data(), sizeof(CascadeDesc) * MAX_CASCADES);
	m_cascade_descs->setDebugName("RCW Cascade Descs");

	// Readback staging: one frame's cascade counter structs
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_readback[i] = std::make_unique<VeBuffer>(m_ve_device, sizeof(CountersGpu), m_cascades,
			vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			1, HostAccess::Random);
		m_readback[i]->map();
	}
}

void RcSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& bindless_set_layout) {
	auto layout_builder = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // depth
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // hash keys
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // hash vals
		.addBinding(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // probes
		.addBinding(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // counters
		.addBinding(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // slot age
		.addBinding(6, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // free lists
		.addBinding(7, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // cascade descs
		.addBinding(8, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // output
		.addBinding(9, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // normal+roughness
		.addBinding(10, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // history
		.addBinding(11, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eCompute)      // linear clamp
		.addBinding(12, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // min/max pyramid
		.addBinding(13, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // deposit radiance
		.addBinding(14, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // persistent J
		.addBinding(15, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // persistent count
		.addBinding(16, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // irradiance atlas RW
		.addBinding(17, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // irradiance atlas sampled
		.addBinding(18, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // merged I
		.addBinding(19, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // dir mip
		.addBinding(20, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // specular atlas RW
		.addBinding(21, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // specular atlas sampled
		.addBinding(22, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // gathered sky visibility
		.addBinding(23, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // prev min/max depth
		.addBinding(24, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // gathered spec
		.addBinding(25, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // deposit mass
		.addBinding(26, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // gathered surface
		.addBinding(27, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // merged lower sky fraction
		.addBinding(28, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // dir mip lower sky fraction
		.addBinding(29, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute)  // ray generation counts/cursors
		.addBinding(30, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute); // c0 probe sky fill
	// Bindings 31-35 exist only alongside the ray query trace variant that reads them.
	if (m_ve_device.supportsRayQuery())
		layout_builder
			.addBinding(31, vk::DescriptorType::eAccelerationStructureKHR, vk::ShaderStageFlagBits::eCompute) // scene TLAS
			.addBinding(32, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // per-instance geometry + material
			.addBinding(33, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // mega vertices
			.addBinding(34, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // mega indices
			.addBinding(35, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute); // punctual lights
	m_set_layout = layout_builder.build();

	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(RcwPushConstant),
	};
	// Bindless goes at set 2, not the 1 the graphics pipelines give it: set 1
	// is RC's own, and rc_world_common.slangh declares it to match.
	std::array<vk::DescriptorSetLayout, 3> set_layouts{
		*global_set_layout,
		*m_set_layout->getDescriptorSetLayout(),
		*bindless_set_layout,
	};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void RcSystem::createComputePipelines() {
	m_insert_c0_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_insert_c0_comp.spv", m_pipeline_layout);
	m_insert_parent_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_insert_parent_comp.spv", m_pipeline_layout);
	m_keepalive_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_keepalive_comp.spv", m_pipeline_layout);
	m_seed_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_seed_comp.spv", m_pipeline_layout);
	m_raygen_count_c0_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_raygen_count_c0_comp.spv", m_pipeline_layout);
	m_raygen_count_up_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_raygen_count_up_comp.spv", m_pipeline_layout);
	m_raygen_offset_top_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_raygen_offset_top_comp.spv", m_pipeline_layout);
	m_raygen_offset_down_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_raygen_offset_down_comp.spv", m_pipeline_layout);
	m_trace_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_trace_deposit_comp.spv", m_pipeline_layout);
	if (m_ve_device.supportsRayQuery())
		m_trace_rayquery_pipeline = std::make_unique<VeComputePipeline>(
			m_ve_device, m_shader_path / "rcw_trace_rayquery_comp.spv", m_pipeline_layout);
	m_resolve_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_resolve_comp.spv", m_pipeline_layout);
	m_merge_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_merge_comp.spv", m_pipeline_layout);
	m_irradiance_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_irradiance_comp.spv", m_pipeline_layout);
	m_gather_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_gather_comp.spv", m_pipeline_layout);
}

void RcSystem::createDescriptorSets() {
	// These sets are rebuilt on every resize and quality change, with bindings
	// 32-35 unwritten.
	m_scene_written = {};
	vk::DescriptorImageInfo depth_info{
		.imageView = m_depth_image_view,
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};
	vk::DescriptorImageInfo normal_info{
		.imageView = m_normal_image_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo history_info{
		.imageView = *m_screen_rays.historyView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo sampler_info{
		.sampler = *m_linear_clamp_sampler,
	};
	vk::DescriptorImageInfo hiz_info{
		.imageView = *m_screen_rays.pyramidView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo prev_depth_info{
		.imageView = *m_prev_depth->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	auto keys_info = m_hash_keys->getDescriptorInfo();
	auto vals_info = m_hash_vals->getDescriptorInfo();
	auto probes_info = m_probes->getDescriptorInfo();
	auto counters_info = m_counters->getDescriptorInfo();
	auto age_info = m_slot_age->getDescriptorInfo();
	auto free_info = m_free_lists->getDescriptorInfo();
	auto descs_info = m_cascade_descs->getDescriptorInfo();
	auto scratch_radiance_info = m_scratch_radiance->getDescriptorInfo();
	auto scratch_mass_info = m_scratch_mass->getDescriptorInfo();
	auto persist_j_info = m_persist_j->getDescriptorInfo();
	auto persist_count_info = m_persist_count->getDescriptorInfo();
	auto merged_info = m_merged->getDescriptorInfo();
	auto dirmip_info = m_dirmip->getDescriptorInfo();
	auto merged_lower_info = m_merged_lower->getDescriptorInfo();
	auto dirmip_lower_info = m_dirmip_lower->getDescriptorInfo();
	auto ray_cursor_info = m_ray_cursor->getDescriptorInfo();
	auto sky_fill_info = m_probe_sky_fill->getDescriptorInfo();
	vk::DescriptorImageInfo atlas_storage_info{
		.imageView = *m_irradiance_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo atlas_sampled_info{
		.imageView = *m_irradiance_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo spec_atlas_storage_info{
		.imageView = *m_spec_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo spec_atlas_sampled_info{
		.imageView = *m_spec_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};

	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::DescriptorImageInfo output_info{
			.imageView = *m_output_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo visibility_out_info{
			.imageView = *m_visibility_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo spec_out_info{
			.imageView = *m_spec_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo geometry_out_info{
			.imageView = *m_geometry_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		VeDescriptorWriter(*m_set_layout, *m_descriptor_pool)
			.writeImage(0, &depth_info)
			.writeBuffer(1, &keys_info)
			.writeBuffer(2, &vals_info)
			.writeBuffer(3, &probes_info)
			.writeBuffer(4, &counters_info)
			.writeBuffer(5, &age_info)
			.writeBuffer(6, &free_info)
			.writeBuffer(7, &descs_info)
			.writeImage(8, &output_info)
			.writeImage(9, &normal_info)
			.writeImage(10, &history_info)
			.writeImage(11, &sampler_info)
			.writeImage(12, &hiz_info)
			.writeBuffer(13, &scratch_radiance_info)
			.writeBuffer(14, &persist_j_info)
			.writeBuffer(15, &persist_count_info)
			.writeImage(16, &atlas_storage_info)
			.writeImage(17, &atlas_sampled_info)
			.writeBuffer(18, &merged_info)
			.writeBuffer(19, &dirmip_info)
			.writeImage(20, &spec_atlas_storage_info)
			.writeImage(21, &spec_atlas_sampled_info)
			.writeImage(22, &visibility_out_info)
			.writeImage(23, &prev_depth_info)
			.writeImage(24, &spec_out_info)
			.writeBuffer(25, &scratch_mass_info)
			.writeImage(26, &geometry_out_info)
			.writeBuffer(27, &merged_lower_info)
			.writeBuffer(28, &dirmip_lower_info)
			.writeBuffer(29, &ray_cursor_info)
			.writeBuffer(30, &sky_fill_info)
			.build(m_descriptor_sets[frame]);
	}
}

void RcSystem::recordReset(vk::raii::CommandBuffer& cmd) {
	cmd.fillBuffer(m_hash_keys->getBuffer(), 0, m_hash_words_per_parity * 2 * WORD, 0u);
	cmd.fillBuffer(m_counters->getBuffer(), 0, m_cascades * 2 * sizeof(CountersGpu), 0u);
	cmd.fillBuffer(m_free_lists->getBuffer(), 0, m_free_total * WORD, 0u);
	// Fill with ones so every unwritten hash entry reads as a husk, not garbage
	cmd.fillBuffer(m_probes->getBuffer(), 0,
		static_cast<vk::DeviceSize>(m_probes_per_parity) * 2 * 2 * WORD, 0xFFFFFFFFu);
	cmd.fillBuffer(m_scratch_radiance->getBuffer(), 0,
		static_cast<vk::DeviceSize>(m_pair_total) * SCRATCH_RADIANCE_WORDS * WORD, 0u);
	cmd.fillBuffer(m_scratch_mass->getBuffer(), 0,
		static_cast<vk::DeviceSize>(m_pair_total) * 2 * WORD, 0u);
}

void RcSystem::writeSceneBindings(uint32_t frame_index) {
	auto buffer_of = [this](const VeBuffer* buffer) {
		return buffer ? buffer : m_dummy_buffer.get();
	};
	const VeBuffer* geometry = buffer_of(m_scene.instance_geometry);
	const VeBuffer* vertices = buffer_of(m_scene.mega_vertices);
	const VeBuffer* indices = buffer_of(m_scene.mega_indices);
	const VeBuffer* lights = buffer_of(m_scene.lights);
	const bool mega_bound = m_scene.mega_vertices && m_scene.mega_indices;
	// Keyed by generation, not handle
	SceneWritten current{
		.mega_generation = mega_bound ? m_scene.mega_generation + 1 : 0,
		.geometry_generation = m_scene.instance_geometry ? m_scene.geometry_generation + 1 : 0,
		.light_generation = m_scene.lights ? m_scene.light_generation + 1 : 0,
	};
	if (m_scene_written[frame_index] == current)
		return;

	auto geometry_info = geometry->getDescriptorInfo();
	auto vertices_info = vertices->getDescriptorInfo();
	auto indices_info = indices->getDescriptorInfo();
	auto lights_info = lights->getDescriptorInfo();
	auto storage_write = [&](uint32_t binding, const vk::DescriptorBufferInfo& info) {
		return vk::WriteDescriptorSet{
			.dstSet = *m_descriptor_sets[frame_index],
			.dstBinding = binding,
			.descriptorCount = 1,
			.descriptorType = vk::DescriptorType::eStorageBuffer,
			.pBufferInfo = &info,
		};
	};
	std::array<vk::WriteDescriptorSet, 4> writes{
		storage_write(32, geometry_info),
		storage_write(33, vertices_info),
		storage_write(34, indices_info),
		storage_write(35, lights_info),
	};
	m_ve_device.getDevice().updateDescriptorSets(writes, {});
	m_scene_written[frame_index] = current;
}

void RcSystem::dispatch(VeFrameInfo& frame_info, vk::raii::CommandBuffer& cmd, bool history_copy) {

	// Reset store if ray backend changes
	const uint32_t frame = frame_info.current_frame;
	const bool ray_query = usingRayQuery();
	if (ray_query != m_last_ray_query)
		requestReset();
	m_last_ray_query = ray_query;
	if (m_reset_pending)
		m_frame_counter = 0;
	const uint32_t parity = m_frame_counter & 1u;
	const uint32_t prev = 1u - parity;
	// Byte offset of cascade n's CountersGpu block in the counters buffer.
	auto counter_block = [&](uint32_t region, uint32_t n) {
		return static_cast<vk::DeviceSize>((region * m_cascades + n) * sizeof(CountersGpu));
	};

	// Async counter readback: consume what this frame index copied out
	// MAX_FRAMES_IN_FLIGHT frames ago
	m_counters_valid = false;
	auto* blocks = static_cast<const CountersGpu*>(m_readback[frame]->getMappedMemory());
	if (blocks && m_has_readback[frame]) {
		uint32_t probes = 0;
		uint32_t overflow = 0;
		for (uint32_t n = 0; n < m_cascades; n++) {
			m_cascade_probes[n] = blocks[n].probes;
			probes += blocks[n].probes;
			overflow += blocks[n].overflow;
		}
		m_probe_count = probes;
		m_overflow_count = overflow;
		std::copy(std::begin(blocks[0].rays), std::end(blocks[0].rays), m_ray_exits.begin());
		m_counters_valid = true;
		if (overflow > 0 && !m_overflow_warned) {
			std::string per_cascade;
			for (uint32_t n = 0; n < m_cascades; n++)
				per_cascade += std::format(" c{}:{}/{}(+{})", n,
					blocks[n].probes - blocks[n].husks,
					m_descs[n].probe_cap,
					blocks[n].overflow);
			VE_LOGW("RC probe overflow (dropped probes):" << per_cascade);
			m_overflow_warned = true;
		} else if (overflow == 0 && m_overflow_warned) {
			VE_LOGI("RC probe overflow resolved.");
			m_overflow_warned = false;
		}
	}

	const float ds0 = std::max(m_rc.ds0, 0.01f);
	const glm::vec3 cam = frame_info.camera_view.position;
	const glm::ivec3 anchor_curr{
		static_cast<int>(std::floor(cam.x / ds0)),
		static_cast<int>(std::floor(cam.y / ds0)),
		static_cast<int>(std::floor(cam.z / ds0))};

	FrameProfiler* profiler = frame_info.profiler;
	if (profiler) {
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_BUILD);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_MAP);
	}

	auto memory_barrier = [&](vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
		vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
		vk::MemoryBarrier2 mem{
			.srcStageMask = src_stage,
			.srcAccessMask = src_access,
			.dstStageMask = dst_stage,
			.dstAccessMask = dst_access,
		};
		vk::DependencyInfo dep{.memoryBarrierCount = 1, .pMemoryBarriers = &mem};
		cmd.pipelineBarrier2(dep);
	};

	// The previous dispatch might still read this parity's region as its prev
	// map (keepalive, prev-slot lookups), and a pending reset overwrites
	// persistent buffers it wrote.
	memory_barrier(vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite);

	if (m_reset_pending) {
		recordReset(cmd);
		memory_barrier(vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eTransferWrite,
			vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eTransferWrite);
		m_reset_pending = false;
		m_has_prev = false;
		m_has_readback = {};
		m_counters_valid = false;
	}
	const glm::ivec3 anchor_prev = m_has_prev ? m_last_anchor0 : anchor_curr;

	// Per-frame clear of the curr map region: keys to empty, counters to zero,
	// then the y/z of all three workgroup dispatch triples to 1 (the x words
	// accumulate via InterlockedMax during insertion).
	cmd.fillBuffer(m_hash_keys->getBuffer(), parity * m_hash_words_per_parity * WORD,
		m_hash_words_per_parity * WORD, 0u);
	cmd.fillBuffer(m_counters->getBuffer(), counter_block(parity, 0),
		m_cascades * sizeof(CountersGpu), 0u);
	// Fills run unordered without a barrier, and the 1s write over part of the
	// zero fill.
	memory_barrier(vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite);
	for (uint32_t n = 0; n < m_cascades; n++) {
		cmd.fillBuffer(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, linear_y), 2 * WORD, 1u);
		cmd.fillBuffer(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, pair_y), 2 * WORD, 1u);
		cmd.fillBuffer(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, probe_y), 2 * WORD, 1u);
	}

	memory_barrier(vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
		vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eIndirectCommandRead);

	// The 9-bit key axes span +-255 cells; past that rcwEncodeKey returns 0 and
	// the probe silently drops (an outer shell in every LOD band). 252 leaves
	// slack.
	const float lod0_max = 252.0f * ds0;
	const float lod0 = std::clamp(m_rc.lod0_dist, 0.5f, lod0_max);
	if (m_rc.lod0_dist > lod0_max && !m_lod0_clamp_warned) {
		VE_LOGW("RC LOD 0 Distance " << m_rc.lod0_dist << " m exceeds the probe key range at Probe Spacing "
			<< ds0 << " m (limit " << lod0_max << " m); clamped. Raise Probe Spacing or lower LOD 0 Distance.");
		m_lod0_clamp_warned = true;
	} else if (m_rc.lod0_dist <= lod0_max) {
		m_lod0_clamp_warned = false;
	}

	const auto& proj = frame_info.camera_view.proj;
	RcwPushConstant push{
		.anchor0_curr = glm::ivec4(anchor_curr, static_cast<int>(parity)),
		.anchor0_prev = glm::ivec4(anchor_prev, std::max(m_rc.probe_ttl, 0)),
		.out_size = glm::vec2(static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_depth_extent.width), static_cast<float>(m_depth_extent.height)),
		.ds0 = ds0,
		.lod0_dist = lod0,
		.proj_22 = proj[2][2],
		.proj_32 = proj[3][2],
		.luma_clamp = m_rc.luma_clamp,
		.hash_words_per_parity = m_hash_words_per_parity,
		.probes_per_parity = m_probes_per_parity,
		.cascade_index = 0,
		.n_cascades = m_cascades,
		.debug_gather_mode = gatherMode(m_rc.debug_view),
		.probe_j_cascade = static_cast<uint32_t>(std::clamp(m_rc.probe_j_cascade, 0, static_cast<int>(m_cascades) - 1)),
		.frame_counter = m_frame_counter,
		.rays_per_pixel = static_cast<uint32_t>(std::clamp(m_rc.rays_per_pixel, 1, 16)),
		.flags = (m_rc.specular ? 0u : FLAG_NO_SPECULAR)
			| (m_rc.debug_view == RcDebugView::EVIDENCE ? FLAG_VISIBILITY_EVIDENCE : 0u)
			| (m_prev_depth_primed ? FLAG_HIST_DEPTH : 0u)
			| (m_rc.single_frame ? FLAG_SINGLE_FRAME : 0u)
			| skyModeBits(m_rc.sky_occlusion, ray_query),
		.hiz_mip_count = m_screen_rays.pyramidMipLevels(),
		.light_count = m_scene.lights ? m_scene.light_count : 0u,
		.history_length = static_cast<uint32_t>(std::clamp(m_rc.history_length, 1, RC_MAX_HISTORY_LENGTH)),
	};
	auto push_constants = [&] {
		cmd.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
			vk::ArrayProxy<const uint8_t>(sizeof(RcwPushConstant),
				reinterpret_cast<const uint8_t*>(&push)));
	};
	// Set 2 is the bindless texture set the shade path samples albedo and emissive from.
	std::array<vk::DescriptorSet, 3> sets{
		*frame_info.global_descriptor_set,
		*m_descriptor_sets[frame],
		*m_bindless_set,
	};
	// Scene bindings and TLAS go before the bind below
	if (ray_query)
		writeSceneBindings(frame);
	// Rewritten every dispatch, whichever tracer runs, rather than cached on
	// the handle: a freed structure's handle can be reissued to the next one.
	if (m_tlas) {
		vk::WriteDescriptorSetAccelerationStructureKHR tlas_info{
			.accelerationStructureCount = 1,
			.pAccelerationStructures = &m_tlas,
		};
		vk::WriteDescriptorSet write{
			.pNext = &tlas_info,
			.dstSet = *m_descriptor_sets[frame],
			.dstBinding = 31,
			.descriptorCount = 1,
			.descriptorType = vk::DescriptorType::eAccelerationStructureKHR,
		};
		m_ve_device.getDevice().updateDescriptorSets(write, {});
	}
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_pipeline_layout, 0, sets, {});

	auto chain_barrier = [&] {
		memory_barrier(vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderStorageWrite,
			vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
			vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite
				| vk::AccessFlagBits2::eIndirectCommandRead);
	};

	// ===== Cascade-0 insertion from spawn pixels =====
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_insert_c0_pipeline->getPipeline());
	push_constants();
	cmd.dispatch((m_extent.width + 7) / 8, (m_extent.height + 7) / 8, 1);

	// ===== Parent closure, cascade n -> n+1, indirect over cascade n =====
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_insert_parent_pipeline->getPipeline());
	for (uint32_t n = 0; n + 1 < m_cascades; n++) {
		chain_barrier();
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, linear_x));
	}

	// ===== TTL keepalive over the prev map (indirect over prev frame args).
	// Cascades touch disjoint buffer regions, so no barriers between them.
	chain_barrier();
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_keepalive_pipeline->getPipeline());
	for (uint32_t n = 0; n < m_cascades; n++) {
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(prev, n) + offsetof(CountersGpu, linear_x));
	}

	// Ray generation reads the finished curr maps.
	chain_barrier();

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_MAP);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_RAYGEN);
	}

	// Give every probe a contiguous run of the R2 sequence, arranged so that
	// probes sharing a parent take adjacent runs and the parent's rays are one
	// contiguous stretch too. Counts flow up the parent chain, then offsets are
	// handed back down; the trace claims each ray's index from its probe's
	// cursor. Without this, a probe's directions are an arbitrary subset of R2
	// and higher cascades under-fill
	cmd.fillBuffer(m_ray_cursor->getBuffer(), 0, m_probes_per_parity * WORD, 0u);
	memory_barrier(vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_raygen_count_c0_pipeline->getPipeline());
	push_constants();
	cmd.dispatch((m_extent.width + 7) / 8, (m_extent.height + 7) / 8, 1);

	// Counts up: cascade n must be final before it folds into cascade n + 1.
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_raygen_count_up_pipeline->getPipeline());
	for (uint32_t n = 0; n + 1 < m_cascades; n++) {
		chain_barrier();
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, linear_x));
	}

	// Offsets down: a parent's base must be final before its children carve it.
	chain_barrier();
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_raygen_offset_top_pipeline->getPipeline());
	push.cascade_index = m_cascades - 1;
	push_constants();
	cmd.dispatchIndirect(m_counters->getBuffer(),
		counter_block(parity, m_cascades - 1) + offsetof(CountersGpu, linear_x));

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_raygen_offset_down_pipeline->getPipeline());
	for (uint32_t n = m_cascades - 1; n-- > 0;) {
		chain_barrier();
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, linear_x));
	}
	chain_barrier();

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_RAYGEN);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_TRACE);
	}

	// ===== Trace + deposit over the finished maps =====
	// Output to eGeneral here already: the trace writes the outcome debug view
	// (RCW_GATHER_TRACE_KIND) directly; the gather writes it otherwise.
	vk::ImageMemoryBarrier2 to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eNone,
		.srcAccessMask = vk::AccessFlagBits2::eNone,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_output_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	// The visibility, spec and geometry outputs share the radiance output's lifecycle
	// exactly: same transition here, same acquire in acquireForRead. They must
	// not diverge or the layout the descriptors declare stops matching one of
	// them.
	vk::ImageMemoryBarrier2 visibility_to_general = to_general;
	visibility_to_general.image = m_visibility_images[frame]->getImage();
	vk::ImageMemoryBarrier2 spec_to_general = to_general;
	spec_to_general.image = m_spec_images[frame]->getImage();
	vk::ImageMemoryBarrier2 geometry_to_general = to_general;
	geometry_to_general.image = m_geometry_images[frame]->getImage();
	std::array<vk::ImageMemoryBarrier2, 4> out_barriers{to_general, visibility_to_general, spec_to_general, geometry_to_general};
	vk::DependencyInfo out_dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(out_barriers.size()),
		.pImageMemoryBarriers = out_barriers.data()};
	cmd.pipelineBarrier2(out_dep);
	// The chain barrier, and with ray query also where the TLAS lands:
	// RayTracingScene::update records its build with no barrier behind it, so
	// nothing before the trace waits for it.
	const vk::PipelineStageFlags2 tlas_stage = ray_query
		? vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR : vk::PipelineStageFlags2{};
	const vk::AccessFlags2 tlas_write = ray_query
		? vk::AccessFlagBits2::eAccelerationStructureWriteKHR : vk::AccessFlags2{};
	const vk::AccessFlags2 tlas_read = ray_query
		? vk::AccessFlagBits2::eAccelerationStructureReadKHR : vk::AccessFlags2{};
	memory_barrier(vk::PipelineStageFlagBits2::eComputeShader | tlas_stage,
		vk::AccessFlagBits2::eShaderStorageWrite | tlas_write,
		vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eDrawIndirect,
		vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite
			| vk::AccessFlagBits2::eIndirectCommandRead | tlas_read);

	// Cross-lod seeding of fresh probes (per-probe indirect over the
	// finished curr maps). Recorded with no barrier ahead of the trace so its
	// serial copies overlap it: the trace reads neither the probe records nor
	// the persistent store. Donor payloads are last frame's state; resolve, the
	// next persistent-store writer, waits behind the post-trace barrier.
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_seed_pipeline->getPipeline());
	for (uint32_t n = 0; n < m_cascades; n++) {
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, linear_x));
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute,
		ray_query ? m_trace_rayquery_pipeline->getPipeline() : m_trace_pipeline->getPipeline());
	push.cascade_index = 0;
	push_constants();
	cmd.dispatch((m_extent.width + 7) / 8, (m_extent.height + 7) / 8, 1);

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_TRACE);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_RESOLVE);
	}

	// ===== Resolve deposits into the persistent store (per-pair indirect;
	// cascades are disjoint, no barriers between them) =====
	chain_barrier();
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_resolve_pipeline->getPipeline());
	for (uint32_t n = 0; n < m_cascades; n++) {
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, pair_x));
	}

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_RESOLVE);
		profiler->endGpuTimer(cmd, ProfileTimer::RC_BUILD);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_SHADE);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_MERGE);
	}

	chain_barrier();
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_merge_pipeline->getPipeline());
	for (uint32_t n = m_cascades; n-- > 0;) {
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(), counter_block(parity, n) + offsetof(CountersGpu, pair_x));
		chain_barrier();
	}

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_MERGE);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_IRRADIANCE);
	}

	// Irradiance tiles: one workgroup per c0 probe
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_irradiance_pipeline->getPipeline());
	push.cascade_index = 0;
	push_constants();
	cmd.dispatchIndirect(m_counters->getBuffer(),
		counter_block(parity, 0) + offsetof(CountersGpu, probe_x));

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_IRRADIANCE);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_GATHER);
	}

	// Counter readback copy + gather over the finished maps + atlas
	memory_barrier(vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eShaderStorageRead
			| vk::AccessFlagBits2::eShaderSampledRead);
	vk::BufferCopy counter_copy{counter_block(parity, 0), 0, m_cascades * sizeof(CountersGpu)};
	cmd.copyBuffer(m_counters->getBuffer(), m_readback[frame]->getBuffer(), counter_copy);
	m_has_readback[frame] = true;

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_gather_pipeline->getPipeline());
	push.cascade_index = 0;
	push_constants();
	cmd.dispatch((m_extent.width + 15) / 16, (m_extent.height + 15) / 16, 1);
	// Output stays in eGeneral; acquireForRead transitions it.
	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_GATHER);
		profiler->endGpuTimer(cmd, ProfileTimer::RC_SHADE);
	}

	m_last_anchor0 = anchor_curr;
	m_has_prev = true;
	m_frame_counter++;

	// Preserve this frame's min/max depth (pyramid mip 0) to validate
	// reprojection into the history while this frame's image is the history;
	// the pyramid is rebuilt in place every march frame. Recorded after the
	// trace consumed the previous copy. A ray query frame may have built no
	// pyramid, so it leaves the next march unvalidated instead.
	if (ray_query)
		m_prev_depth_primed = false;
	if (ray_query || !history_copy)
		return;
	auto mip0_barrier = [&](vk::Image image, uint32_t mip_count,
		vk::ImageLayout from, vk::ImageLayout to,
		vk::AccessFlags2 src_access, vk::AccessFlags2 dst_access,
		vk::PipelineStageFlags2 src_stage, vk::PipelineStageFlags2 dst_stage) {
		vk::ImageMemoryBarrier2 b{
			.srcStageMask = src_stage, .srcAccessMask = src_access,
			.dstStageMask = dst_stage, .dstAccessMask = dst_access,
			.oldLayout = from, .newLayout = to,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = image,
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mip_count, 0, 1}};
		vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
		cmd.pipelineBarrier2(dep);
	};
	vk::Image pyramid = m_screen_rays.pyramidImage();
	vk::Extent2D prev_extent = m_prev_depth->getExtent2D();
	assert(prev_extent == m_screen_rays.pyramidMip0Extent());
	mip0_barrier(pyramid, 1,
		vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eTransferRead,
		vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eCopy);
	mip0_barrier(m_prev_depth->getImage(), 1,
		vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferDstOptimal,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eCopy);
	vk::ImageCopy region{
		.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
		.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
		.extent = {prev_extent.width, prev_extent.height, 1}};
	cmd.copyImage(pyramid, vk::ImageLayout::eTransferSrcOptimal,
		m_prev_depth->getImage(), vk::ImageLayout::eTransferDstOptimal, region);
	mip0_barrier(pyramid, 1,
		vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderSampledRead,
		vk::PipelineStageFlagBits2::eCopy, vk::PipelineStageFlagBits2::eComputeShader);
	mip0_barrier(m_prev_depth->getImage(), 1,
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead,
		vk::PipelineStageFlagBits2::eCopy, vk::PipelineStageFlagBits2::eComputeShader);
	m_prev_depth_primed = true;
}

void RcSystem::acquireForRead(vk::raii::CommandBuffer& cmd, uint32_t frame_index) {
	vk::ImageMemoryBarrier2 acquire{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_output_images[frame_index]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	vk::ImageMemoryBarrier2 visibility_acquire = acquire;
	visibility_acquire.image = m_visibility_images[frame_index]->getImage();
	vk::ImageMemoryBarrier2 spec_acquire = acquire;
	spec_acquire.image = m_spec_images[frame_index]->getImage();
	vk::ImageMemoryBarrier2 geometry_acquire = acquire;
	geometry_acquire.image = m_geometry_images[frame_index]->getImage();
	std::array<vk::ImageMemoryBarrier2, 4> acquires{acquire, visibility_acquire, spec_acquire, geometry_acquire};
	vk::DependencyInfo dep{
		.imageMemoryBarrierCount = static_cast<uint32_t>(acquires.size()),
		.pImageMemoryBarriers = acquires.data(),
	};
	cmd.pipelineBarrier2(dep);
}

void RcSystem::allocate() {
	bool store_changed = m_pending.cascades != m_cascades || m_pending.memory_mb != m_memory_mb;
	m_resolution_div = m_pending.resolution_div;
	m_cascades = m_pending.cascades;
	m_memory_mb = m_pending.memory_mb;
	if (store_changed) {
		computeLayout();
		m_store_stale = true;
	}
	m_extent = vk::Extent2D{std::max(1u, m_depth_extent.width / m_resolution_div),
		std::max(1u, m_depth_extent.height / m_resolution_div)};
	if (!m_trace_pipeline)
		createComputePipelines();
	createOutputImages();
	createPrevDepthImage();
	if (m_store_stale) {
		createBuffers();
		clearStoreState();
		m_store_stale = false;
	}
	createDescriptorSets();
	m_allocated = true;
}

void RcSystem::release() {
	m_descriptor_sets = makeNullArray<vk::raii::DescriptorSet>();
	releaseStore();
	m_prev_depth.reset();
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_output_images[i].reset();
		m_visibility_images[i].reset();
		m_spec_images[i].reset();
		m_geometry_images[i].reset();
	}
	m_insert_c0_pipeline.reset();
	m_insert_parent_pipeline.reset();
	m_keepalive_pipeline.reset();
	m_seed_pipeline.reset();
	m_raygen_count_c0_pipeline.reset();
	m_raygen_count_up_pipeline.reset();
	m_raygen_offset_top_pipeline.reset();
	m_raygen_offset_down_pipeline.reset();
	m_trace_pipeline.reset();
	m_trace_rayquery_pipeline.reset();
	m_resolve_pipeline.reset();
	m_merge_pipeline.reset();
	m_irradiance_pipeline.reset();
	m_gather_pipeline.reset();
	clearStoreState();
	m_store_stale = true;
	m_allocated = false;
}

void RcSystem::markInactive() {
	m_prev_depth_primed = false;
	m_has_readback = {};
	m_counters_valid = false;
}

void RcSystem::clearStoreState() {
	// Fresh store memory: the next dispatch records the reset fills, and the
	// readback ring starts over (its old slots would read unwritten staging).
	requestReset();
	m_has_readback = {};
	m_counters_valid = false;
	m_probe_count = 0;
	m_overflow_count = 0;
	m_cascade_probes = {};
	m_ray_exits = {};
	m_overflow_warned = false;
	m_lod0_clamp_warned = false;
	m_prev_depth_primed = false;
	m_has_prev = false;
}

const vk::raii::ImageView& RcSystem::getOutputImageView(uint32_t frame_index) const {
	assert(m_allocated);
	return m_output_images[frame_index]->getImageView();
}

const vk::raii::ImageView& RcSystem::getDummyImageView() const {
	return m_dummy_image->getImageView();
}

const vk::raii::ImageView& RcSystem::getVisibilityImageView(uint32_t frame_index) const {
	assert(m_allocated);
	return m_visibility_images[frame_index]->getImageView();
}

const vk::raii::ImageView& RcSystem::getVisibilityDummyImageView() const {
	return m_visibility_dummy_image->getImageView();
}

const vk::raii::ImageView& RcSystem::getSpecImageView(uint32_t frame_index) const {
	assert(m_allocated);
	return m_spec_images[frame_index]->getImageView();
}

const vk::raii::ImageView& RcSystem::getGeometryImageView(uint32_t frame_index) const {
	assert(m_allocated);
	return m_geometry_images[frame_index]->getImageView();
}

uint32_t RcSystem::probeCount() const {
	return m_counters_valid ? m_probe_count : 0u;
}

uint32_t RcSystem::overflowCount() const {
	return m_counters_valid ? m_overflow_count : 0u;
}

uint32_t RcSystem::cascadeProbeCount(uint32_t n) const {
	if (!m_counters_valid || n >= m_cascades)
		return 0u;
	return m_cascade_probes[n];
}

uint32_t RcSystem::rayExitCount(uint32_t code) const {
	if (!m_counters_valid || code >= m_ray_exits.size())
		return 0u;
	return m_ray_exits[code];
}

} // namespace ve
