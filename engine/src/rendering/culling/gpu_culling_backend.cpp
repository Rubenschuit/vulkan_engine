#include "pch.hpp"
#include "rendering/culling/gpu_culling_backend.hpp"
#include "rendering/culling/gpu_culling_system.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/geometry_prepass_system.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/ve_renderer.hpp"
#include "rendering/ve_frame_info.hpp"
#include "rendering/frame_stats.hpp"

namespace ve {

GpuCullingBackend::GpuCullingBackend(GpuCullingSystem& cull_system, GpuSceneManager& gpu_scene)
	: m_cull{cull_system}, m_gpu_scene{gpu_scene} {}

void GpuCullingBackend::cull(VeFrameInfo& fi, GpuSceneManager& gpu_scene) {
	auto& cmd = fi.cmd();
	gpu_scene.updateDirtyTransforms(fi.current_frame, *fi.registry, cmd);
	m_cull.dispatch(cmd, fi, gpu_scene);
}

GpuCullingBackend::IndirectDrawSource GpuCullingBackend::getDrawSource(uint32_t frame) const {
	return {
		.indirect = m_cull.getIndirectBuffer(frame),
		.bucket_offsets = m_gpu_scene.getBucketGroupOffsets(),
		.bucket_counts = m_gpu_scene.getBucketGroupCounts(),
		.compact_indirect = m_cull.compactionEnabled()
			? &m_cull.getCompactedIndirectBuffer(frame) : nullptr,
		.compact_counts = m_cull.compactionEnabled()
			? &m_cull.getCompactCountBuffer(frame) : nullptr,
	};
}

void GpuCullingBackend::renderGeometryPrePass(VeFrameInfo& fi, PbrMegaBuffer& mega,
                                           GeometryPrePassSystem& dps,
                                           const vk::raii::DescriptorSet& bindless_set) const {
	auto src = getDrawSource(fi.current_frame);
	dps.renderGpuCulled(fi, mega, bindless_set, src.indirect,
		src.bucket_offsets, src.bucket_counts, 4,
		src.compact_indirect, src.compact_counts);
}

void GpuCullingBackend::renderShadows(VeFrameInfo& fi, ShadowRenderSystem& srs,
                                      PbrMegaBuffer& mega, GpuSceneManager& gpu_scene) const {
	srs.renderShadowMapsGpuCulled(fi, m_cull, mega, gpu_scene);
}

void GpuCullingBackend::renderOpaque(VeFrameInfo& fi, PbrRenderSystem& pbr,
                                     const vk::raii::DescriptorSet& bindless) const {
	auto src = getDrawSource(fi.current_frame);
	pbr.renderOpaqueGpuCulled(fi, bindless, src.indirect,
		src.bucket_offsets, src.bucket_counts,
		src.compact_indirect, src.compact_counts);
}

void GpuCullingBackend::renderTransparency(VeFrameInfo& fi, PbrRenderSystem& pbr,
                                           const vk::raii::DescriptorSet& bindless,
                                           GpuSceneManager& gpu_scene,
                                           VeRenderer& renderer) const {
	const auto* bucket_counts = gpu_scene.getBucketGroupCounts();
	bool has_transparents = (bucket_counts[4] > 0 || bucket_counts[5] > 0);
	if (!has_transparents)
		return;

	auto& cmd = fi.cmd();
	auto src = getDrawSource(fi.current_frame);

	renderer.beginWboitRender(cmd);
	pbr.renderTransparentWboit(fi, bindless, src.indirect,
		gpu_scene.getBucketGroupOffsets(), bucket_counts,
		src.compact_indirect, src.compact_counts);
	renderer.endWboitRender(cmd);

	renderer.beginWboitComposite(cmd);
	pbr.compositeWboit(cmd);
	renderer.endWboitComposite(cmd);
}

vk::raii::DescriptorSet& GpuCullingBackend::getGlobalDescriptorSet(uint32_t frame) {
	return m_cull.getGlobalDescriptorSet(frame);
}

void GpuCullingBackend::snapshotStats(uint32_t frame) {
	m_cull.snapshotReadback(frame);
}

void GpuCullingBackend::collectStats(uint32_t, FrameStats& stats, Registry&) const {
	stats.cull_total_objects = m_gpu_scene.getTotalRegisteredCount();
	stats.cull_visible_objects = m_cull.readbackDrawCounts();
	stats.visible_triangles = m_cull.readbackTriangleCount();
	stats.visible_meshlets = 0;
}

void GpuCullingBackend::setHizEnabled(bool enabled) { m_cull.setHizEnabled(enabled); }
bool GpuCullingBackend::isHizEnabled() const { return m_cull.isHizEnabled(); }

} // namespace ve
