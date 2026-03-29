#pragma once
#include "rendering/culling/culling_backend.hpp"

#include <cstdint>

namespace ve {

class EventBus;
class GpuCullingSystem;
class GpuSceneManager;
class MeshletCullingSystem;

class MeshletCullingBackend final : public CullingBackend {
public:
	MeshletCullingBackend(MeshletCullingSystem& meshlet, GpuCullingSystem& gpu_cull,
	                      GpuSceneManager& gpu_scene, EventBus& event_bus);

	void cull(VeFrameInfo& fi, GpuSceneManager& gpu_scene) override;
	void renderDepthPrePass(VeFrameInfo& fi, PbrMegaBuffer& mega,
	                        DepthPrePassSystem& dps) const override;
	void renderShadows(VeFrameInfo& fi, ShadowRenderSystem& srs,
	                   PbrMegaBuffer& mega, GpuSceneManager& gpu_scene) const override;
	void renderOpaque(VeFrameInfo& fi, PbrRenderSystem& pbr,
	                  const vk::raii::DescriptorSet& bindless) const override;
	void renderTransparency(VeFrameInfo& fi, PbrRenderSystem& pbr,
	                        const vk::raii::DescriptorSet& bindless,
	                        GpuSceneManager& gpu_scene,
	                        VeRenderer& renderer) const override;
	vk::raii::DescriptorSet& getGlobalDescriptorSet(uint32_t frame) override;
	void collectStats(uint32_t frame, UIContext& ui,
	                  Registry& registry) const override;
	void setHizEnabled(bool enabled) override;
	bool isHizEnabled() const override;

private:
	MeshletCullingSystem& m_meshlet_cull_system;
	GpuCullingSystem& m_gpu_cull;
	GpuSceneManager& m_gpu_scene;
	bool m_gpu_shadow_fallback = true;
};

} // namespace ve
