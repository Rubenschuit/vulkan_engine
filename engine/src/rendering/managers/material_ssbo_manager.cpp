#include "pch.hpp"
#include "rendering/managers/material_ssbo_manager.hpp"
#include "rendering/managers/bindless_texture_registry.hpp"
#include "resources/ve_material.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "utils/ve_log.hpp"

namespace ve {

MaterialSSBOManager::MaterialSSBOManager(VeDevice& device, BindlessTextureRegistry& texture_registry, EventBus& event_bus)
	: m_ve_device(device), m_texture_registry(texture_registry), m_event_bus(event_bus) {

	m_buffer = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(MaterialGPU), MAX_GPU_MATERIALS,
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
		vk::MemoryPropertyFlagBits::eDeviceLocal);

	m_staging_buffer = std::make_unique<VeBuffer>(m_ve_device,
		sizeof(MaterialGPU), MAX_GPU_MATERIALS,
		vk::BufferUsageFlagBits::eTransferSrc,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	m_staging_buffer->map();
}

MaterialSSBOManager::~MaterialSSBOManager() = default;

uint32_t MaterialSSBOManager::registerMaterial(VeMaterial* mat) {
	auto it = m_material_to_index.find(mat);
	if (it != m_material_to_index.end())
		return it->second;

	uint32_t index;
	if (!m_free_list.empty()) {
		index = m_free_list.back();
		m_free_list.pop_back();
	} else {
		assert(m_next_index < MAX_GPU_MATERIALS && "Material SSBO full");
		index = m_next_index++;
	}

	m_material_to_index[mat] = index;
	writeMaterialGPU(index, mat);
	return index;
}

void MaterialSSBOManager::updateMaterial(uint32_t index, VeMaterial* mat) {
	writeMaterialGPU(index, mat);
}

void MaterialSSBOManager::flushToDevice(const vk::raii::CommandBuffer& cmd) {
	if (!m_dirty || m_next_index == 0)
		return;

	vk::DeviceSize size = static_cast<vk::DeviceSize>(m_next_index) * sizeof(MaterialGPU);
	cmd.copyBuffer(m_staging_buffer->getBuffer(), m_buffer->getBuffer(),
		vk::BufferCopy{0, 0, size});

	vk::BufferMemoryBarrier2 barrier{
		.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
		.srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eVertexShader
			| vk::PipelineStageFlagBits2::eFragmentShader
			| vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = m_buffer->getBuffer(),
		.offset = 0,
		.size = size,
	};
	vk::DependencyInfo dep{.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &barrier};
	cmd.pipelineBarrier2(dep);

	m_dirty = false;
}

void MaterialSSBOManager::reset() {
	m_material_to_index.clear();
	m_free_list.clear();
	m_next_index = 0;
}

void MaterialSSBOManager::unregisterMaterial(VeMaterial* mat) {
	auto it = m_material_to_index.find(mat);
	if (it == m_material_to_index.end())
		return;
	m_free_list.push_back(it->second);
	m_material_to_index.erase(it);
}

void MaterialSSBOManager::writeMaterialGPU(uint32_t index, VeMaterial* mat) {
	auto factors = mat->getMaterialFactors();
	auto alpha_props = mat->getAlphaProps();

	auto getIdx = [&](const ResourceHandle<VeTexture>& tex, TextureType fallback) -> uint32_t {
		if (tex.isValid())
			return m_texture_registry.registerTexture(tex.get());
		return m_texture_registry.getDefaultIndex(fallback);
	};

	bool has_texture = mat->getAlbedoTexture().isValid();
	uint32_t flags = static_cast<uint32_t>(alpha_props.alpha_mode)
		| (alpha_props.double_sided ? MaterialFlag::DOUBLE_SIDED : 0u)
		| (mat->getFlipTexCoordV() ? MaterialFlag::FLIP_TEX_V : 0u)
		| (alpha_props.use_spec_gloss_texture ? MaterialFlag::SPEC_GLOSS : 0u)
		| (has_texture ? MaterialFlag::HAS_TEXTURE : 0u);

	MaterialGPU gpu{
		.base_color_factor = factors.base_color_factor,
		.emissive_factor = glm::vec4(factors.emissive_factor, 0.0f),
		.pbr_params = glm::vec4(factors.metallic_factor, factors.roughness_factor,
		                        factors.emissive_strength, factors.transmission_factor),
		.specular_color_ior = glm::vec4(factors.specular_factor, factors.ior),
		.albedo_index = getIdx(mat->getAlbedoTexture(), TextureType::ALBEDO),
		.normal_index = getIdx(mat->getNormalTexture(), TextureType::NORMAL),
		.metallic_roughness_index = getIdx(mat->getMetallicRoughnessTexture(), TextureType::METALLIC_ROUGHNESS),
		.occlusion_index = getIdx(mat->getOcclusionTexture(), TextureType::OCCLUSION),
		.emissive_index = getIdx(mat->getEmissiveTexture(), TextureType::EMISSIVE),
		.material_flags = flags,
		.alpha_cutoff = (alpha_props.alpha_mode == AlphaMode::MASK) ? alpha_props.alpha_cutoff : 0.0f,
		.specular_index = getIdx(mat->getSpecularTexture(), TextureType::SPECULAR),
		.specular_color_index = getIdx(mat->getSpecularColorTexture(), TextureType::SPECULAR_COLOR),
		.specular_strength = factors.specular_strength,
		._pad0 = 0,
		._pad1 = 0,
	};

	m_staging_buffer->writeToBuffer(&gpu, sizeof(MaterialGPU),
		static_cast<vk::DeviceSize>(index) * sizeof(MaterialGPU));
	m_dirty = true;
	m_event_bus.enqueue(MaterialDataChangedEvent{index});
}

} // namespace ve
