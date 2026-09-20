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
	float thickness;
	float luma_clamp;
	float intensity;
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
	float blend_zone;
};
static_assert(sizeof(RcwPushConstant) == 120, "must match RcwPush in rc_world_common.slangh");

// Tile-atlas extent from capacity; mirrors rcwTilesPerRow. See shader comments.
vk::Extent2D atlasExtent(uint32_t capacity) {
	uint32_t tiles_per_row = 1u << ((std::bit_width(capacity) - 1 + 1) >> 1);
	uint32_t rows = (capacity + tiles_per_row - 1) / tiles_per_row;
	return vk::Extent2D{tiles_per_row * TILE, rows * TILE};
}
constexpr vk::DeviceSize WORD = sizeof(uint32_t);
constexpr uint32_t MAX_THETA0 = 8; // largest c0 polar bin count setQuality accepts

// Directions per probe in cascade n: 2 * theta0^2 * 4^n (32, 128, 512, ...
// at the default theta0 = 4). Mirrors rcwDirCount.
uint32_t dirCount(uint32_t theta0, uint32_t cascade) {
	return (2u * theta0 * theta0) << (2 * cascade);
}

// The gather view an RC view selects; 0 for the views that display an output as it is
uint32_t gatherMode(RcDebugView view) {
	switch (view) {
		case RcDebugView::PROBE_J:
		case RcDebugView::COVERAGE:
		case RcDebugView::TRACE_KIND:
		case RcDebugView::CONVERGENCE:
		case RcDebugView::PROBE_GRID:
		case RcDebugView::SPEC:
			return static_cast<uint32_t>(view);
		default:
			return 0;
	}
}

} // namespace

RcSystem::RcSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	std::filesystem::path shader_path,
	vk::Extent2D render_extent,
	const vk::raii::ImageView& depth_image_view,
	const vk::raii::ImageView& normal_roughness_image_view,
	const ScreenRayResources& screen_rays,
	EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_depth_extent(render_extent), m_descriptor_pool(&descriptor_pool), m_screen_rays(screen_rays) {

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
	createPipelineLayout(global_set_layout);
	computeLayout();
}

RcSystem::~RcSystem() = default;

void RcSystem::invalidateStore() {
	requestReset();
	// A dropped/aborted frame voids the "prev depth == the frame the history
	// shows" pairing; run the next trace frame unvalidated.
	m_prev_depth_primed = false;
}

void RcSystem::setParams(const RcSettings& rc) {
	// Structural params re-key every probe; the population rules decide which
	// probes exist and receive deposits, so the other rule's probes and their
	// EMA must not survive a switch
	bool structural = rc.ds0 != m_rc.ds0 || rc.lod0_dist != m_rc.lod0_dist;
	bool population = rc.deposit_mode != m_rc.deposit_mode || rc.parent_mode != m_rc.parent_mode;
	bool requested = rc.reset_requests != m_rc.reset_requests;
	m_rc = rc;
	if (structural || population || requested)
		requestReset();
}

bool RcSystem::setQuality(uint32_t resolution_div, uint32_t cascades, uint32_t memory_mb,
	uint32_t c0_polar_bins, float c0_ray_length) {
	Quality request{
		.resolution_div = std::clamp(resolution_div, 1u, 8u),
		.cascades = static_cast<uint32_t>(std::clamp(static_cast<int>(cascades), 1,
			static_cast<int>(MAX_CASCADES))),
		.memory_mb = static_cast<uint32_t>(std::clamp(static_cast<int>(memory_mb), 16, 4096)),
		.theta0 = static_cast<uint32_t>(std::clamp(static_cast<int>(c0_polar_bins), 2,
			static_cast<int>(MAX_THETA0))),
		.c0_ray_length = std::clamp(c0_ray_length, 0.5f, 8.0f),
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
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled
				| vk::ImageUsageFlagBits::eTransferSrc,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		// Rests in eShaderReadOnlyOptimal (read by fragment between frames)
		m_output_images[i]->transitionImageLayout(
			vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eFragmentShader);
		m_output_images[i]->setDebugName(("RC Output [" + std::to_string(i) + "]").c_str());

		// No eTransferSrc: the GI dump contract is 4-channel radiance only.
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
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled
				| vk::ImageUsageFlagBits::eTransferSrc,
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
	// Per-cascade capacity as a share of cascade 0's
	constexpr double SHAPE[MAX_CASCADES] = {1.0, 0.2769, 0.0731, 0.0197, 0.0053, 0.0014};
	constexpr uint32_t BYTES_PER_PAIR = SCRATCH_WORDS * WORD + 2 * WORD + WORD;
	double unit_cost = 0.0;
	for (uint32_t n = 0; n < m_cascades; n++)
		unit_cost += SHAPE[n] * dirCount(m_theta0, n) * BYTES_PER_PAIR;
	double budget = static_cast<double>(m_memory_mb) * 1024.0 * 1024.0;
	// 65,535 * LINEAR_WG: keeps the per-probe indirect (ceil(probe_cap /
	// LINEAR_WG) groups) within limits
	uint32_t cap0 = static_cast<uint32_t>(
		std::clamp(budget / unit_cost, 1024.0, 65535.0 * LINEAR_WG));
	// Device limits: scratch, the largest buffer at SCRATCH_WORDS words per
	// pair, must fit one allocation and one whole storage binding, and the c0
	// tile atlases one image.
	double buffer_cap0 = static_cast<double>(m_max_buffer_bytes)
		/ (SCRATCH_WORDS * WORD * (unit_cost / BYTES_PER_PAIR));
	uint32_t tiles_per_side = m_max_image_dimension / TILE;
	uint32_t tiles_per_row = std::bit_floor(tiles_per_side);
	double atlas_cap0 = std::min(2.0 * tiles_per_row * tiles_per_row - 1.0,
		static_cast<double>(tiles_per_row) * tiles_per_side);
	double device_cap0 = std::min(buffer_cap0, atlas_cap0);
	m_device_clamped = cap0 > device_cap0;
	if (m_device_clamped)
		cap0 = static_cast<uint32_t>(device_cap0);
	uint32_t hash_off = 0;
	uint32_t probe_off = 0;
	uint32_t slot_off = 0;
	uint32_t free_off = 0;
	uint32_t pair_off = 0;
	m_descs = {};
	// Accumulated below with max; a recreate must not inherit a larger layout.
	m_merged_cap = 0;
	m_dirmip_cap = 0;
	for (uint32_t n = 0; n < m_cascades; n++) {
		uint32_t cap = std::max(256u, static_cast<uint32_t>(std::llround(cap0 * SHAPE[n])));
		m_descs[n] = CascadeDesc{
			.hash_base = hash_off,
			.hash_cap = std::bit_ceil(cap * 2),
			.probe_base = probe_off,
			.probe_cap = cap,
			.slot_base = slot_off,
			.free_base = free_off,
			.pair_base = pair_off,
		};
		hash_off += m_descs[n].hash_cap;
		probe_off += cap;
		slot_off += cap;
		free_off += 2 + cap;
		pair_off += cap * dirCount(m_theta0, n);
		m_merged_cap = std::max(m_merged_cap, cap * dirCount(m_theta0, n));
		if (n > 0)
			m_dirmip_cap = std::max(m_dirmip_cap, cap * dirCount(m_theta0, n) / 4);
	}
	m_dirmip_cap = std::max(m_dirmip_cap, 1u);
	m_hash_words_per_parity = hash_off;
	m_probes_per_parity = probe_off;
	m_slot_total = slot_off;
	m_free_total = free_off;
	m_pair_total = pair_off;
	if (static_cast<vk::DeviceSize>(m_pair_total) * SCRATCH_WORDS * WORD > m_max_buffer_bytes)
		VE_LOGE("RC store: the per-cascade probe floors exceed the device buffer limit; lower Probe Directions or Cascades");

	// Debug-panel readout
	vk::Extent2D atlas = atlasExtent(m_descs[0].probe_cap);
	m_store_bytes = static_cast<vk::DeviceSize>(m_pair_total) * BYTES_PER_PAIR
		+ static_cast<vk::DeviceSize>(m_merged_cap + m_dirmip_cap) * 2 * WORD
		+ static_cast<vk::DeviceSize>(m_merged_cap + m_dirmip_cap) * WORD
		+ static_cast<vk::DeviceSize>(m_hash_words_per_parity) * 2 * 2 * WORD
		+ static_cast<vk::DeviceSize>(m_probes_per_parity) * 2 * 2 * WORD
		+ static_cast<vk::DeviceSize>(m_slot_total + m_free_total) * WORD
		+ static_cast<vk::DeviceSize>(m_cascades) * 2 * sizeof(CountersGpu)
		+ static_cast<vk::DeviceSize>(RAY_STAT_WORDS) * 2 * WORD
		// RGBA16F irradiance + R16F sky-visibility tile atlases.
		+ static_cast<vk::DeviceSize>(atlas.width) * atlas.height * (8 + 2);
}

void RcSystem::releaseStore() {
	for (auto& slot : m_readback)
		slot.reset();
	m_visibility_atlas.reset();
	m_irradiance_atlas.reset();
	m_dirmip_lower.reset();
	m_merged_lower.reset();
	m_dirmip.reset();
	m_merged.reset();
	m_persist_count.reset();
	m_persist_j.reset();
	m_scratch.reset();
	m_cascade_descs.reset();
	m_free_lists.reset();
	m_slot_age.reset();
	m_ray_stat_buffer.reset();
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
	m_ray_stat_buffer = std::make_unique<VeBuffer>(m_ve_device, WORD, RAY_STAT_WORDS * 2,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst
			| vk::BufferUsageFlagBits::eTransferSrc,
		device_local);
	m_ray_stat_buffer->setDebugName("RCW Ray Stats");
	m_slot_age = std::make_unique<VeBuffer>(m_ve_device, WORD, m_slot_total,
		vk::BufferUsageFlagBits::eStorageBuffer, device_local);
	m_slot_age->setDebugName("RCW Slot Age");
	m_free_lists = std::make_unique<VeBuffer>(m_ve_device, WORD, m_free_total,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_free_lists->setDebugName("RCW Free Lists");
	m_scratch = std::make_unique<VeBuffer>(m_ve_device, WORD, m_pair_total * SCRATCH_WORDS,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		device_local);
	m_scratch->setDebugName("RCW Deposit Scratch");
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

	// Sky visibility: one scalar per irradiance texel, same tile geometry.
	m_visibility_atlas = std::make_unique<VeImage>(
		m_ve_device, atlas.width, atlas.height,
		vk::SampleCountFlagBits::e1, vk::Format::eR16Sfloat,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1);
	m_visibility_atlas->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
		vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eComputeShader);
	m_visibility_atlas->setDebugName("RCW Sky Visibility Atlas");

	m_cascade_descs = std::make_unique<VeBuffer>(m_ve_device, sizeof(CascadeDesc), MAX_CASCADES,
		vk::BufferUsageFlagBits::eStorageBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	m_cascade_descs->map();
	m_cascade_descs->writeToBuffer(m_descs.data(), sizeof(CascadeDesc) * MAX_CASCADES);
	m_cascade_descs->setDebugName("RCW Cascade Descs");

	// Readback staging: the cascade counter structs, then the ray stat words.
	const uint32_t readback_words =
		m_cascades * static_cast<uint32_t>(sizeof(CountersGpu) / sizeof(uint32_t)) + RAY_STAT_WORDS;
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_readback[i] = std::make_unique<VeBuffer>(m_ve_device, WORD, readback_words,
			vk::BufferUsageFlagBits::eTransferDst,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			1, HostAccess::Random);
		m_readback[i]->map();
	}
}

void RcSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout) {
	m_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
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
		.addBinding(13, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // deposit scratch
		.addBinding(14, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // persistent J
		.addBinding(15, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // persistent count
		.addBinding(16, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // irradiance atlas RW
		.addBinding(17, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // irradiance atlas sampled
		.addBinding(18, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // merged I
		.addBinding(19, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // dir mip
		.addBinding(20, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // sky visibility atlas RW
		.addBinding(21, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)  // sky visibility atlas sampled
		.addBinding(22, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // gathered sky visibility
		.addBinding(23, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // prev min/max depth
		.addBinding(24, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // gathered spec
		.addBinding(25, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // ray stats
		.addBinding(26, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)  // gathered surface
		.addBinding(27, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // merged lower sky fraction
		.addBinding(28, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute) // dir mip lower sky fraction
		.build();

	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(RcwPushConstant),
	};
	std::array<vk::DescriptorSetLayout, 2> set_layouts{
		*global_set_layout,
		*m_set_layout->getDescriptorSetLayout(),
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
	const std::unordered_map<uint32_t, uint32_t> specialization{
		{THETA0_SPEC_ID, m_theta0},
		{C0_RAY_LENGTH_SPEC_ID, std::bit_cast<uint32_t>(m_c0_ray_length)},
	};
	m_insert_c0_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_insert_c0_comp.spv", m_pipeline_layout, specialization);
	m_insert_parent_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_insert_parent_comp.spv", m_pipeline_layout, specialization);
	m_keepalive_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_keepalive_comp.spv", m_pipeline_layout, specialization);
	m_seed_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_seed_comp.spv", m_pipeline_layout, specialization);
	m_trace_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_trace_deposit_comp.spv", m_pipeline_layout, specialization);
	m_resolve_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_resolve_comp.spv", m_pipeline_layout, specialization);
	m_merge_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_merge_comp.spv", m_pipeline_layout, specialization);
	m_dirmip_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_dirmip_comp.spv", m_pipeline_layout, specialization);
	m_irradiance_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_irradiance_comp.spv", m_pipeline_layout, specialization);
	m_gather_pipeline = std::make_unique<VeComputePipeline>(
		m_ve_device, m_shader_path / "rcw_gather_comp.spv", m_pipeline_layout, specialization);
	m_pipeline_theta0 = m_theta0;
	m_pipeline_c0_ray_length = m_c0_ray_length;
}

void RcSystem::createDescriptorSets() {
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
	auto ray_stats_info = m_ray_stat_buffer->getDescriptorInfo();
	auto age_info = m_slot_age->getDescriptorInfo();
	auto free_info = m_free_lists->getDescriptorInfo();
	auto descs_info = m_cascade_descs->getDescriptorInfo();
	auto scratch_info = m_scratch->getDescriptorInfo();
	auto persist_j_info = m_persist_j->getDescriptorInfo();
	auto persist_count_info = m_persist_count->getDescriptorInfo();
	auto merged_info = m_merged->getDescriptorInfo();
	auto dirmip_info = m_dirmip->getDescriptorInfo();
	auto merged_lower_info = m_merged_lower->getDescriptorInfo();
	auto dirmip_lower_info = m_dirmip_lower->getDescriptorInfo();
	vk::DescriptorImageInfo atlas_storage_info{
		.imageView = *m_irradiance_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo atlas_sampled_info{
		.imageView = *m_irradiance_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo visibility_storage_info{
		.imageView = *m_visibility_atlas->getImageView(),
		.imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo visibility_sampled_info{
		.imageView = *m_visibility_atlas->getImageView(),
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
			.writeBuffer(13, &scratch_info)
			.writeBuffer(14, &persist_j_info)
			.writeBuffer(15, &persist_count_info)
			.writeImage(16, &atlas_storage_info)
			.writeImage(17, &atlas_sampled_info)
			.writeBuffer(18, &merged_info)
			.writeBuffer(19, &dirmip_info)
			.writeImage(20, &visibility_storage_info)
			.writeImage(21, &visibility_sampled_info)
			.writeImage(22, &visibility_out_info)
			.writeImage(23, &prev_depth_info)
			.writeImage(24, &spec_out_info)
			.writeBuffer(25, &ray_stats_info)
			.writeImage(26, &geometry_out_info)
			.writeBuffer(27, &merged_lower_info)
			.writeBuffer(28, &dirmip_lower_info)
			.build(m_descriptor_sets[frame]);
	}
}

void RcSystem::recordReset(vk::raii::CommandBuffer& cmd) {
	cmd.fillBuffer(m_hash_keys->getBuffer(), 0, m_hash_words_per_parity * 2 * WORD, 0u);
	cmd.fillBuffer(m_counters->getBuffer(), 0, m_cascades * 2 * sizeof(CountersGpu), 0u);
	cmd.fillBuffer(m_ray_stat_buffer->getBuffer(), 0, RAY_STAT_WORDS * 2 * WORD, 0u);
	cmd.fillBuffer(m_free_lists->getBuffer(), 0, m_free_total * WORD, 0u);
	// Fill with ones so every unwritten hash entry reads as a husk, not garbage
	cmd.fillBuffer(m_probes->getBuffer(), 0,
		static_cast<vk::DeviceSize>(m_probes_per_parity) * 2 * 2 * WORD, 0xFFFFFFFFu);
	cmd.fillBuffer(m_scratch->getBuffer(), 0,
		static_cast<vk::DeviceSize>(m_pair_total) * SCRATCH_WORDS * WORD, 0u);
}

void RcSystem::dispatch(VeFrameInfo& frame_info, vk::raii::CommandBuffer& cmd, bool history_copy) {
	const uint32_t frame = frame_info.current_frame;
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
	auto* staging = static_cast<const uint32_t*>(m_readback[frame]->getMappedMemory());
	if (staging && m_has_readback[frame]) {
		auto* blocks = reinterpret_cast<const CountersGpu*>(staging);
		const uint32_t* stats = staging + m_cascades * (sizeof(CountersGpu) / sizeof(uint32_t));
		uint32_t probes = 0;
		uint32_t overflow = 0;
		for (uint32_t n = 0; n < m_cascades; n++) {
			m_cascade_probes[n] = blocks[n].probes;
			probes += blocks[n].probes;
			overflow += blocks[n].overflow;
		}
		m_probe_count = probes;
		m_overflow_count = overflow;
		for (uint32_t i = 0; i < RAY_STAT_WORDS; i++)
			m_ray_stats[i] = stats[i];
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

	// Per-frame clear of the curr map region: keys to empty, counters and ray
	// stats to zero, then the y/z of all three workgroup dispatch triples to 1
	// (the x words accumulate via InterlockedMax during insertion).
	cmd.fillBuffer(m_hash_keys->getBuffer(), parity * m_hash_words_per_parity * WORD,
		m_hash_words_per_parity * WORD, 0u);
	cmd.fillBuffer(m_counters->getBuffer(), counter_block(parity, 0),
		m_cascades * sizeof(CountersGpu), 0u);
	cmd.fillBuffer(m_ray_stat_buffer->getBuffer(), parity * RAY_STAT_WORDS * WORD,
		RAY_STAT_WORDS * WORD, 0u);
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
		.thickness = m_rc.thickness,
		.luma_clamp = m_rc.luma_clamp,
		.intensity = m_rc.intensity,
		.hash_words_per_parity = m_hash_words_per_parity,
		.probes_per_parity = m_probes_per_parity,
		.cascade_index = 0,
		.n_cascades = m_cascades,
		.debug_gather_mode = gatherMode(m_rc.debug_view),
		.probe_j_cascade = static_cast<uint32_t>(std::clamp(m_rc.probe_j_cascade, 0, static_cast<int>(m_cascades) - 1)),
		.frame_counter = m_frame_counter,
		.rays_per_pixel = static_cast<uint32_t>(std::clamp(m_rc.rays_per_pixel, 1, 16)),
		.flags = (static_cast<uint32_t>(std::clamp(m_rc.sky_visibility, 0.0f, 1.0f) * 255.0f + 0.5f)
				<< SKY_TRUST_SHIFT)
			| (m_rc.debug_view == RcDebugView::EVIDENCE ? FLAG_VISIBILITY_EVIDENCE : 0u)
			| (m_rc.rough_specular || m_rc.debug_view == RcDebugView::SPEC ? FLAG_ROUGH_SPEC : 0u)
			| (m_rc.count_weighted_read ? FLAG_COUNT_WEIGHT : 0u)
			| (m_rc.hist_depth_check && m_prev_depth_primed ? FLAG_HIST_DEPTH : 0u)
			| (m_rc.freeze_probes ? FLAG_FREEZE_PROBES : 0u)
			| (m_rc.single_frame ? FLAG_SINGLE_FRAME : 0u)
			| (static_cast<uint32_t>(std::clamp(m_rc.parent_mode, 0, 1))
				<< PARENT_MODE_SHIFT)
			| (static_cast<uint32_t>(std::clamp(m_rc.deposit_mode, 0, 2))
				<< DEPOSIT_MODE_SHIFT)
			| (static_cast<uint32_t>(std::clamp(m_rc.sky_min_frac, 0.0f, 1.0f) * 255.0f + 0.5f)
				<< SKY_MIN_FRAC_SHIFT),
		.hiz_mip_count = m_screen_rays.pyramidMipLevels(),
		.blend_zone = std::clamp(m_rc.blend_zone, 0.0f, 0.9f),
	};
	auto push_constants = [&] {
		cmd.pushConstants(*m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
			vk::ArrayProxy<const uint8_t>(sizeof(RcwPushConstant),
				reinterpret_cast<const uint8_t*>(&push)));
	};
	std::array<vk::DescriptorSet, 2> sets{
		*frame_info.global_descriptor_set,
		*m_descriptor_sets[frame],
	};
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

	// ===== Cross-lod seeding of fresh probes (per-probe indirect over the
	// finished curr maps; donor payloads are last frame's state - resolve, the
	// next persistent-store writer, runs after the trace barrier) =====
	chain_barrier();
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_seed_pipeline->getPipeline());
	for (uint32_t n = 0; n < m_cascades; n++) {
		push.cascade_index = n;
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(),
			counter_block(parity, n) + offsetof(CountersGpu, linear_x));
	}

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_MAP);
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
	// them (the spec image's content is only written under the feature flag,
	// but its layout ping-pongs unconditionally).
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
	chain_barrier();
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_trace_pipeline->getPipeline());
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

	// ===== Top-down merge with dir-mip pre-averaging: merge(n) writes the
	// cascade-local merged I, dirmip(n) extracts its 4:1 direction average for
	// merge(n-1) before the buffer is overwritten =====
	chain_barrier();
	for (int32_t n = static_cast<int32_t>(m_cascades) - 1; n >= 0; n--) {
		vk::DeviceSize pair_args =
			counter_block(parity, static_cast<uint32_t>(n)) + offsetof(CountersGpu, pair_x);
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_merge_pipeline->getPipeline());
		push.cascade_index = static_cast<uint32_t>(n);
		push_constants();
		cmd.dispatchIndirect(m_counters->getBuffer(), pair_args);
		chain_barrier();
		if (n > 0) {
			cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_dirmip_pipeline->getPipeline());
			push_constants();
			cmd.dispatchIndirect(m_counters->getBuffer(), pair_args);
			chain_barrier();
		}
	}

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_MERGE);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_IRRADIANCE);
	}

	// ===== Irradiance tiles: one workgroup per c0 probe =====
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, m_irradiance_pipeline->getPipeline());
	push.cascade_index = 0;
	push_constants();
	cmd.dispatchIndirect(m_counters->getBuffer(),
		counter_block(parity, 0) + offsetof(CountersGpu, probe_x));

	if (profiler) {
		profiler->endGpuTimer(cmd, ProfileTimer::RC_IRRADIANCE);
		profiler->beginGpuTimer(cmd, ProfileTimer::RC_GATHER);
	}

	// ===== Counter readback copy + gather over the finished maps + atlas =====
	memory_barrier(vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderStorageWrite,
		vk::PipelineStageFlagBits2::eTransfer | vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eShaderStorageRead
			| vk::AccessFlagBits2::eShaderSampledRead);
	vk::BufferCopy counter_copy{counter_block(parity, 0), 0, m_cascades * sizeof(CountersGpu)};
	cmd.copyBuffer(m_counters->getBuffer(), m_readback[frame]->getBuffer(), counter_copy);
	vk::BufferCopy stat_copy{parity * RAY_STAT_WORDS * WORD,
		m_cascades * sizeof(CountersGpu), RAY_STAT_WORDS * WORD};
	cmd.copyBuffer(m_ray_stat_buffer->getBuffer(), m_readback[frame]->getBuffer(), stat_copy);
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
	// the pyramid is rebuilt in place every trace frame. Recorded after the
	// trace consumed the previous copy.
	if (!history_copy)
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
	bool store_changed = m_pending.cascades != m_cascades || m_pending.memory_mb != m_memory_mb
		|| m_pending.theta0 != m_theta0 || m_pending.c0_ray_length != m_c0_ray_length;
	m_resolution_div = m_pending.resolution_div;
	m_cascades = m_pending.cascades;
	m_memory_mb = m_pending.memory_mb;
	m_theta0 = m_pending.theta0;
	m_c0_ray_length = m_pending.c0_ray_length;
	if (store_changed) {
		computeLayout();
		m_store_stale = true;
	}
	m_extent = vk::Extent2D{std::max(1u, m_depth_extent.width / m_resolution_div),
		std::max(1u, m_depth_extent.height / m_resolution_div)};
	if (!m_trace_pipeline || m_pipeline_theta0 != m_theta0 || m_pipeline_c0_ray_length != m_c0_ray_length)
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
	m_trace_pipeline.reset();
	m_resolve_pipeline.reset();
	m_merge_pipeline.reset();
	m_dirmip_pipeline.reset();
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
	m_ray_stats = {};
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

uint32_t RcSystem::rayStat(uint32_t i) const {
	if (!m_counters_valid || i >= m_ray_stats.size())
		return 0u;
	return m_ray_stats[i];
}

} // namespace ve
