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
#include <unordered_map>

namespace ve {

struct DecodedTexture;
struct GpuCaps;
struct UploadContext;

enum class TextureType {
	ALBEDO,
	NORMAL,
	METALLIC_ROUGHNESS,
	OCCLUSION,
	EMISSIVE,
	SPECULAR,         // KHR_materials_specular: A = strength
	SPECULAR_COLOR,   // KHR_materials_specular: RGB = color tint
};

// Raw bytes for an image embedded in a .glb file
struct EmbeddedImageData {
	std::vector<uint8_t> bytes;
	bool is_ktx{};
};

using EmbeddedImageCache = std::unordered_map<std::string, EmbeddedImageData>;

class VENGINE_API VeTexture : public Resource {
public:
	// For ResourceManager: stores id, does not load. Call load() to load.
	VeTexture(VeDevice& device, const std::string& resource_id);

	// Constructs texture from generated pixel data
	VeTexture(VeDevice& device, uint32_t width, uint32_t height, TextureType type);

	// Constructs a texture from pre-decoded CPU-side data (loader worker output).
	// Performs GPU upload immediately; must be called on the main thread.
	VeTexture(VeDevice& device, const std::string& resource_id, const DecodedTexture& decoded);

	// Same, but records the upload into ctx instead of submit-and-wait. Caller
	// owns submit + sync.
	VeTexture(VeDevice& device, const std::string& resource_id, const DecodedTexture& decoded, UploadContext& ctx);

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
	
	// Resolve a filesystem path to a texture handle via the resource manager.
	static ResourceHandle<VeTexture> loadFromPath(VeResourceManager& resource_manager, const std::filesystem::path& path, TextureType fallback_type);

	// Resource-manager cache key used by loadFromPath. Same key is used to
	// register pre-decoded textures so subsequent loadFromPath calls hit the cache.
	static std::string makeResourceKey(const std::filesystem::path& path, TextureType type);

	// Worker-thread-safe decode. For glb-embedded images, pass the per-image
	// EmbeddedImageData
	static DecodedTexture decode(const std::filesystem::path& path, TextureType type,
	                             const GpuCaps& gpu_caps,
	                             const EmbeddedImageData* embedded = nullptr);


	//debug
	void printDebugInfo() const {
		m_texture_image->printDebugInfo();
	}

protected:
	bool doLoad() override;
	void doUnload() override;
	void emitUnloadingEvent(EventBus& bus) override;

private:
	bool recordDecodedTexture(const DecodedTexture& decoded, UploadContext& ctx);
	void createTextureSampler();
	static stbi_uc* generateDefaultTexture(int width, int height, TextureType type);

	ve::VeDevice& m_ve_device;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	std::unique_ptr<ve::VeImage> m_texture_image;
	std::optional<vk::raii::Sampler> m_texture_sampler;
};
} // namespace ve