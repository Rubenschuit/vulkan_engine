#include "pch.hpp"
#include "rendering/culling/meshlet_culling_backend.hpp"
#include "rendering/culling/meshlet_culling_system.hpp"
#include "rendering/culling/gpu_culling_system.hpp"
#include "rendering/managers/gpu_scene_manager.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/ve_renderer.hpp"
#include "rendering/ve_frame_info.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "ui/imgui_layer.hpp"

namespace ve {

MeshletCullingBackend::MeshletCullingBackend(MeshletCullingSystem& meshlet,
                                             GpuCullingSystem& gpu_cull,
                                             GpuSceneManager& gpu_scene,
                                             EventBus& event_bus)
	: m_meshlet_cull_system{meshlet}, m_gpu_cull{gpu_cull}, m_gpu_scene{gpu_scene} {
	event_bus.subscribe<GpuShadowFallbackChangedEvent>([this](const GpuShadowFallbackChangedEvent& e) {
		m_gpu_shadow_fallback = e.enabled;
	});
}

void MeshletCullingBackend::cull(VeFrameInfo& fi, GpuSceneManager& gpu_scene) {
	auto& cmd = fi.cmd();
	gpu_scene.updateDirtyTransforms(fi.current_frame, *fi.registry, cmd);
	m_meshlet_cull_system.dispatch(cmd, fi, gpu_scene);
}

void MeshletCullingBackend::renderDepthPrePass(VeFrameInfo& fi, PbrMegaBuffer& mega,
                                               DepthPrePassSystem& dps) const {
	dps.renderGpuCulledMeshlets(fi, mega,
		m_meshlet_cull_system.getMeshletIndirectBuffer(fi.current_frame),
		m_meshlet_cull_system.getMeshletDrawCounts(fi.current_frame),
		m_meshlet_cull_system.getCpuDrawCounts());
}

void MeshletCullingBackend::renderShadows(VeFrameInfo& fi, ShadowRenderSystem& srs,
                                          PbrMegaBuffer& mega,
                                          GpuSceneManager& gpu_scene) const {
	if (m_gpu_shadow_fallback)
		srs.renderShadowMapsGpuCulled(fi, m_gpu_cull, mega, gpu_scene);
	else
		srs.renderShadowMapsGpuCulledMeshlets(fi, m_meshlet_cull_system, mega, gpu_scene);
}

void MeshletCullingBackend::renderOpaque(VeFrameInfo& fi, PbrRenderSystem& pbr,
                                         const vk::raii::DescriptorSet& bindless) const {
	pbr.renderOpaqueGpuCulledMeshlets(fi, bindless,
		m_meshlet_cull_system.getMeshletIndirectBuffer(fi.current_frame),
		m_meshlet_cull_system.getMeshletDrawCounts(fi.current_frame),
		m_meshlet_cull_system.getCpuDrawCounts());
}

void MeshletCullingBackend::renderTransparency(VeFrameInfo& fi, PbrRenderSystem& pbr,
                                               const vk::raii::DescriptorSet& bindless,
                                               GpuSceneManager&, VeRenderer& renderer) const {
	uint32_t frame = fi.current_frame;
	auto& indirect = m_meshlet_cull_system.getMeshletIndirectBuffer(frame);
	auto& draw_counts = m_meshlet_cull_system.getMeshletDrawCounts(frame);
	const uint32_t* cpu_counts = m_meshlet_cull_system.getCpuDrawCounts();

	// Check if there are transparent draws (buckets 4-5)
	if (cpu_counts) {
		if (cpu_counts[4] == 0 && cpu_counts[5] == 0)
			return;
	}

	auto& cmd = fi.cmd();
	renderer.beginWboitRender(cmd);
	pbr.renderTransparentWboitMeshlets(fi, bindless, indirect, draw_counts, cpu_counts);
	renderer.endWboitRender(cmd);

	renderer.beginWboitComposite(cmd);
	pbr.compositeWboit(cmd);
	renderer.endWboitComposite(cmd);
}

vk::raii::DescriptorSet& MeshletCullingBackend::getGlobalDescriptorSet(uint32_t frame) {
	return m_meshlet_cull_system.getGlobalDescriptorSet(frame);
}

void MeshletCullingBackend::collectStats(uint32_t frame, UIContext& ui, Registry&) const {
	(void)frame;
	ui.stats.cull_total_objects = m_gpu_scene.getTotalRegisteredCount();
	uint32_t readback_visible = m_meshlet_cull_system.readbackVisibleObjectCount();
	ui.stats.cull_visible_objects = readback_visible > 0 ? readback_visible : ui.stats.cull_total_objects;
	const uint32_t* rb = m_meshlet_cull_system.getRawDrawCounts();
	if (rb) {
		uint32_t total_draws = 0;
		for (uint32_t b = 0; b < MeshletCullingSystem::BUCKET_COUNT; b++)
			total_draws += rb[b];
		ui.stats.visible_meshlets = total_draws;
	}
	ui.stats.visible_triangles = m_meshlet_cull_system.readbackTriangleCount();
}

void MeshletCullingBackend::setHizEnabled(bool enabled) { m_meshlet_cull_system.setHizEnabled(enabled); }
bool MeshletCullingBackend::isHizEnabled() const { return m_meshlet_cull_system.isHizEnabled(); }

} // namespace ve
