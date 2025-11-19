/* The VeTexture class is responsible for loading texture images
   and creating Vulkan image resources. Loads .ktx, .ktx2 using ktx.h and
   .png, .jpg, etc using stb_image.h.
   Has overloaded constructors for single texture and textures array.*/
#pragma once
#include "ve_export.hpp"
#include "ve_device.hpp"
#include "ve_image.hpp"

#include <stb_image.h>
#include <filesystem>

namespace ve {

enum class TextureType {
	ALBEDO,
	NORMAL,
	METALLIC_ROUGHNESS,
};

class VENGINE_API VeTexture {
public:
    // Creates a 2Dtexture from a single file, assumes format is RGBA8Srgb
	VeTexture(ve::VeDevice& device, const std::filesystem::path& texture_path);
	// Creates a 2D texture array from an array of files
	VeTexture(ve::VeDevice& device, const std::vector<std::filesystem::path>& texture_paths, vk::Format format);
	~VeTexture();

	VeTexture(const VeTexture&) = delete;
	VeTexture& operator=(const VeTexture&) = delete;

	const vk::raii::Sampler& getSampler() const { return m_texture_sampler; };
	const vk::raii::ImageView& getImageView() const { return m_texture_image->getImageView(); };
	vk::DescriptorImageInfo getDescriptorInfo() const;

	// Create a depth compare sampler for shadow maps
	static vk::raii::Sampler createDepthCompareSampler(VeDevice& device);

	//debug
	void printDebugInfo() const {
		m_texture_image->printDebugInfo();
	}

private:
	void createTextureImage(const std::filesystem::path& texture_path);
	void createTextureImageSTB(const std::filesystem::path& texture_path);
	void createTextureImageArraySTB(const std::vector<std::filesystem::path>& texture_paths, vk::Format format);
	void createTextureSampler();
	stbi_uc* generateDefaultTexture(int width, int height, TextureType type);

	ve::VeDevice& m_ve_device;
	uint32_t m_width;
	uint32_t m_height;
	std::unique_ptr<ve::VeImage> m_texture_image;

	// Todo:: move sampler outside of texture class if we want to sample multiple images
	vk::raii::Sampler m_texture_sampler{nullptr};
};
} // namespace ve