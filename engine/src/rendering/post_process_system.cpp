#include "pch.hpp"
#include "rendering/post_process_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "utils/ve_log.hpp"

namespace ve {

PostProcessSystem::PostProcessSystem(
	VeDevice& device,
	vk::Format color_format,
	const vk::raii::ImageView& resolve_target_view,
	const vk::raii::ImageView& bloom_texture_view,
	std::filesystem::path shader_path)
	: m_ve_device(device), m_shader_path(std::move(shader_path)) {

	createDescriptorSetLayout();
	createDescriptorPool();
	createDescriptorSet(resolve_target_view, bloom_texture_view);
	createPipelineLayout();
	createPipeline(color_format);
}

PostProcessSystem::~PostProcessSystem() = default;

void PostProcessSystem::createDescriptorSetLayout() {
	m_descriptor_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();
}

void PostProcessSystem::createDescriptorPool() {
	m_descriptor_pool = VeDescriptorPool::Builder(m_ve_device)
		.setMaxSets(1)
		.addPoolSize(vk::DescriptorType::eCombinedImageSampler, 2)
		.setPoolFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
		.build();
}

void PostProcessSystem::createDescriptorSet(const vk::raii::ImageView& resolve_target_view, const vk::raii::ImageView& bloom_texture_view) {
	if (!m_sampler) {
		vk::SamplerCreateInfo sampler_info{
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
			.compareOp = vk::CompareOp::eAlways,
			.minLod = 0.0f,
			.maxLod = 1.0f,
			.borderColor = vk::BorderColor::eIntOpaqueBlack,
			.unnormalizedCoordinates = vk::False
		};

		m_sampler = std::make_unique<vk::raii::Sampler>(m_ve_device.getDevice(), sampler_info);
	}

	vk::DescriptorImageInfo image_info{
		.sampler = **m_sampler,
		.imageView = *resolve_target_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};

	vk::DescriptorImageInfo bloom_info{
		.sampler = **m_sampler,
		.imageView = *bloom_texture_view,
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal
	};

	VeDescriptorWriter(*m_descriptor_set_layout, *m_descriptor_pool)
		.writeImage(0, &image_info)
		.writeImage(1, &bloom_info)
		.build(m_descriptor_set);
}

void PostProcessSystem::createPipelineLayout() {
	vk::DescriptorSetLayout layouts[1] = {m_descriptor_set_layout->getDescriptorSetLayout()};
	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(PostProcessPushConstant)
	};

	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};

	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void PostProcessSystem::createPipeline(vk::Format color_format) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	pipeline_config.color_format = color_format;
	pipeline_config.pipeline_layout = *m_pipeline_layout;

	// No vertex input for full screen triangle
	pipeline_config.attribute_descriptions.clear();
	pipeline_config.binding_descriptions.clear();
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;

	// Disable depth testing for post process
	pipeline_config.depth_stencil_info.depthTestEnable = vk::False;
	pipeline_config.depth_stencil_info.depthWriteEnable = vk::False;

	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config);
}

void PostProcessSystem::render(vk::raii::CommandBuffer& command_buffer, const PostProcessPushConstant& push) {
	command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	command_buffer.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*m_descriptor_set},
		{}
	);

	command_buffer.pushConstants<PostProcessPushConstant>(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eFragment,
		0,
		push
	);

	// Draw 3 vertices for the full screen triangle
	command_buffer.draw(3, 1, 0, 0);
}

void PostProcessSystem::recreatePipeline(vk::Format color_format, const vk::raii::ImageView& resolve_target_view, const vk::raii::ImageView& bloom_texture_view) {
	m_ve_pipeline.reset();
	m_descriptor_set = nullptr;
	m_descriptor_pool->resetPool();
	createDescriptorSet(resolve_target_view, bloom_texture_view);
	createPipeline(color_format);
}

} // namespace ve

