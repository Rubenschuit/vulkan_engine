#pragma once
#include "rendering/culling/culling_backend.hpp"

#include <cstdint>

namespace ve {

class GpuCullingSystem;
class GpuSceneManager;
class VeBuffer;

class GpuCullingBackend final : public CullingBackend {
public:
	GpuCullingBackend(GpuCullingSystem& cull_system, GpuSceneManager& gpu_scene);

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

	// Compaction-aware buffer source (eliminates repeated compaction checks)
	struct IndirectDrawSource {
		const VeBuffer& indirect;
		const uint32_t* bucket_offsets;
		const uint32_t* bucket_counts;
		const VeBuffer* compact_indirect;
		const VeBuffer* compact_counts;
	};
	IndirectDrawSource getDrawSource(uint32_t frame) const;

private:
	GpuCullingSystem& m_cull;
	GpuSceneManager& m_gpu_scene;
};

} // namespace ve
