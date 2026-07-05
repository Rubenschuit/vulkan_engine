#include "pch.hpp"
#include "rendering/bloom_system.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "events/render_events.hpp"

namespace ve {

BloomSystem::BloomSystem(
	VeDevice& device,
	vk::Extent2D extent,
	const vk::raii::ImageView& input_image_view,
	std::filesystem::path downsample_shader_path,
	std::filesystem::path upsample_shader_path,
	EventBus& event_bus)
	: m_ve_device(device),
	  m_downsample_shader_path(std::move(downsample_shader_path)),
	  m_upsample_shader_path(std::move(upsample_shader_path)) {

	event_bus.subscribe<ResolutionChangedEvent>([this](const ResolutionChangedEvent& e) {
		recreateResources(e.extent, e.resolve_target_view);
	});

	createDescriptorSetLayout();
	createDescriptorPool();
	createMipChain(extent);
	createDescriptorSets(input_image_view);
	createPipelineLayouts();
	createPipelines();
}

BloomSystem::~BloomSystem() = default;

void BloomSystem::createDescriptorSetLayout() {
	m_descriptor_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();
}

void BloomSystem::createDescriptorPool() {
	// We need descriptor sets for each mip and one for the source HDR image
	m_descriptor_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(m_mip_count + 1)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, m_mip_count + 1)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.build();
}

void BloomSystem::createDescriptorSets(const vk::raii::ImageView& input_image_view) {
	vk::DescriptorImageInfo image_info{
		.sampler = **m_sampler,
		.imageView = *input_image_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};

	VeDescriptorWriter(*m_descriptor_set_layout, *m_descriptor_pool)
		.writeImage(0, &image_info)
		.build(m_hdr_input_descriptor_set);
}

// Creates images and descriptor sets that make up the mip chain
void BloomSystem::createMipChain(vk::Extent2D extent) {
	m_bloom_mips.clear();
	m_bloom_mips.reserve(m_mip_count);

	if (!m_sampler) {
		vk::SamplerCreateInfo sampler_info{
			.magFilter = vk::Filter::eLinear,
			.minFilter = vk::Filter::eLinear,
			.mipmapMode = vk::SamplerMipmapMode::eLinear,
			.addressModeU = vk::SamplerAddressMode::eClampToEdge,
			.addressModeV = vk::SamplerAddressMode::eClampToEdge,
			.addressModeW = vk::SamplerAddressMode::eClampToEdge,
			.mipLodBias = 0.0f,
			.compareEnable = vk::False,
			.compareOp = vk::CompareOp::eAlways,
			.minLod = 0.0f,
			.maxLod = 1.0f,
			.borderColor = vk::BorderColor::eIntOpaqueBlack,
			.unnormalizedCoordinates = vk::False
		};
		m_sampler = std::make_unique<vk::raii::Sampler>(m_ve_device.getDevice(), sampler_info);
	}

	vk::Extent2D current_extent = extent;
	// Create mip chain, dividing the extent by 2 for each mip
	for (uint32_t i = 0; i < m_mip_count; i++) {
		current_extent.width = std::max(1u, current_extent.width / 2);
		current_extent.height = std::max(1u, current_extent.height / 2);

		BloomMip mip;
		mip.image = std::make_unique<VeImage>(
			m_ve_device,
			current_extent.width,
			current_extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR16G16B16A16Sfloat, // HDR format
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false,
			1
		);

		mip.image->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			{},
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eFragmentShader
		);
		mip.image->setDebugName(("Bloom Mip " + std::to_string(i)).c_str());

		vk::DescriptorImageInfo image_info{
			.sampler = **m_sampler,
			.imageView = *mip.image->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
		};

		VeDescriptorWriter(*m_descriptor_set_layout, *m_descriptor_pool)
			.writeImage(0, &image_info)
			.build(mip.descriptor_set);

		m_bloom_mips.push_back(std::move(mip));
	}
}

void BloomSystem::createPipelineLayouts() {
	vk::DescriptorSetLayout layouts[1] = {m_descriptor_set_layout->getDescriptorSetLayout()};

	vk::PushConstantRange downsample_push{
		.stageFlags = vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(BloomDownsamplePushConstant)
	};

	vk::PipelineLayoutCreateInfo downsample_info{
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &downsample_push
	};
	m_downsample_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), downsample_info);

	vk::PushConstantRange upsample_push{
		.stageFlags = vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(BloomUpsamplePushConstant)
	};

	vk::PipelineLayoutCreateInfo upsample_info{
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &upsample_push
	};
	m_upsample_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), upsample_info);
}

void BloomSystem::createPipelines() {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	config.color_format = vk::Format::eR16G16B16A16Sfloat;
	config.attribute_descriptions.clear();
	config.binding_descriptions.clear();
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.depth_stencil_info.depthTestEnable = vk::False;
	config.depth_stencil_info.depthWriteEnable = vk::False;

	// Downsample pipeline
	config.pipeline_layout = *m_downsample_pipeline_layout;
	m_downsample_pipeline = std::make_unique<VePipeline>(m_ve_device, m_downsample_shader_path, config);

	// Upsample pipeline with additive blending
	config.pipeline_layout = *m_upsample_pipeline_layout;
	config.color_blend_attachment.blendEnable = VK_TRUE;
	config.color_blend_attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.dstColorBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.colorBlendOp = vk::BlendOp::eAdd;
	config.color_blend_attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
	config.color_blend_attachment.alphaBlendOp = vk::BlendOp::eAdd;

	m_upsample_pipeline = std::make_unique<VePipeline>(m_ve_device, m_upsample_shader_path, config);
}

// Records the command buffer to perform downsampling and upsampling using
// the mip chain to ultimately produce the bloom effect image. The final result
// is stored in the first mip of the mip chain and can be used for composition
// in the post process system.
void BloomSystem::render(vk::raii::CommandBuffer& command_buffer) {
	// 1. Downsampling
	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_downsample_pipeline->getPipeline());

	for (uint32_t i = 0; i < m_mip_count; i++) {
		// Transition current mip to color attachment for writing
		m_bloom_mips[i].image->transitionImageLayout(
			command_buffer,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::AccessFlagBits2::eShaderRead,
			vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead,
			vk::PipelineStageFlagBits2::eFragmentShader,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);

		vk::RenderingAttachmentInfo color_attachment{
			.imageView = *m_bloom_mips[i].image->getImageView(),
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eClear,
			.storeOp = vk::AttachmentStoreOp::eStore,
			.clearValue = vk::ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f)
		};

		vk::RenderingInfo rendering_info{
			.renderArea = { .offset = {0, 0}, .extent = m_bloom_mips[i].image->getExtent2D() },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment
		};

		command_buffer.beginRendering(rendering_info);
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(m_bloom_mips[i].image->getWidth()),
			.height = static_cast<float>(m_bloom_mips[i].image->getHeight()),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = m_bloom_mips[i].image->getExtent2D()});

		// Bind input: first mip uses HDR buffer, subsequent mips use previous mip
		vk::DescriptorSet input_set_handle;
		if (i == 0) {
			input_set_handle = *m_hdr_input_descriptor_set;
		} else {
			input_set_handle = *m_bloom_mips[i-1].descriptor_set;
		}

		command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_downsample_pipeline_layout, 0, {input_set_handle}, {});

		BloomDownsamplePushConstant push{
			.is_first_pass = (i == 0) ? 1 : 0
		};
		command_buffer.pushConstants<BloomDownsamplePushConstant>(*m_downsample_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, push);

		command_buffer.draw(3, 1, 0, 0);
		command_buffer.endRendering();

		// Transition current mip back to shader read for the next pass
		m_bloom_mips[i].image->transitionImageLayout(
			command_buffer,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eFragmentShader
		);
	}

	// 2. Upsampling
	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_upsample_pipeline->getPipeline());

	// Filter radius in texel space
	float filter_radius = 0.005f;

	for (uint32_t i = m_mip_count - 1; i > 0; i--) {
		uint32_t next_mip_idx = i - 1;

		// Transition next mip (the larger one) to color attachment for writing (additive blend)
		m_bloom_mips[next_mip_idx].image->transitionImageLayout(
			command_buffer,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::AccessFlagBits2::eShaderRead,
			vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead,
			vk::PipelineStageFlagBits2::eFragmentShader,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
		);

		vk::RenderingAttachmentInfo color_attachment{
			.imageView = *m_bloom_mips[next_mip_idx].image->getImageView(),
			.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
			.loadOp = vk::AttachmentLoadOp::eLoad, // We want to blend with existing content
			.storeOp = vk::AttachmentStoreOp::eStore
		};

		vk::RenderingInfo rendering_info{
			.renderArea = { .offset = {0, 0},
			.extent = m_bloom_mips[next_mip_idx].image->getExtent2D() },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment
		};

		command_buffer.beginRendering(rendering_info);
		command_buffer.setViewport(0, vk::Viewport{
			.x = 0.0f, .y = 0.0f,
			.width = static_cast<float>(m_bloom_mips[next_mip_idx].image->getWidth()),
			.height = static_cast<float>(m_bloom_mips[next_mip_idx].image->getHeight()),
			.minDepth = 0.0f, .maxDepth = 1.0f
		});
		command_buffer.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = m_bloom_mips[next_mip_idx].image->getExtent2D()});

		// Bind input: the current smaller mip
		command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_upsample_pipeline_layout, 0, {*m_bloom_mips[i].descriptor_set}, {});

		BloomUpsamplePushConstant push{ .filter_radius = filter_radius };
		command_buffer.pushConstants<BloomUpsamplePushConstant>(*m_upsample_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, push);

		command_buffer.draw(3, 1, 0, 0);
		command_buffer.endRendering();

		// Transition next mip back to shader read
		m_bloom_mips[next_mip_idx].image->transitionImageLayout(
			command_buffer,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eFragmentShader
		);
	}
}

void BloomSystem::recreateResources(vk::Extent2D extent, const vk::raii::ImageView& input_image_view) {
	m_hdr_input_descriptor_set = nullptr;
	m_bloom_mips.clear();
	m_descriptor_pool->resetPool();
	createMipChain(extent);
	createDescriptorSets(input_image_view);
}

} // namespace ve

