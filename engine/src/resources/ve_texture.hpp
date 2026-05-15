/* VeTexture - loads texture images and creates Vulkan image resources.
 * Inherits from Resource for use with VeResourceManager.
 * Loads .ktx, .ktx2 using ktx.h and .png, .jpg, etc using stb_image.h.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_resource.hpp"
#include "resources/ve_resource_manager.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"

#include <stb_image.h>
#include <filesystem>
#include <memory>

namespace ve {

struct DecodedTexture;

enum class TextureType {
	ALBEDO,
	NORMAL,
	METALLIC_ROUGHNESS,
	OCCLUSION,
	EMISSIVE,
	SPECULAR,         // KHR_materials_specular: A = strength
	SPECULAR_COLOR,   // KHR_materials_specular: RGB = color tint
};

// Holds decoded pixel data or raw KTX bytes for images embedded in .glb files.
struct EmbeddedImageData {
	std::vector<uint8_t> pixels; // decoded RGBA or raw KTX bytes
	uint32_t width{};
	uint32_t height{};
	bool is_ktx{};
};

class VENGINE_API VeTexture : public Resource {
public:
	// For ResourceManager: stores id, does not load. Call load() to load.
	VeTexture(VeDevice& device, const std::string& resource_id);
	~VeTexture() override;

	VeTexture(const VeTexture&) = delete;
	VeTexture& operator=(const VeTexture&) = delete;

	const vk::raii::Sampler& getSampler() const;
	const vk::raii::ImageView& getImageView() const { return m_texture_image->getImageView(); };
	const VeImage* getImage() const { return m_texture_image.get(); }
	vk::DescriptorImageInfo getDescriptorInfo() const;

	uint32_t getWidth() const { return m_width; }
	uint32_t getHeight() const { return m_height; }
	uint32_t getMipLevels() const { return m_texture_image ? m_texture_image->getMipLevels() : 0; }
	vk::Format getFormat() const { return m_texture_image ? m_texture_image->getFormat() : vk::Format::eUndefined; }

	// Create a depth comparison sampler for shadow maps
	static vk::raii::Sampler createDepthCompareSampler(VeDevice& device);
	// Create a regular (non-comparison) sampler for raw depth reads (PCSS blocker search)
	static vk::raii::Sampler createShadowRawSampler(VeDevice& device);

	// Create a default texture (4x4 placeholder for albedo/normal/metallic-roughness)
	static std::shared_ptr<VeTexture> createDefault(VeDevice& device, TextureType type);
	// Load texture via ResourceManager; returns default if path is placeholder or missing
	static ResourceHandle<VeTexture> loadOrDefault(VeResourceManager& resource_manager, const std::filesystem::path& path, TextureType fallback_type);

	// Embedded image cache for .glb support
	static void registerEmbedded(const std::string& key, EmbeddedImageData data);
	static bool hasEmbedded(const std::string& key);
	static void clearEmbeddedCache();

	// For createDefault - constructs texture from generated pixel data
	VeTexture(VeDevice& device, uint32_t width, uint32_t height, TextureType type);

	//debug
	void printDebugInfo() const {
		m_texture_image->printDebugInfo();
	}

protected:
	bool doLoad() override;
	void doUnload() override;
	void emitUnloadingEvent(EventBus& bus) override;

private:
	bool createTextureImage(const std::filesystem::path& texture_path, vk::Format format_hint = vk::Format::eUndefined);
	bool createTextureImageFromKtxMemory(const uint8_t* data, size_t size, vk::Format format_hint = vk::Format::eUndefined);
	bool uploadKtxTexture(void* k_texture_ptr, vk::Format format_hint);
	bool createTextureImageSTB(const std::filesystem::path& texture_path, vk::Format format = vk::Format::eR8G8B8A8Srgb);
	void createTextureImageFromPixels(uint32_t width, uint32_t height, const stbi_uc* pixels, vk::Format format);
	void createTextureSampler();
	stbi_uc* generateDefaultTexture(int width, int height, TextureType type);

	ve::VeDevice& m_ve_device;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	std::unique_ptr<ve::VeImage> m_texture_image;
	std::optional<vk::raii::Sampler> m_texture_sampler;
};
} // namespace ve