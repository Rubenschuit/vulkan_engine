/*  Watches RenderSettings for changes that the renderer can't apply per-frame
*  (require pipeline recreation, descriptor rebuild, or device wait-idle) and
*  emits the corresponding event.
* 
*  Call tick() each frame, diffs current settings against cached, emits events.
*/
#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"

namespace ve {

class VeDevice;
class VeRenderer;
class EventBus;
struct RenderSettings;
class RenderResources;


class VENGINE_API SettingsWatcher {
public:
	SettingsWatcher(VeDevice& device, VeRenderer& renderer,
	                EventBus& event_bus, const RenderSettings& settings,
	                RenderResources& resources);

	SettingsWatcher(const SettingsWatcher&) = delete;
	SettingsWatcher& operator=(const SettingsWatcher&) = delete;

	void tick();

private:
	void seed();

	VeDevice& m_ve_device;
	VeRenderer& m_ve_renderer;
	EventBus& m_event_bus;
	const RenderSettings& m_settings;
	RenderResources& m_resources;

	bool m_shadow_mask_half_res = false;
	bool m_gtao_half_res = true;
	ShadowResolutionPreset m_shadow_resolution_preset = ShadowResolutionPreset::MEDIUM;
	int m_pcf_samples = 8;
	int m_pcss_filter_samples = 16;
	float m_depth_bias_constant = ve::SHADOW_DEPTH_BIAS_CONSTANT;
	float m_depth_bias_slope = ve::SHADOW_DEPTH_BIAS_SLOPE;
	float m_depth_bias_clamp = ve::SHADOW_DEPTH_BIAS_CLAMP;
	ShadowCullMode m_shadow_cull_mode = ShadowCullMode::Front;
	Topology m_last_topology = Topology::TRIANGLE_LIST;
};

} // namespace ve