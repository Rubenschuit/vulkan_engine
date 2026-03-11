#pragma once
#include "rendering/culling_backend.hpp"

#include <cstdint>

namespace ve {

class GpuCullingSystem;
class MeshletCullingSystem;

class MeshletCullingBackend final : public CullingBackend {
public:
	MeshletCullingBackend(MeshletCullingSystem& meshlet, GpuCullingSystem& gpu_cull);

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

	void setGpuShadowFallback(bool enabled) { m_gpu_shadow_fallback = enabled; }

private:
	MeshletCullingSystem& m_meshlet_cull_system;
	GpuCullingSystem& m_gpu_cull;
	bool m_gpu_shadow_fallback = false;
};

} // namespace ve
