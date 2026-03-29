#pragma once
#include <vulkan/vulkan_raii.hpp>
#include <cstdint>

namespace ve {

class DepthPrePassSystem;
class GpuSceneManager;
class PbrMegaBuffer;
class PbrRenderSystem;
class ShadowRenderSystem;
class VeRenderer;
class Registry;
struct VeFrameInfo;
struct UIContext;

class CullingBackend {
public:
	virtual ~CullingBackend() = default;

	virtual void cull(VeFrameInfo& fi, GpuSceneManager& gpu_scene) = 0;

	virtual void renderDepthPrePass(VeFrameInfo& fi, PbrMegaBuffer& mega,
	                                DepthPrePassSystem& dps) const = 0;

	virtual void renderShadows(VeFrameInfo& fi, ShadowRenderSystem& srs,
	                           PbrMegaBuffer& mega, GpuSceneManager& gpu_scene) const = 0;

	virtual void renderOpaque(VeFrameInfo& fi, PbrRenderSystem& pbr,
	                          const vk::raii::DescriptorSet& bindless) const = 0;

	virtual void renderTransparency(VeFrameInfo& fi, PbrRenderSystem& pbr,
	                                const vk::raii::DescriptorSet& bindless,
	                                GpuSceneManager& gpu_scene,
	                                VeRenderer& renderer) const = 0;

	virtual vk::raii::DescriptorSet& getGlobalDescriptorSet(uint32_t frame) = 0;

	virtual void collectStats(uint32_t frame, UIContext& ui,
	                          Registry& registry) const = 0;

	virtual void setHizEnabled(bool enabled) = 0;
	virtual bool isHizEnabled() const = 0;
};

} // namespace ve