#pragma once
#include "rendering/culling/culling_backend.hpp"

#include <cstdint>

namespace ve {

class GpuCullingSystem;
class GpuSceneManager;
class MeshletCullingSystem;

class MeshletCullingBackend final : public CullingBackend {
public:
	MeshletCullingBackend(MeshletCullingSystem& meshlet, GpuCullingSystem* gpu_cull,
	                      GpuSceneManager& gpu_scene);

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
	void collectStats(uint32_t frame, FrameStats& stats,
	                  Registry& registry) const override;
	void setHizEnabled(bool enabled) override;
	bool isHizEnabled() const override;

	// Attach/detach the GpuCullingSystem at runtime. Used for a non-meshlet
	// shadow fallback
	void setGpuCull(GpuCullingSystem* gpu_cull) { m_gpu_cull = gpu_cull; }

private:
	MeshletCullingSystem& m_meshlet_cull_system;
	GpuCullingSystem* m_gpu_cull = nullptr;
	GpuSceneManager& m_gpu_scene;
};

} // namespace ve
