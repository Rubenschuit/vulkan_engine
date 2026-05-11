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
class SpotLightComponent;
class RigidbodyComponent;
class AnimatorComponent;
class SkinComponent;
class VeTexture;
class TextureInspector;

class VENGINE_API InspectorPanel : public EditorPanel {
public:
	~InspectorPanel();

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Entity Inspector"; }

	void setTextureInspector(TextureInspector* inspector) { m_texture_inspector = inspector; }

private:
	void renderEntityHeader(Registry& registry, Entity entity, EditorState& state);
	void renderTransform(TransformComponent& transform);
	void renderMesh(MeshComponent& mesh);
	void renderPointLight(PointLightComponent& light);
	void renderDirectionalLight(DirectionalLightComponent& light);
	void renderSpotLight(SpotLightComponent& light);
	void renderRigidbody(RigidbodyComponent& rb, EditorState& state);
	void renderAnimator(AnimatorComponent& animator);
	void renderSkin(Registry& registry, SkinComponent& skin, EditorState& state);

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

	TextureInspector* m_texture_inspector = nullptr;
};

} // namespace ve
