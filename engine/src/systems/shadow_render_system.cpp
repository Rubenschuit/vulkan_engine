#include "pch.hpp"
#include "systems/shadow_render_system.hpp"
#include "core/ve_device.hpp"
#include "core/ve_pipeline.hpp"
#include "core/ve_buffer.hpp"
#include "core/ve_descriptors.hpp"
#include "core/ve_image.hpp"
#include "core/ve_texture.hpp"
#include "game/ve_frame_info.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

namespace ve {

struct SimplePushConstantData {
	alignas(16) glm::mat4 transform;
	alignas(16) glm::mat3x4 normal_transform;
	alignas(4)  float has_texture;
	alignas(4)  float padding[3];
};
static_assert(sizeof(SimplePushConstantData) <= 128, "Push constants must be 128 bytes for stable layout");

ShadowRenderSystem::ShadowRenderSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& shadow_global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(shader_path) {

	// Initialize 2D arrays for per-frame, per-light data
	m_shadow_ubos.resize(MAX_FRAMES_IN_FLIGHT);
	m_shadow_global_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_views.resize(MAX_FRAMES_IN_FLIGHT);
	m_light_projs.resize(MAX_FRAMES_IN_FLIGHT);

	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_shadow_ubos[i].resize(MAX_LIGHTS);
		m_shadow_global_descriptor_sets[i].reserve(MAX_LIGHTS);
	}

	// Create shadow resources (images, sampler, descriptor sets)
	createShadowResources();

	// Create shadow descriptor set layout
	m_shadow_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment)
		.build();

	createShadowUBOs();
	createShadowPassDescriptorSets(descriptor_pool, shadow_global_set_layout);
	createShadowTextureDescriptorSets(descriptor_pool);
	createPipelineLayout(shadow_global_set_layout, material_set_layout);
	createPipeline(m_shadow_depth_format);
}

ShadowRenderSystem::~ShadowRenderSystem() {
}

void ShadowRenderSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& shadow_global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout) {
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(SimplePushConstantData)
	};
	vk::DescriptorSetLayout layouts[2] = {*shadow_global_set_layout, *material_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 2,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void ShadowRenderSystem::createPipeline(vk::Format depth_format) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);

	// Depth-only pipeline: no color output
	pipeline_config.color_format = vk::Format::eUndefined;
	pipeline_config.depth_format = depth_format;
	pipeline_config.attribute_descriptions = VeModel::Vertex::getAttributeDescriptionsShadow();

	pipeline_config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eFront;
	pipeline_config.rasterization_info.depthClampEnable = VK_TRUE;
	pipeline_config.rasterization_info.depthBiasEnable = VK_TRUE;

	pipeline_config.depth_stencil_info.depthTestEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eLess;

	assert(m_pipeline_layout != VK_NULL_HANDLE && "Pipeline layout is null");
	pipeline_config.pipeline_layout = m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
	assert(m_ve_pipeline != VK_NULL_HANDLE && "Failed to create shadow pipeline");
}

void ShadowRenderSystem::createShadowUBOs() {
	vk::DeviceSize buffer_size = sizeof(UniformBufferObject);
	assert(buffer_size > 0 && "Shadow uniform buffer size is zero");
	//assert(buffer_size % 16 == 0 && "Shadow uniform buffer size must be a multiple of 16 bytes");
	assert(buffer_size <= m_ve_device.getDeviceProperties().limits.maxUniformBufferRange && "Shadow uniform buffer size exceeds maximum limit");

	// Create one buffer for each frame and each light
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		for (size_t light = 0; light < MAX_LIGHTS; light++) {
			m_shadow_ubos[frame][light] = std::make_unique<VeBuffer>(
				m_ve_device,
				buffer_size,
				1,
				vk::BufferUsageFlagBits::eUniformBuffer,
				vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
				m_ve_device.getDeviceProperties().limits.minUniformBufferOffsetAlignment
			);
			m_shadow_ubos[frame][light]->map();
		}
	}
}

void ShadowRenderSystem::createShadowPassDescriptorSets(VeDescriptorPool& descriptor_pool, const vk::raii::DescriptorSetLayout& shadow_global_set_layout) {
	// Create descriptor sets for shadow pass (per-frame, per-light) - contains view/proj matrices
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		m_shadow_global_descriptor_sets[frame].clear();

		for (size_t light = 0; light < MAX_LIGHTS; light++) {
			vk::raii::DescriptorSet descriptor_set{nullptr};
			descriptor_pool.allocateDescriptor(shadow_global_set_layout, descriptor_set);

			// Update descriptor set with shadow UBO for this specific light
			vk::DescriptorBufferInfo buffer_info{
				.buffer = *m_shadow_ubos[frame][light]->getBuffer(),
				.offset = 0,
				.range = sizeof(UniformBufferObject)
			};
			vk::WriteDescriptorSet descriptor_write{
				.dstSet = *descriptor_set,
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = vk::DescriptorType::eUniformBuffer,
				.pBufferInfo = &buffer_info
			};
			m_ve_device.getDevice().updateDescriptorSets(descriptor_write, {});

			m_shadow_global_descriptor_sets[frame].push_back(std::move(descriptor_set));
		}
	}
}

void ShadowRenderSystem::createShadowResources() {
	VE_LOGD("Shadow system: Creating shadow resources");

	// Find suitable depth format
	m_shadow_depth_format = m_ve_device.findSupportedFormat(
		{vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
		vk::ImageTiling::eOptimal,
		vk::FormatFeatureFlagBits::eDepthStencilAttachment | vk::FormatFeatureFlagBits::eSampledImage
	);

	// Create a single 2D array texture for all shadow maps (MAX_LIGHTS layers)
	m_shadow_map_array = std::make_unique<VeImage>(
		m_ve_device,
		SHADOW_MAP_RESOLUTION,
		SHADOW_MAP_RESOLUTION,
		vk::SampleCountFlagBits::e1,
		m_shadow_depth_format,
		vk::ImageTiling::eOptimal,
		vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
		vk::MemoryPropertyFlagBits::eDeviceLocal,
		vk::ImageAspectFlagBits::eDepth,
		false,  // not cubemap
		MAX_LIGHTS  // array layers
	);

	// Create individual layer views for rendering to specific layers
	m_shadow_map_layer_views.clear();
	m_shadow_map_layer_views.reserve(MAX_LIGHTS);
	for (uint32_t i = 0; i < MAX_LIGHTS; i++) {
		m_shadow_map_layer_views.push_back(m_shadow_map_array->createLayerImageView(i));
	}

	// Transition shadow map array to depth stencil read-only optimal layout
	m_shadow_map_array->transitionImageLayout(
		vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		vk::AccessFlags2{},
		vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eFragmentShader
	);

	// Create depth compare sampler for shadow maps
	m_shadow_sampler = VeTexture::createDepthCompareSampler(m_ve_device);
}

void ShadowRenderSystem::createShadowTextureDescriptorSets(VeDescriptorPool& descriptor_pool) {
	// Create descriptor sets for shadow textures (per-frame) - used by other systems to sample shadows
	m_shadow_descriptor_sets.clear();
	m_shadow_descriptor_sets.reserve(MAX_FRAMES_IN_FLIGHT);

	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::raii::DescriptorSet set{nullptr};
		VeDescriptorWriter(*m_shadow_set_layout, descriptor_pool)
			.build(set);
		m_shadow_descriptor_sets.push_back(std::move(set));
	}

	// Update all per-frame shadow descriptor sets (sampler and image)
	for (size_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		// Write sampler (binding 0)
		vk::DescriptorImageInfo sampler_info{
			.sampler = *m_shadow_sampler,
			.imageView = nullptr,
			.imageLayout = vk::ImageLayout::eUndefined
		};
		vk::WriteDescriptorSet sampler_write{
			.dstSet = *m_shadow_descriptor_sets[frame],
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = vk::DescriptorType::eSampler,
			.pImageInfo = &sampler_info
		};

		// Write texture array (binding 1)
		vk::DescriptorImageInfo image_info{
			.sampler = nullptr,
			.imageView = *m_shadow_map_array->getImageView(),
			.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal
		};
		vk::WriteDescriptorSet image_write{
			.dstSet = *m_shadow_descriptor_sets[frame],
			.dstBinding = 1,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = vk::DescriptorType::eSampledImage,
			.pImageInfo = &image_info
		};

		m_ve_device.getDevice().updateDescriptorSets({sampler_write, image_write}, {});
	}
}

void ShadowRenderSystem::updateUniformBuffer(uint32_t frame_index, const UniformBufferObject& ubo) {
	if (ubo.num_lights == 0) {
		VE_LOGW("Shadow system: no lights in UBO!");
		m_light_views[frame_index].clear();
		m_light_projs[frame_index].clear();
		return;
	}

	m_light_views[frame_index].resize(ubo.num_lights);
	m_light_projs[frame_index].resize(ubo.num_lights);

	// Update each light's cached data and write to its dedicated buffer
	for (uint32_t light_index = 0; light_index < ubo.num_lights && light_index < MAX_LIGHTS; light_index++) {
		m_light_views[frame_index][light_index] = ubo.point_lights[light_index].light_view;
		m_light_projs[frame_index][light_index] = ubo.point_lights[light_index].light_proj;

		UniformBufferObject shadow_ubo{};
		shadow_ubo.view = ubo.point_lights[light_index].light_view;
		shadow_ubo.proj = ubo.point_lights[light_index].light_proj;
		m_shadow_ubos[frame_index][light_index]->writeToBuffer(&shadow_ubo);
	}
}

void ShadowRenderSystem::renderShadowMaps(VeFrameInfo& frame_info) {
	const auto& light_views = m_light_views[frame_info.current_frame];
	const auto& light_projs = m_light_projs[frame_info.current_frame];

	assert(light_views.size() == light_projs.size() && "Light views and projections must have same size");
	assert(light_views.size() <= m_shadow_map_layer_views.size() && "Not enough shadow layer views");
	if (light_views.empty()) {
		return;
	}
	assert(light_views.size() <= MAX_LIGHTS && "Number of lights exceeds MAX_LIGHTS");

	auto& command_buffer = frame_info.command_buffer;
	vk::Extent2D shadow_extent{SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION};

	for (size_t light_index = 0; light_index < light_views.size(); light_index++) {
		assert(light_index < MAX_LIGHTS && "Light index exceeds MAX_LIGHTS");
		assert(light_index < m_shadow_map_layer_views.size() && "Light index exceeds shadow layer views count");

		// Begin depth-only rendering to shadow map (render to specific layer of array)
		vk::RenderingAttachmentInfo depth_attachment{
			.imageView = *m_shadow_map_layer_views[light_index],
			.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearDepthStencilValue(1.0f, 0)
		};
		vk::RenderingInfo shadow_rendering_info{
			.renderArea = { {0, 0}, shadow_extent },
			.layerCount = 1,
			.colorAttachmentCount = 0,
			.pColorAttachments = nullptr,
			.pDepthAttachment = &depth_attachment
		};

		// Transition shadow map layer to depth attachment optimal
		vk::ImageMemoryBarrier2 barrier{
			.sType = vk::StructureType::eImageMemoryBarrier2,
			.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
			.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
			.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
			.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = *m_shadow_map_array->getImage(),
			.subresourceRange = {
				.aspectMask = vk::ImageAspectFlagBits::eDepth,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = static_cast<uint32_t>(light_index),
				.layerCount = 1
			}
		};
		vk::DependencyInfo dep_info{
			.sType = vk::StructureType::eDependencyInfo,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &barrier
		};
		command_buffer.pipelineBarrier2(dep_info);

		command_buffer.beginRendering(shadow_rendering_info);

		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = static_cast<float>(SHADOW_MAP_RESOLUTION),
			.height = static_cast<float>(SHADOW_MAP_RESOLUTION),
			.minDepth = 0.0f,
			.maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), shadow_extent));

		// Set depth bias to reduce shadow artifacts (push geometry slightly away from light)
		command_buffer.setDepthBias(1.25f, 0.0f, 1.75f);

		// Render scene from light's perspective
		renderShadowMap(frame_info, static_cast<uint32_t>(light_index));

		command_buffer.endRendering();

		// Transition shadow map layer back to shader read optimal
		barrier.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests;
		barrier.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
		barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
		barrier.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
		barrier.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
		command_buffer.pipelineBarrier2(dep_info);
	}
}

void ShadowRenderSystem::renderShadowMap(VeFrameInfo& frame_info, uint32_t light_index) const {

	const vk::raii::DescriptorSet& shadow_global_set = m_shadow_global_descriptor_sets[frame_info.current_frame][light_index];

	frame_info.command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	frame_info.command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,  // firstSet - bind starting at set 0
		{*shadow_global_set, frame_info.material_descriptor_set},
		{}  // dynamicOffsets
	);

	for (auto& [id, obj] : frame_info.game_objects) {
		if (!obj.ve_model || !obj.has_shadow)
			continue;

		SimplePushConstantData push{};
		const glm::mat3 nrm = obj.getNormalTransform();
		push.normal_transform[0] = glm::vec4(nrm[0], 0.0f);
		push.normal_transform[1] = glm::vec4(nrm[1], 0.0f);
		push.normal_transform[2] = glm::vec4(nrm[2], 0.0f);
		push.transform = obj.getTransform();
		push.has_texture = obj.has_texture;
		frame_info.command_buffer.pushConstants(
			*m_pipeline_layout,
			vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			0,
			vk::ArrayProxy<const uint8_t>(sizeof(SimplePushConstantData), reinterpret_cast<const uint8_t*>(&push))
		);
		obj.ve_model->bindVertexBuffer(frame_info.command_buffer);
		obj.ve_model->bindIndexBuffer(frame_info.command_buffer);
		obj.ve_model->drawIndexed(frame_info.command_buffer);
	}
}

} // namespace ve

