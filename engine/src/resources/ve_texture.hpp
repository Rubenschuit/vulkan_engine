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

enum class TextureType {
	ALBEDO,
	NORMAL,
	METALLIC_ROUGHNESS,
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
	vk::DescriptorImageInfo getDescriptorInfo() const;

	// Create a depth compare sampler for shadow maps
	static vk::raii::Sampler createDepthCompareSampler(VeDevice& device);

	// Create a default texture (4x4 placeholder for albedo/normal/metallic-roughness)
	static std::shared_ptr<VeTexture> createDefault(VeDevice& device, TextureType type);
	// Load texture via ResourceManager; returns default if path is placeholder or missing
	static ResourceHandle<VeTexture> loadOrDefault(VeResourceManager& resource_manager, const std::filesystem::path& path, TextureType fallback_type, vk::Format format);

	// For createDefault - constructs texture from generated pixel data
	VeTexture(VeDevice& device, uint32_t width, uint32_t height, TextureType type);

	//debug
	void printDebugInfo() const {
		m_texture_image->printDebugInfo();
	}

protected:
	bool doLoad() override;
	void doUnload() override;

private:
	bool createTextureImage(const std::filesystem::path& texture_path, vk::Format format_hint = vk::Format::eUndefined);
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