#include "pch.hpp"
#include "rendering/culling/cpu_culling_backend.hpp"
#include "rendering/culling/culling_system.hpp"
#include "rendering/depth_prepass_system.hpp"
#include "rendering/pbr_render_system.hpp"
#include "rendering/shadow_render_system.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
#include "rendering/ve_frame_info.hpp"
#include "scene/ve_registry.hpp"
#include "ui/imgui_layer.hpp"
#include "vulkan/ve_thread_pool.hpp"

namespace ve {

CpuCullingBackend::CpuCullingBackend(CullingSystem& culling, PbrRenderSystem& pbr,
                                     MaterialSSBOManager& mat_mgr, UIContext& ui,
                                     std::vector<vk::raii::DescriptorSet>& global_sets,
                                     VeThreadPool& thread_pool)
	: m_culling{culling}, m_pbr{pbr}, m_mat_mgr{mat_mgr},
	  m_ui{ui}, m_global_sets{global_sets}, m_thread_pool{thread_pool} {}

void CpuCullingBackend::cull(VeFrameInfo& fi, GpuSceneManager&) {
	m_culling.setCullingEnabled(m_ui.enable_frustum_culling);
	m_culling.setForceLodLevel(m_ui.lod_force_level);
	m_culling.setLodThresholds(m_ui.lod_screen_thresholds);
	m_culling.setLodHysteresis(m_ui.lod_hysteresis);
	m_culling.setMinParallelEntities(static_cast<uint32_t>(m_ui.min_parallel_cull_entities));
	m_culling.cullObjects(fi, &m_thread_pool);

	m_pbr.prepareFrame(fi, m_mat_mgr);
}

void CpuCullingBackend::renderDepthPrePass(VeFrameInfo& fi, PbrMegaBuffer& mega,
                                           DepthPrePassSystem& dps) const {
	dps.render(fi, mega,
		m_pbr.getIndirectBuffer(fi.current_frame),
		m_pbr.getDepthBucketOffsets(),
		m_pbr.getDepthBucketCounts(), 2);
}

void CpuCullingBackend::renderShadows(VeFrameInfo& fi, ShadowRenderSystem& srs,
                                      PbrMegaBuffer&, GpuSceneManager&) const {
	srs.renderShadowMaps(fi);
}

void CpuCullingBackend::renderOpaque(VeFrameInfo& fi, PbrRenderSystem& pbr,
                                     const vk::raii::DescriptorSet& bindless) const {
	pbr.renderOpaque(fi, bindless);
}

void CpuCullingBackend::renderTransparency(VeFrameInfo&, PbrRenderSystem&,
                                           const vk::raii::DescriptorSet&,
                                           GpuSceneManager&, VeRenderer&) const {
	// CPU transparent rendering happens inside the scene render pass 
}

vk::raii::DescriptorSet& CpuCullingBackend::getGlobalDescriptorSet(uint32_t frame) {
	return m_global_sets[frame];
}

void CpuCullingBackend::collectStats(uint32_t, UIContext& ui, Registry& registry) const {
	ui.stats.cull_total_objects = m_culling.getLastTotalMeshObjects();
	ui.stats.cull_visible_objects = m_culling.getLastVisibleCount();
	uint32_t tri_count = 0;
	for (auto& vo : m_culling.getVisibleObjectsRef()) {
		auto* mesh = registry.getComponent<MeshComponent>(vo.entity);
		if (mesh)
			tri_count += mesh->getMesh()->getLodIndexCount(vo.lod_level) / 3;
	}
	ui.stats.visible_triangles = tri_count;
	ui.stats.visible_meshlets = 0;
}

} // namespace ve
