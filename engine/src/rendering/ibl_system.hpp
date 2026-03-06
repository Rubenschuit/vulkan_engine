#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_texture.hpp"
#include <memory>
#include <array>
#include <filesystem>

namespace ve {
class VeDevice;
class VeDescriptorPool;
class VeDescriptorSetLayout;
}

namespace ve {

class VENGINE_API IblSystem {
public:
	IblSystem(VeDevice& device, VeDescriptorPool& descriptor_pool,
	          VeResourceManager& resource_manager,
	          const std::filesystem::path& brdf_lut_path);
	~IblSystem();

	IblSystem(const IblSystem&) = delete;
	IblSystem& operator=(const IblSystem&) = delete;

	// Load IBL data for a skybox path. Looks for companion _ibl.ktx and sh.txt.
	bool loadForSkybox(const std::filesystem::path& skybox_path);

	bool isAvailable() const { return m_ibl_available; }

	const vk::raii::DescriptorSetLayout& getIblSetLayout() const;

	// Returns active IBL set when available, dummy (black) otherwise
	vk::raii::DescriptorSet& getOutputDescriptorSet(uint32_t frame_index);

	uint32_t getPrefilteredMipLevels() const { return m_prefiltered_mip_levels; }

	// L2 spherical harmonics coefficients (9 vec4, xyz = rgb, w unused)
	const std::array<glm::vec4, 9>& getSHCoefficients() const { return m_sh_coefficients; }

private:
	void createSetLayout();
	void createSamplers();
	void createDummyResources();
	void writeDescriptorSet(vk::raii::DescriptorSet& set,
	                        const vk::raii::ImageView& prefiltered_view,
	                        const vk::raii::ImageView& brdf_lut_view);
	bool parseSHFile(const std::filesystem::path& sh_path);

	VeDevice& m_ve_device;
	VeResourceManager& m_resource_manager;
	VeDescriptorPool& m_descriptor_pool;

	std::unique_ptr<VeDescriptorSetLayout> m_ibl_set_layout;
	vk::raii::Sampler m_cubemap_sampler{nullptr};
	vk::raii::Sampler m_brdf_lut_sampler{nullptr};

	ResourceHandle<VeTexture> m_prefiltered_handle;
	ResourceHandle<VeTexture> m_brdf_lut_handle;

	// Dummy resources for valid descriptors when IBL unavailable
	std::unique_ptr<VeImage> m_dummy_cubemap;
	std::unique_ptr<VeImage> m_dummy_2d;

	std::array<vk::raii::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_active_descriptor_sets = makeNullArray<vk::raii::DescriptorSet>();
	vk::raii::DescriptorSet m_dummy_descriptor_set{nullptr};
	bool m_has_active_sets = false;
	bool m_has_dummy_set = false;

	bool m_ibl_available = false;
	uint32_t m_prefiltered_mip_levels = 1;
	std::array<glm::vec4, 9> m_sh_coefficients{};
};

} // namespace ve