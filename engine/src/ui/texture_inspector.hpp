#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource_manager.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_raii.hpp>
#include <functional>
#include <string>
#include <vector>

namespace ve {

class VeImage;
class VeTexture;

class VENGINE_API TextureInspector {
public:
	TextureInspector() = default;
	~TextureInspector();

	TextureInspector(const TextureInspector&) = delete;
	TextureInspector& operator=(const TextureInspector&) = delete;
	TextureInspector(TextureInspector&&) = default;
	TextureInspector& operator=(TextureInspector&&) = default;

	struct ImageSource {
		std::function<const VeImage*()> image;
		std::function<uint32_t()> generation;
	};

	void open(const VeImage* image, const vk::raii::Sampler& sampler, const char* name);
	void open(ImageSource source, const vk::raii::Sampler& sampler, const char* name);
	void open(ResourceHandle<VeTexture> texture, const char* name);
	void close();
	void render();
	void invalidateCache();

	bool isOpen() const { return m_open; }

private:
	void rebuildCache();
	void clearCache();
	void flushPendingDeletions();

	bool m_open = false;
	std::string m_name;
	const VeImage* m_image = nullptr;
	const vk::raii::Sampler* m_sampler = nullptr;
	ImageSource m_source;
	uint32_t m_source_generation = 0;
	ResourceHandle<VeTexture> m_texture;

	int m_selected_layer = 0;
	int m_channel_mode = 0; // 0=RGB, 1=R, 2=G, 3=B, 4=A
	float m_exposure = 1.0f;
	uint32_t m_frame_counter = 0;

	struct LayerCache {
		vk::raii::ImageView image_view{nullptr};
		VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

		LayerCache() = default;
		LayerCache(LayerCache&&) = default;
		LayerCache& operator=(LayerCache&&) = default;
		LayerCache(const LayerCache&) = delete;
		LayerCache& operator=(const LayerCache&) = delete;
	};
	std::vector<LayerCache> m_layer_cache;

	struct PendingDeletion {
		std::vector<LayerCache> entries;
		uint32_t frame_queued = 0;
		ResourceHandle<VeTexture> texture; // outlives the views made from it
	};
	std::vector<PendingDeletion> m_pending_deletions;
};

} // namespace ve
