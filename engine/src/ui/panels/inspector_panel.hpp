#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.h>

namespace ve {

class TransformComponent;
class MeshComponent;
class PointLightComponent;
class DirectionalLightComponent;
class VeTexture;

class VENGINE_API InspectorPanel : public EditorPanel {
public:
	~InspectorPanel();

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Inspector"; }

private:
	void renderEntityHeader(Registry& registry, Entity entity);
	void renderTransform(TransformComponent& transform);
	void renderMesh(MeshComponent& mesh);
	void renderPointLight(PointLightComponent& light);
	void renderDirectionalLight(DirectionalLightComponent& light);

	// Texture thumbnail cache
	struct TextureCacheEntry {
		VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
		uint32_t last_used_frame = 0;
	};
	std::unordered_map<std::string, TextureCacheEntry> m_texture_cache;
	uint32_t m_frame_counter = 0;
	Registry* m_last_registry = nullptr;
	static constexpr uint32_t MAX_CACHED_TEXTURES = 32;
	static constexpr uint32_t TEXTURE_CACHE_TTL = 300;

	VkDescriptorSet getOrCreateTextureDescriptor(const std::string& id, const VeTexture* texture);
	void evictStaleTextures();
	void clearTextureCache();
	void renderTextureSlot(const char* label, const std::string& id, const VeTexture* texture, float thumb_size = 48.0f);
};

} // namespace ve
