#include "pch.hpp"
#include "rendering/gtao_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_buffer.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "platform/ve_file_system.hpp"
#include "utils/ve_log.hpp"

namespace ve {

struct GtaoPushConstant {
	glm::vec2 ao_size;     // AO output resolution
	glm::vec2 depth_size;  // depth buffer resolution
	float radius;          // world-space AO radius
	float intensity;       // AO intensity multiplier
	float proj_scale;      // projection scale: proj[1][1] * ao_size.y * 0.5
	float inv_focal_x;     // 1.0 / proj[0][0]
	float inv_focal_y;     // 1.0 / proj[1][1]
	float proj_22;         // proj[2][2] for depth linearization
	float proj_32;         // proj[3][2] for depth linearization
	float _pad;
};

struct BlurPushConstant {
	glm::vec2 image_size;  // AO image resolution
	glm::vec2 depth_size;  // depth buffer resolution
	int direction_x;
	int direction_y;
	float sharpness;
	float _pad;
};

GtaoSystem::GtaoSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	VeResourceManager& resource_manager,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	std::filesystem::path shader_path,
	vk::Extent2D ao_extent,
	vk::Extent2D depth_extent,
	const vk::raii::ImageView& depth_image_view,
	const vk::raii::Image& depth_image)
	: m_ve_device(device), m_shader_path(std::move(shader_path)),
	  m_extent(ao_extent), m_depth_extent(depth_extent) {

	m_depth_image = *depth_image;
	m_depth_image_view = *depth_image_view;
	m_default_ao_texture = resource_manager.load<VeTexture>("default_albedo");
	createAoImages(m_extent);
	createComputeSetLayout();
	createBlurSetLayout();
	createOutputSetLayout();
	createSampler();
	createGtaoPipelineLayout(global_set_layout);
	createBlurPipelineLayout();
	createPipelines();
	createDescriptorSets(descriptor_pool);
}

GtaoSystem::~GtaoSystem() = default;

void GtaoSystem::createAoImages(vk::Extent2D extent) {
	auto make_image = [&]() {
		auto img = std::make_unique<VeImage>(
			m_ve_device,
			extent.width,
			extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR8Unorm,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1
		);
		img->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eFragmentShader
		);
		return img;
	};
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_ao_raw_images[i] = make_image();
		m_ao_blur_images[i] = make_image();
	}
}

void GtaoSystem::createComputeSetLayout() {
	m_compute_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // depth
		.addBinding(1, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute) // AO out
		.build();
}

void GtaoSystem::createBlurSetLayout() {
	m_blur_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // depth
		.addBinding(1, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute) // AO input
		.addBinding(2, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute) // AO output
		.build();
}

void GtaoSystem::createOutputSetLayout() {
	m_output_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eSampler, vk::ShaderStageFlagBits::eFragment)
		.build();
}

void GtaoSystem::createSampler() {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eLinear,
		.minFilter = vk::Filter::eLinear,
		.mipmapMode = vk::SamplerMipmapMode::eNearest,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = vk::BorderColor::eFloatOpaqueWhite,
		.unnormalizedCoordinates = VK_FALSE,
	};
	m_linear_clamp_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void GtaoSystem::createGtaoPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(GtaoPushConstant),
	};

	std::array<vk::DescriptorSetLayout, 2> set_layouts{
		*global_set_layout,
		*m_compute_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};

	m_gtao_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void GtaoSystem::createBlurPipelineLayout() {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eCompute,
		.offset = 0,
		.size = sizeof(BlurPushConstant),
	};

	std::array<vk::DescriptorSetLayout, 1> set_layouts{
		*m_blur_set_layout->getDescriptorSetLayout(),
	};

	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = static_cast<uint32_t>(set_layouts.size()),
		.pSetLayouts = set_layouts.data(),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};

	m_blur_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void GtaoSystem::createPipelines() {
	auto load_module = [&](const std::filesystem::path& path) {
		auto code = VeFileSystem::readFile(path);
		vk::ShaderModuleCreateInfo info{
			.codeSize = code.size(),
			.pCode = reinterpret_cast<const uint32_t*>(code.data()),
		};
		return vk::raii::ShaderModule(m_ve_device.getDevice(), info);
	};

	// GTAO pipeline
	m_gtao_module = load_module(m_shader_path / "gtao_comp.spv");
	{
		vk::PipelineShaderStageCreateInfo stage_info{
			.stage = vk::ShaderStageFlagBits::eCompute,
			.module = *m_gtao_module,
			.pName = "compMain",
		};
		vk::ComputePipelineCreateInfo pipeline_info{
			.stage = stage_info,
			.layout = *m_gtao_pipeline_layout,
		};
		m_gtao_pipeline = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pipeline_info);
	}

	// Blur pipeline
	m_blur_module = load_module(m_shader_path / "gtao_blur_comp.spv");
	{
		vk::PipelineShaderStageCreateInfo stage_info{
			.stage = vk::ShaderStageFlagBits::eCompute,
			.module = *m_blur_module,
			.pName = "compMain",
		};
		vk::ComputePipelineCreateInfo pipeline_info{
			.stage = stage_info,
			.layout = *m_blur_pipeline_layout,
		};
		m_blur_pipeline = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pipeline_info);
	}
}

void GtaoSystem::createDescriptorSets(VeDescriptorPool& descriptor_pool) {
	vk::DescriptorImageInfo depth_info{
		.imageView = m_depth_image_view,
		.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
	};
	vk::DescriptorImageInfo sampler_info{
		.sampler = *m_linear_clamp_sampler,
	};

	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::DescriptorImageInfo ao_raw_storage_info{
			.imageView = *m_ao_raw_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo ao_raw_sampled_info{
			.imageView = *m_ao_raw_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo ao_blur_storage_info{
			.imageView = *m_ao_blur_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo ao_blur_sampled_info{
			.imageView = *m_ao_blur_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};

		// GTAO compute: depth + raw AO storage
		VeDescriptorWriter(*m_compute_set_layout, descriptor_pool)
			.writeImage(0, &depth_info)
			.writeImage(1, &ao_raw_storage_info)
			.build(m_compute_descriptor_sets[frame]);

		// Blur H: depth + raw AO (sampled read) + blur AO (storage write)
		VeDescriptorWriter(*m_blur_set_layout, descriptor_pool)
			.writeImage(0, &depth_info)
			.writeImage(1, &ao_raw_sampled_info)
			.writeImage(2, &ao_blur_storage_info)
			.build(m_blur_h_descriptor_sets[frame]);

		// Blur V: depth + blur AO (sampled read) + raw AO (storage write back)
		VeDescriptorWriter(*m_blur_set_layout, descriptor_pool)
			.writeImage(0, &depth_info)
			.writeImage(1, &ao_blur_sampled_info)
			.writeImage(2, &ao_raw_storage_info)
			.build(m_blur_v_descriptor_sets[frame]);

		// Output: raw AO (sampled) + linear sampler (for fragment shaders)
		VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
			.writeImage(0, &ao_raw_sampled_info)
			.writeImage(1, &sampler_info)
			.build(m_output_descriptor_sets[frame]);
	}

	// Dummy output: default white texture
	vk::DescriptorImageInfo default_sampled_info{
		.imageView = *m_default_ao_texture->getImageView(),
		.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	};
	VeDescriptorWriter(*m_output_set_layout, descriptor_pool)
		.writeImage(0, &default_sampled_info)
		.writeImage(1, &sampler_info)
		.build(m_dummy_output_descriptor_set);
}

void GtaoSystem::dispatch(VeFrameInfo& frame_info) {
	auto& cmd = frame_info.command_buffer;
	uint32_t frame = frame_info.current_frame;

	// ===== Pre-GTAO barriers =====
	// Depth: eDepthAttachmentOptimal -> eDepthStencilReadOnlyOptimal
	vk::ImageMemoryBarrier2 depth_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
		.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_depth_image,
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
	};
	// AO raw: eShaderReadOnlyOptimal -> eGeneral for storage write
	vk::ImageMemoryBarrier2 ao_raw_to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_ao_raw_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		std::array barriers = {depth_to_read, ao_raw_to_general};
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
			.pImageMemoryBarriers = barriers.data(),
		};
		cmd.pipelineBarrier2(dep);
	}

	// ===== GTAO dispatch =====
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_gtao_pipeline);

	std::array<vk::DescriptorSet, 2> gtao_sets{
		*frame_info.global_descriptor_set,
		*m_compute_descriptor_sets[frame],
	};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_gtao_pipeline_layout, 0, gtao_sets, {});

	const auto& proj = frame_info.camera.getProj();
	GtaoPushConstant gtao_push{
		.ao_size = glm::vec2(static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_depth_extent.width), static_cast<float>(m_depth_extent.height)),
		.radius = m_radius,
		.intensity = m_intensity,
		.proj_scale = std::abs(proj[1][1]) * static_cast<float>(m_extent.height) * 0.5f,
		.inv_focal_x = 1.0f / proj[0][0],
		.inv_focal_y = 1.0f / proj[1][1],
		.proj_22 = proj[2][2],
		.proj_32 = proj[3][2],
		._pad = 0.0f,
	};
	cmd.pushConstants(
		*m_gtao_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(GtaoPushConstant), reinterpret_cast<const uint8_t*>(&gtao_push)));

	uint32_t groups_x = (m_extent.width + 15) / 16;
	uint32_t groups_y = (m_extent.height + 15) / 16;
	cmd.dispatch(groups_x, groups_y, 1);

	// ===== Pre-blur-H barriers =====
	// ao_raw: eGeneral -> eShaderReadOnlyOptimal (compute read)
	vk::ImageMemoryBarrier2 ao_raw_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_ao_raw_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	// ao_blur: -> eGeneral for storage write
	vk::ImageMemoryBarrier2 ao_blur_to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_ao_blur_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		std::array barriers = {ao_raw_to_read, ao_blur_to_general};
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
			.pImageMemoryBarriers = barriers.data(),
		};
		cmd.pipelineBarrier2(dep);
	}

	// ===== Blur H dispatch =====
	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_blur_pipeline);
	std::array<vk::DescriptorSet, 1> blur_h_sets{*m_blur_h_descriptor_sets[frame]};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_blur_pipeline_layout, 0, blur_h_sets, {});

	BlurPushConstant blur_push_h{
		.image_size = glm::vec2(static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_depth_extent.width), static_cast<float>(m_depth_extent.height)),
		.direction_x = 1,
		.direction_y = 0,
		.sharpness = 16.0f,
		._pad = 0.0f,
	};
	cmd.pushConstants(
		*m_blur_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(BlurPushConstant), reinterpret_cast<const uint8_t*>(&blur_push_h)));
	cmd.dispatch(groups_x, groups_y, 1);

	// ===== Pre-blur-V barriers =====
	// ao_blur: eGeneral -> eShaderReadOnlyOptimal
	vk::ImageMemoryBarrier2 ao_blur_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_ao_blur_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	// ao_raw: -> eGeneral for storage write (blur V output)
	vk::ImageMemoryBarrier2 ao_raw_to_general2{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_ao_raw_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		std::array barriers = {ao_blur_to_read, ao_raw_to_general2};
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
			.pImageMemoryBarriers = barriers.data(),
		};
		cmd.pipelineBarrier2(dep);
	}

	// ===== Blur V dispatch =====
	std::array<vk::DescriptorSet, 1> blur_v_sets{*m_blur_v_descriptor_sets[frame]};
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_blur_pipeline_layout, 0, blur_v_sets, {});

	BlurPushConstant blur_push_v{
		.image_size = glm::vec2(static_cast<float>(m_extent.width), static_cast<float>(m_extent.height)),
		.depth_size = glm::vec2(static_cast<float>(m_depth_extent.width), static_cast<float>(m_depth_extent.height)),
		.direction_x = 0,
		.direction_y = 1,
		.sharpness = 16.0f,
		._pad = 0.0f,
	};
	cmd.pushConstants(
		*m_blur_pipeline_layout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(BlurPushConstant), reinterpret_cast<const uint8_t*>(&blur_push_v)));
	cmd.dispatch(groups_x, groups_y, 1);

	// ===== Post-blur barriers =====
	// ao_raw: eGeneral -> eShaderReadOnlyOptimal (fragment reads final blurred AO)
	vk::ImageMemoryBarrier2 ao_final_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = *m_ao_raw_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	// Restore depth to eDepthAttachmentOptimal so downstream consumers
	vk::ImageMemoryBarrier2 depth_to_attachment{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
		.oldLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eDepthAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_depth_image,
		.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
	};
	{
		std::array barriers = {ao_final_to_read, depth_to_attachment};
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
			.pImageMemoryBarriers = barriers.data(),
		};
		cmd.pipelineBarrier2(dep);
	}
}

void GtaoSystem::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D ao_extent,
	vk::Extent2D depth_extent,
	const vk::raii::ImageView& depth_image_view, const vk::raii::Image& depth_image) {
	m_ve_device.getDevice().waitIdle();
	m_extent = ao_extent;
	m_depth_extent = depth_extent;
	m_depth_image = *depth_image;
	m_depth_image_view = *depth_image_view;
	createAoImages(ao_extent);
	createDescriptorSets(descriptor_pool);
}

} // namespace ve
