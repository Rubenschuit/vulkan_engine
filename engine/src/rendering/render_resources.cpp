#include "pch.hpp"
#include "rendering/render_resources.hpp"

#include "scene/ve_scene.hpp"
#include "utils/ve_log.hpp"
#include "ve_config.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "vulkan/ve_device.hpp"

#include <cassert>

namespace ve {

RenderResources::RenderResources(VeDevice& device,
                                 VeResourceManager& resource_manager,
                                 EventBus& event_bus)
	: m_ve_device(device),
	  m_resource_manager(resource_manager),
	  m_event_bus(event_bus) {
	createDescriptors();
}

RenderResources::~RenderResources() = default;

void RenderResources::createDescriptors() {
	VE_LOGD("Creating descriptors");

	m_global_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.addBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment)
		.build();

	m_material_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();

	m_global_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(8192)
		.addPoolSize(vk::DescriptorType::eUniformBuffer, 1024)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 4096)
		.addPoolSize(vk::DescriptorType::eSampler, 256)
		.addPoolSize(vk::DescriptorType::eSampledImage, 256)
		.addPoolSize(vk::DescriptorType::eStorageBuffer, 8192)
		.addPoolSize(vk::DescriptorType::eStorageImage, 128)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet
			| vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind)
		.buildShared();

	m_default_particle_texture_handle = m_resource_manager.load<VeTexture>("default_particle");
}

SceneContext RenderResources::makeSceneContext() {
	return {m_ve_device, m_resource_manager, m_event_bus};
}

} // namespace ve
