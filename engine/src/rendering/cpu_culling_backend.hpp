#pragma once
#include "rendering/culling_backend.hpp"

#include <cstdint>
#include <vector>

namespace vk::raii {
class DescriptorSet;
}

namespace ve {

class CullingSystem;
class MaterialSSBOManager;
class VeThreadPool;
struct UIContext;

class CpuCullingBackend final : public CullingBackend {
public:
	CpuCullingBackend(CullingSystem& culling, PbrRenderSystem& pbr,
	                  MaterialSSBOManager& mat_mgr, UIContext& ui,
	                  std::vector<vk::raii::DescriptorSet>& global_sets,
	                  VeThreadPool& thread_pool);

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
	void setHizEnabled(bool) override {}
	bool isHizEnabled() const override { return false; }

private:
	CullingSystem& m_culling;
	PbrRenderSystem& m_pbr;
	MaterialSSBOManager& m_mat_mgr;
	UIContext& m_ui;
	std::vector<vk::raii::DescriptorSet>& m_global_sets;
	VeThreadPool& m_thread_pool;
};

} // namespace ve
