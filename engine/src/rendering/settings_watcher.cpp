#include "pch.hpp"
#include "rendering/settings_watcher.hpp"

#include "events/engine_events.hpp"
#include "events/render_events.hpp"
#include "events/event_bus.hpp"
#include "rendering/render_resources.hpp"
#include "rendering/render_settings.hpp"
#include "rendering/ve_renderer.hpp"
#include "vulkan/ve_device.hpp"

namespace ve {

static vk::Extent2D halveExtent(vk::Extent2D e, bool half) {
	if (!half)
		return e;
	return {std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
}

SettingsWatcher::SettingsWatcher(VeDevice& device, VeRenderer& renderer,
                                 EventBus& event_bus, const RenderSettings& settings,
                                 RenderResources& resources)
	: m_ve_device(device),
	  m_ve_renderer(renderer),
	  m_event_bus(event_bus),
	  m_settings(settings),
	  m_resources(resources) {
	seed();
}

void SettingsWatcher::seed() {
	m_shadow_mask_half_res = m_settings.shadow_mask_half_res;
	m_gtao_half_res = m_settings.gtao_half_res;
	m_shadow_resolution_preset = m_settings.shadow_resolution_preset;
	m_pcf_samples = m_settings.pcf_samples;
	m_pcss_filter_samples = m_settings.pcss_filter_samples;
	m_depth_bias_constant = m_settings.depth_bias_constant;
	m_depth_bias_slope = m_settings.depth_bias_slope;
	m_depth_bias_clamp = m_settings.depth_bias_clamp;
	m_shadow_cull_mode = m_settings.shadow_cull_mode;
	m_last_topology = m_settings.topology;
}

void SettingsWatcher::tick() {
	auto extent = m_ve_renderer.getExtent();

	bool depth_bias_changed = m_settings.depth_bias_constant != m_depth_bias_constant
		|| m_settings.depth_bias_slope != m_depth_bias_slope
		|| m_settings.depth_bias_clamp != m_depth_bias_clamp
		|| m_settings.shadow_cull_mode != m_shadow_cull_mode;
	bool topology_changed = m_settings.topology != m_last_topology;
	bool samples_changed = m_settings.pcf_samples != m_pcf_samples
		|| m_settings.pcss_filter_samples != m_pcss_filter_samples;
	bool shadow_mask_res_changed = m_settings.shadow_mask_half_res != m_shadow_mask_half_res;
	bool gtao_res_changed = m_settings.gtao_half_res != m_gtao_half_res;
	bool shadow_atlas_res_changed = m_settings.shadow_resolution_preset != m_shadow_resolution_preset;

	if (samples_changed || shadow_mask_res_changed || gtao_res_changed || shadow_atlas_res_changed)
		m_ve_device.getDevice().waitIdle();

	if (depth_bias_changed) {
		m_depth_bias_constant = m_settings.depth_bias_constant;
		m_depth_bias_slope = m_settings.depth_bias_slope;
		m_depth_bias_clamp = m_settings.depth_bias_clamp;
		m_shadow_cull_mode = m_settings.shadow_cull_mode;
		m_event_bus.emitImmediate(DepthBiasChangedEvent{});
	}

	if (topology_changed) {
		m_last_topology = m_settings.topology;
		m_event_bus.emitImmediate(TopologyChangedEvent{.topology = m_settings.topology});
	}

	if (samples_changed) {
		m_pcf_samples = m_settings.pcf_samples;
		m_pcss_filter_samples = m_settings.pcss_filter_samples;
		m_event_bus.emitImmediate(ShadowSamplesChangedEvent{
			.pcf_samples = static_cast<uint32_t>(m_pcf_samples),
			.pcss_filter_samples = static_cast<uint32_t>(m_pcss_filter_samples)
		});
	}

	if (shadow_mask_res_changed) {
		m_shadow_mask_half_res = m_settings.shadow_mask_half_res;
		m_event_bus.emitImmediate(ShadowMaskResolutionChangedEvent{
			.pool = *m_resources.pool(),
			.mask_extent = halveExtent(extent, m_shadow_mask_half_res),
			.depth_extent = extent,
			.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
			.depth_image = m_ve_renderer.getResolvedDepthImage()
		});
	}

	if (gtao_res_changed) {
		m_gtao_half_res = m_settings.gtao_half_res;
		m_event_bus.emitImmediate(GtaoResolutionChangedEvent{
			.pool = *m_resources.pool(),
			.ao_extent = halveExtent(extent, m_gtao_half_res),
			.depth_extent = extent,
			.depth_image_view = m_ve_renderer.getResolvedDepthImageView(),
			.depth_image = m_ve_renderer.getResolvedDepthImage()
		});
	}

	if (shadow_atlas_res_changed) {
		m_shadow_resolution_preset = m_settings.shadow_resolution_preset;
		m_event_bus.emitImmediate(ShadowAtlasResolutionChangedEvent{
			.pool = *m_resources.pool(),
			.preset = m_shadow_resolution_preset
		});
	}
}

} // namespace ve
