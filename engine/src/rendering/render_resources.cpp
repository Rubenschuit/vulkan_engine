#include "pch.hpp"
#include "rendering/render_resources.hpp"

#include "application/ve_engine_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_material_properties.hpp"
#include "scene/ve_scene.hpp"
#include "utils/ve_log.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_device.hpp"

#include <cassert>

namespace ve {

RenderResources::RenderResources(VeDevice& device,
                                 VeResourceManager& resource_manager,
                                 const EngineConfig& config)
	: m_ve_device(device),
	  m_resource_manager(resource_manager),
	  m_config(config) {
	createBuffers();
	createDescriptors();
}

RenderResources::~RenderResources() = default;

void RenderResources::createBuffers() {
	VE_LOGD("Creating buffers");
	vk::DeviceSize buffer_size = sizeof(UniformBufferObject);
	assert(buffer_size > 0 && "Uniform buffer size is zero");
	assert(buffer_size <= m_ve_device.getDeviceProperties().limits.maxUniformBufferRange && "Uniform buffer size exceeds maximum limit");

	m_uniform_buffers.clear();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_uniform_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device, buffer_size, 1,
			vk::BufferUsageFlagBits::eUniformBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
		));
		m_uniform_buffers[i]->map();
	}

	m_instance_buffers.clear();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_instance_buffers.emplace_back(std::make_unique<VeBuffer>(
			m_ve_device, sizeof(InstanceData), INITIAL_INSTANCE_CAPACITY,
			vk::BufferUsageFlagBits::eStorageBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
		));
		m_instance_buffers[i]->map();
	}
}

void RenderResources::createDescriptors() {
	VE_LOGD("Creating descriptors");

	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment)
		.build();

	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(3, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(4, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(5, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eFragment)
		.build();

	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(1024)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 1024)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 4096)
		.addPoolSize(vk::DescriptorType::eSampler, 256)
		.addPoolSize(vk::DescriptorType::eSampledImage, 256)
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 1024)
		.addPoolSize(vk::DescriptorType::eStorageImage, 128)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.buildShared();

	m_default_material_ubo = std::make_unique<VeBuffer>(m_ve_device, MATERIAL_UBO_SIZE, 1,
		vk::BufferUsageFlagBits::eUniformBuffer,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	MaterialFactors defaults{};
	float default_ubo[16];
	writeMaterialUBO(default_ubo, defaults);
	m_default_material_ubo->map();
	m_default_material_ubo->writeToBuffer(default_ubo, MATERIAL_UBO_SIZE);
	m_default_material_ubo->unmap();
	auto default_material_ubo_info = m_default_material_ubo->getDescriptorInfo();

	m_particle_texture_handle = m_resource_manager.load<VeTexture>(m_config.particle_assets.glow.lexically_normal().generic_string());
	m_fire_texture_handle = m_resource_manager.load<VeTexture>(m_config.particle_assets.fire.lexically_normal().generic_string());
	m_smoke_texture_handle = m_resource_manager.load<VeTexture>(m_config.particle_assets.smoke.lexically_normal().generic_string());

	m_default_albedo_handle = m_resource_manager.load<VeTexture>("default_albedo");
	m_default_normal_handle = m_resource_manager.load<VeTexture>("default_normal");
	m_default_mr_handle = m_resource_manager.load<VeTexture>("default_metallic_roughness");
	m_default_occlusion_handle = m_resource_manager.load<VeTexture>("default_occlusion");
	m_default_emissive_handle = m_resource_manager.load<VeTexture>("default_emissive");
	auto default_albedo_info = m_default_albedo_handle.get()->getDescriptorInfo();
	auto default_normal_info = m_default_normal_handle.get()->getDescriptorInfo();
	auto default_mr_info = m_default_mr_handle.get()->getDescriptorInfo();
	auto default_occlusion_info = m_default_occlusion_handle.get()->getDescriptorInfo();
	auto default_emissive_info = m_default_emissive_handle.get()->getDescriptorInfo();
	m_default_material_descriptor_set = vk::raii::DescriptorSet{nullptr};
	VeDescriptorWriter(*m_material_set_layout, *m_global_pool)
		.writeImage(0, &default_albedo_info)
		.writeImage(1, &default_normal_info)
		.writeImage(2, &default_mr_info)
		.writeImage(3, &default_occlusion_info)
		.writeImage(4, &default_emissive_info)
		.writeBuffer(5, &default_material_ubo_info)
		.build(m_default_material_descriptor_set);
}

void RenderResources::bindMaterialSsbo(const VeBuffer& material_ssbo) {
	m_global_descriptor_sets.clear();
	m_global_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);
	auto material_ssbo_info = material_ssbo.getDescriptorInfo();
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		auto buffer_info = m_uniform_buffers[i]->getDescriptorInfo();
		auto instance_info = m_instance_buffers[i]->getDescriptorInfo();
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_global_set_layout, *m_global_pool)
			.writeBuffer(0, &buffer_info)
			.writeBuffer(1, &instance_info)
			.writeBuffer(2, &material_ssbo_info)
			.build(set);
		m_global_descriptor_sets.push_back(std::move(set));
	}
}

SceneContext RenderResources::makeSceneContext() {
	return {m_ve_device, m_resource_manager, *m_global_pool, *m_material_set_layout, &m_default_material_descriptor_set};
}

} // namespace ve