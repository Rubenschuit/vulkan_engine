#include "pch.hpp"
#include "rendering/ibl_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_buffer.hpp"
#include "utils/ve_log.hpp"

#include <fstream>
#include <sstream>
#include <regex>

namespace ve {

IblSystem::IblSystem(VeDevice& device, VeDescriptorPool& descriptor_pool,
                     VeResourceManager& resource_manager,
                     const std::filesystem::path& brdf_lut_path)
	: m_ve_device(device), m_resource_manager(resource_manager), m_descriptor_pool(descriptor_pool) {

	createSetLayout();
	createSamplers();
	createDummyResources();

	// Load shared BRDF LUT if available
	if (std::filesystem::exists(brdf_lut_path)) {
		m_brdf_lut_handle = m_resource_manager.load<VeTexture>(brdf_lut_path.lexically_normal().generic_string());
		VE_LOGI("IBL: loaded BRDF LUT from " << brdf_lut_path.generic_string());
	} else {
		VE_LOGW("IBL: BRDF LUT not found at " << brdf_lut_path.generic_string());
	}
}

IblSystem::~IblSystem() = default;

void IblSystem::createSetLayout() {
	m_ibl_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment) // prefiltered
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment) // BRDF LUT
		.build();
}

void IblSystem::createSamplers() {
	// Cubemap sampler: linear filter, clamp-to-edge, linear mipmap
	vk::SamplerCreateInfo cubemap_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eLinear,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = vk::False,
		.maxAnisotropy = 1.0f,
		.compareEnable = vk::False,
		.minLod = 0.0f,
		.maxLod = VK_LOD_CLAMP_NONE,
		.borderColor = vk::BorderColor::eFloatOpaqueBlack,
	};
	m_cubemap_sampler = vk::raii::Sampler(m_ve_device.getDevice(), cubemap_info);

	// BRDF LUT sampler: linear filter, clamp-to-edge, no mipmaps
	vk::SamplerCreateInfo brdf_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eNearest,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = vk::False,
		.maxAnisotropy = 1.0f,
		.compareEnable = vk::False,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = vk::BorderColor::eFloatOpaqueBlack,
	};
	m_brdf_lut_sampler = vk::raii::Sampler(m_ve_device.getDevice(), brdf_info);
}

void IblSystem::createDummyResources() {
	// 4x4 black cubemap (dummy prefiltered)
	m_dummy_cubemap = std::make_unique<VeImage>(
		m_ve_device, 4, 4,
		vk::SampleCountFlagBits::e1, vk::Format::eR8G8B8A8Unorm,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		true, 6, 1);

	m_dummy_cubemap->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);

	constexpr uint32_t face_bytes = 4 * 4 * 4; // 4x4 RGBA
	constexpr uint32_t total_bytes = face_bytes * 6;
	std::vector<uint8_t> black(total_bytes, 0);
	VeBuffer staging(m_ve_device, total_bytes, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	staging.map();
	staging.writeToBuffer(black.data(), total_bytes);
	m_ve_device.copyBufferToImage(staging.getBuffer(), m_dummy_cubemap->getImage(), 4, 4, 6);

	m_dummy_cubemap->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);

	// 4x4 black 2D texture (dummy BRDF LUT)
	m_dummy_2d = std::make_unique<VeImage>(
		m_ve_device, 4, 4,
		vk::SampleCountFlagBits::e1, vk::Format::eR8G8B8A8Unorm,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eColor,
		false, 1, 1);

	m_dummy_2d->transitionImageLayout(
		vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
		{}, vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::PipelineStageFlagBits2::eTransfer);

	VeBuffer staging2(m_ve_device, face_bytes, 1,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	staging2.map();
	staging2.writeToBuffer(black.data(), face_bytes);
	m_ve_device.copyBufferToImage(staging2.getBuffer(), m_dummy_2d->getImage(), 4, 4, 1);

	m_dummy_2d->transitionImageLayout(
		vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader);

	// Build dummy descriptor set
	writeDescriptorSet(m_dummy_descriptor_set,
		m_dummy_cubemap->getImageView(), m_dummy_2d->getImageView());
	m_has_dummy_set = true;
}

void IblSystem::writeDescriptorSet(vk::raii::DescriptorSet& set,
                                    const vk::raii::ImageView& prefiltered_view,
                                    const vk::raii::ImageView& brdf_lut_view) {
	vk::DescriptorImageInfo pref_info{
		.sampler = *m_cubemap_sampler,
		.imageView = *prefiltered_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	vk::DescriptorImageInfo brdf_info{
		.sampler = *m_brdf_lut_sampler,
		.imageView = *brdf_lut_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};

	auto writer = VeDescriptorWriter(*m_ibl_set_layout, m_descriptor_pool)
		.writeImage(0, &pref_info)
		.writeImage(1, &brdf_info);

	if (*set != vk::DescriptorSet{})
		writer.overwrite(set);
	else
		writer.build(set);
}

bool IblSystem::parseSHFile(const std::filesystem::path& sh_path) {
	std::ifstream file(sh_path);
	if (!file.is_open())
		return false;

	// Parse 9 lines of format: ( x, y, z); // comment
	std::string line;
	size_t idx = 0;
	while (std::getline(file, line) && idx < 9) {
		// Find the parenthesized triple
		auto open = line.find('(');
		auto close = line.find(')');
		if (open == std::string::npos || close == std::string::npos)
			continue;
		std::string values = line.substr(open + 1, close - open - 1);

		// Parse 3 comma-separated floats
		std::istringstream ss(values);
		float x, y, z;
		char comma;
		ss >> x >> comma >> y >> comma >> z;
		if (ss.fail())
			continue;

		m_sh_coefficients[idx] = glm::vec4(x, y, z, 0.0f);
		idx++;
	}

	if (idx != 9) {
		VE_LOGW("IBL: sh.txt has " << idx << " coefficients, expected 9");
		return false;
	}
	return true;
}

const vk::raii::DescriptorSetLayout& IblSystem::getIblSetLayout() const {
	return m_ibl_set_layout->getDescriptorSetLayout();
}

vk::raii::DescriptorSet& IblSystem::getOutputDescriptorSet(uint32_t frame_index) {
	if (m_ibl_available && m_has_active_sets)
		return m_active_descriptor_sets[frame_index];
	return m_dummy_descriptor_set;
}

void IblSystem::computeExposureCompensation() {
	const glm::vec3 l0(m_sh_coefficients[0]);
	float avg_luminance = glm::dot(l0, glm::vec3(0.2126f, 0.7152f, 0.0722f));

	constexpr float TARGET_LUMINANCE = 0.5f;
	constexpr float MAX_BOOST = 10.0f;
	constexpr float MIN_LUMINANCE = 0.001f;

	if (avg_luminance < MIN_LUMINANCE)
		m_exposure_compensation = MAX_BOOST;
	else if (avg_luminance < TARGET_LUMINANCE)
		m_exposure_compensation = glm::clamp(TARGET_LUMINANCE / avg_luminance, 1.0f, MAX_BOOST);
	else
		m_exposure_compensation = 1.0f;

	VE_LOGI("IBL: L0 luminance=" << avg_luminance << " exposure_compensation=" << m_exposure_compensation);
}

bool IblSystem::loadForSkybox(const std::filesystem::path& skybox_path) {
	// cmgen structure: textures/skybox/<name>/<name>_skybox.ktx
	// Companion files: <name>/<name>_ibl.ktx, <name>/sh.txt
	auto parent = skybox_path.parent_path();
	auto stem = skybox_path.stem().string();

	// Strip _skybox suffix if present to get the base name
	std::string base_name = stem;
	const std::string skybox_suffix = "_skybox";
	if (base_name.size() > skybox_suffix.size()
		&& base_name.compare(base_name.size() - skybox_suffix.size(), skybox_suffix.size(), skybox_suffix) == 0)
		base_name = base_name.substr(0, base_name.size() - skybox_suffix.size());

	auto ibl_path = parent / (base_name + "_ibl.ktx");
	auto sh_path = parent / "sh.txt";

	if (!std::filesystem::exists(ibl_path) || !std::filesystem::exists(sh_path)) {
		m_ibl_available = false;
		m_sh_coefficients = {};
		m_exposure_compensation = 1.0f;
		VE_LOGD("IBL: no companion files for " << skybox_path.filename().generic_string());
		return false;
	}

	// Parse spherical harmonics
	if (!parseSHFile(sh_path)) {
		m_ibl_available = false;
		m_sh_coefficients = {};
		m_exposure_compensation = 1.0f;
		return false;
	}

	computeExposureCompensation();

	// Load prefiltered specular cubemap
	m_prefiltered_handle = m_resource_manager.load<VeTexture>(
		ibl_path.lexically_normal().generic_string());

	m_prefiltered_mip_levels = m_prefiltered_handle.get()->getMipLevels();
	if (m_prefiltered_mip_levels == 0)
		m_prefiltered_mip_levels = 1;

	// use loaded LUT or dummy
	const vk::raii::ImageView& brdf_view = m_brdf_lut_handle.get()
		? m_brdf_lut_handle.get()->getImageView()
		: m_dummy_2d->getImageView();

	// Wait for all in-flight frames before overwriting descriptor sets.
	m_ve_device.getDevice().waitIdle();

	for (auto& set : m_active_descriptor_sets)
		writeDescriptorSet(set, m_prefiltered_handle.get()->getImageView(), brdf_view);
	m_has_active_sets = true;
	m_ibl_available = true;

	VE_LOGI("IBL: loaded SH + prefiltered for " << base_name
		<< " (" << m_prefiltered_mip_levels << " mip levels)");
	return true;
}

} // namespace ve