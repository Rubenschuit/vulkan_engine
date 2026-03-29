#include "pch.hpp"
#include "rendering/outline_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_image.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "platform/ve_file_system.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#include <algorithm>
#include <functional>

namespace ve {

struct MaskPushConstant {
	alignas(4) uint32_t instance_offset;
};
static_assert(sizeof(MaskPushConstant) == 4);

struct JfaInitPushConstant {
	alignas(8) glm::ivec2 image_size;
};
static_assert(sizeof(JfaInitPushConstant) == 8);

struct JfaStepPushConstant {
	alignas(8) glm::ivec2 image_size;
	alignas(4) int step_size;
	alignas(4) int _pad;
};
static_assert(sizeof(JfaStepPushConstant) == 16);

struct OutlineCompositePushConstant {
	alignas(8) glm::vec2 image_size;
	alignas(4) float outline_width;
	alignas(4) float _pad;
	alignas(16) glm::vec4 outline_color;
};
static_assert(sizeof(OutlineCompositePushConstant) == 32);

OutlineSystem::OutlineSystem(
	VeDevice& device,
	VeDescriptorPool& descriptor_pool,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	std::filesystem::path shader_path,
	vk::Extent2D extent,
	vk::Format composite_color_format,
	EventBus& event_bus)
	: m_ve_device(device), m_shader_path(std::move(shader_path)), m_extent(extent) {

	event_bus.subscribe<ResolutionChangedEvent>([this](const ResolutionChangedEvent& e) {
		recreate(e.pool, e.extent, e.swap_chain_format);
	});

	createImages(extent);
	createSampler();
	createMaskPipelineLayout(global_set_layout);
	createMaskPipeline();
	createJfaSetLayouts();
	createJfaPipelineLayouts();
	createJfaPipelines();
	createCompositeSetLayout();
	createCompositePipelineLayout();
	createCompositePipeline(composite_color_format);
	createDescriptorSets(descriptor_pool);
}

OutlineSystem::~OutlineSystem() = default;

void OutlineSystem::createImages(vk::Extent2D extent) {
	auto make_mask = [&]() {
		auto img = std::make_unique<VeImage>(
			m_ve_device, extent.width, extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR8Unorm,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		img->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::AccessFlagBits2::eNone,
			vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eFragmentShader);
		return img;
	};

	auto make_jfa = [&]() {
		auto img = std::make_unique<VeImage>(
			m_ve_device, extent.width, extent.height,
			vk::SampleCountFlagBits::e1,
			vk::Format::eR32G32Sint,
			vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
			vk::MemoryPropertyFlagBits::eDeviceLocal,
			vk::ImageAspectFlagBits::eColor,
			false, 1);
		img->transitionImageLayout(
			vk::ImageLayout::eUndefined,
			vk::ImageLayout::eGeneral,
			vk::AccessFlagBits2::eNone,
			vk::AccessFlagBits2::eShaderStorageWrite,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eComputeShader);
		return img;
	};

	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		m_mask_images[i] = make_mask();
		m_jfa_images_a[i] = make_jfa();
		m_jfa_images_b[i] = make_jfa();
	}
}

void OutlineSystem::createSampler() {
	vk::SamplerCreateInfo sampler_info{
		.magFilter = vk::Filter::eNearest,
		.minFilter = vk::Filter::eNearest,
		.mipmapMode = vk::SamplerMipmapMode::eNearest,
		.addressModeU = vk::SamplerAddressMode::eClampToEdge,
		.addressModeV = vk::SamplerAddressMode::eClampToEdge,
		.addressModeW = vk::SamplerAddressMode::eClampToEdge,
		.mipLodBias = 0.0f,
		.anisotropyEnable = VK_FALSE,
		.compareEnable = VK_FALSE,
		.minLod = 0.0f,
		.maxLod = 0.0f,
		.borderColor = vk::BorderColor::eIntOpaqueBlack,
		.unnormalizedCoordinates = VK_FALSE,
	};
	m_nearest_sampler = vk::raii::Sampler(m_ve_device.getDevice(), sampler_info);
}

void OutlineSystem::createMaskPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex,
		.offset = 0,
		.size = sizeof(MaskPushConstant),
	};
	vk::DescriptorSetLayout layouts[1] = {*global_set_layout};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	m_mask_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void OutlineSystem::createMaskPipeline() {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);

	config.attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsShadow();
	config.binding_descriptions = VeMesh::Vertex::getShadowBindingDescriptions();
	config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	config.color_format = vk::Format::eR8Unorm;
	config.depth_stencil_info.depthTestEnable = VK_FALSE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eBack;
	config.color_blend_attachment.blendEnable = VK_FALSE;

	config.dynamic_state_enables.push_back(vk::DynamicState::eCullMode);
	config.dynamic_state_info.dynamicStateCount =
		static_cast<uint32_t>(config.dynamic_state_enables.size());
	config.dynamic_state_info.pDynamicStates = config.dynamic_state_enables.data();

	config.pipeline_layout = *m_mask_pipeline_layout;
	m_mask_pipeline = std::make_unique<VePipeline>(
		m_ve_device, m_shader_path / "outline_mask.spv", config);
}

void OutlineSystem::createJfaSetLayouts() {
	m_jfa_init_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)
		.build();

	m_jfa_step_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eSampledImage, vk::ShaderStageFlagBits::eCompute)
		.addBinding(1, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute)
		.build();
}

void OutlineSystem::createJfaPipelineLayouts() {
	{
		vk::PushConstantRange push_range{
			.stageFlags = vk::ShaderStageFlagBits::eCompute,
			.offset = 0,
			.size = sizeof(JfaInitPushConstant),
		};
		vk::DescriptorSetLayout layouts[1] = {m_jfa_init_set_layout->getDescriptorSetLayout()};
		vk::PipelineLayoutCreateInfo layout_info{
			.setLayoutCount = 1,
			.pSetLayouts = layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		m_jfa_init_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
	}
	{
		vk::PushConstantRange push_range{
			.stageFlags = vk::ShaderStageFlagBits::eCompute,
			.offset = 0,
			.size = sizeof(JfaStepPushConstant),
		};
		vk::DescriptorSetLayout layouts[1] = {m_jfa_step_set_layout->getDescriptorSetLayout()};
		vk::PipelineLayoutCreateInfo layout_info{
			.setLayoutCount = 1,
			.pSetLayouts = layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		m_jfa_step_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
	}
}

void OutlineSystem::createJfaPipelines() {
	auto load_module = [&](const std::filesystem::path& path) {
		auto code = VeFileSystem::readFile(path);
		vk::ShaderModuleCreateInfo info{
			.codeSize = code.size(),
			.pCode = reinterpret_cast<const uint32_t*>(code.data()),
		};
		return vk::raii::ShaderModule(m_ve_device.getDevice(), info);
	};

	m_jfa_init_module = load_module(m_shader_path / "jfa_init_comp.spv");
	{
		vk::PipelineShaderStageCreateInfo stage{
			.stage = vk::ShaderStageFlagBits::eCompute,
			.module = *m_jfa_init_module,
			.pName = "compMain",
		};
		vk::ComputePipelineCreateInfo pi{.stage = stage, .layout = *m_jfa_init_pipeline_layout};
		m_jfa_init_pipeline = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pi);
	}

	m_jfa_step_module = load_module(m_shader_path / "jfa_step_comp.spv");
	{
		vk::PipelineShaderStageCreateInfo stage{
			.stage = vk::ShaderStageFlagBits::eCompute,
			.module = *m_jfa_step_module,
			.pName = "compMain",
		};
		vk::ComputePipelineCreateInfo pi{.stage = stage, .layout = *m_jfa_step_pipeline_layout};
		m_jfa_step_pipeline = vk::raii::Pipeline(m_ve_device.getDevice(), nullptr, pi);
	}
}

void OutlineSystem::createCompositeSetLayout() {
	m_composite_set_layout = VeDescriptorSetLayout::Builder(m_ve_device)
		.addBinding(0, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.addBinding(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment)
		.build();
}

void OutlineSystem::createCompositePipelineLayout() {
	vk::PushConstantRange push_range{
		.stageFlags = vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(OutlineCompositePushConstant),
	};
	vk::DescriptorSetLayout layouts[1] = {m_composite_set_layout->getDescriptorSetLayout()};
	vk::PipelineLayoutCreateInfo layout_info{
		.setLayoutCount = 1,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	m_composite_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), layout_info);
}

void OutlineSystem::createCompositePipeline(vk::Format color_format) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = vk::SampleCountFlagBits::e1;
	config.color_format = color_format;
	config.pipeline_layout = *m_composite_pipeline_layout;
	config.attribute_descriptions.clear();
	config.binding_descriptions.clear();
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.depth_stencil_info.depthTestEnable = VK_FALSE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;

	m_composite_pipeline = std::make_unique<VePipeline>(
		m_ve_device, m_shader_path / "outline_composite.spv", config);
}

void OutlineSystem::createDescriptorSets(VeDescriptorPool& descriptor_pool) {
	for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
		vk::DescriptorImageInfo mask_sampled{
			.imageView = *m_mask_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo jfa_a_storage{
			.imageView = *m_jfa_images_a[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo jfa_a_sampled{
			.imageView = *m_jfa_images_a[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo jfa_b_storage{
			.imageView = *m_jfa_images_b[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eGeneral,
		};
		vk::DescriptorImageInfo jfa_b_sampled{
			.imageView = *m_jfa_images_b[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};

		VeDescriptorWriter(*m_jfa_init_set_layout, descriptor_pool)
			.writeImage(0, &mask_sampled)
			.writeImage(1, &jfa_a_storage)
			.build(m_jfa_init_sets[frame]);

		VeDescriptorWriter(*m_jfa_step_set_layout, descriptor_pool)
			.writeImage(0, &jfa_a_sampled)
			.writeImage(1, &jfa_b_storage)
			.build(m_jfa_step_a_to_b_sets[frame]);

		VeDescriptorWriter(*m_jfa_step_set_layout, descriptor_pool)
			.writeImage(0, &jfa_b_sampled)
			.writeImage(1, &jfa_a_storage)
			.build(m_jfa_step_b_to_a_sets[frame]);

		vk::DescriptorImageInfo jfa_a_combined{
			.sampler = *m_nearest_sampler,
			.imageView = *m_jfa_images_a[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		vk::DescriptorImageInfo mask_combined{
			.sampler = *m_nearest_sampler,
			.imageView = *m_mask_images[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		VeDescriptorWriter(*m_composite_set_layout, descriptor_pool)
			.writeImage(0, &jfa_a_combined)
			.writeImage(1, &mask_combined)
			.build(m_composite_a_sets[frame]);

		vk::DescriptorImageInfo jfa_b_combined{
			.sampler = *m_nearest_sampler,
			.imageView = *m_jfa_images_b[frame]->getImageView(),
			.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		};
		VeDescriptorWriter(*m_composite_set_layout, descriptor_pool)
			.writeImage(0, &jfa_b_combined)
			.writeImage(1, &mask_combined)
			.build(m_composite_b_sets[frame]);
	}
}

void OutlineSystem::renderMask(VeFrameInfo& fi, Registry& registry, Entity root_entity) {
	if (root_entity.isNull() || !registry.isAlive(root_entity)) {
		m_has_outline = false;
		return;
	}

	// Recursively collect all mesh entities under the root entity
	std::vector<Entity> mesh_entities;
	std::function<void(Entity)> collect = [&](Entity e) {
		if (registry.hasComponent<MeshComponent>(e))
			mesh_entities.push_back(e);
		Entity child = registry.firstChild(e);
		while (!child.isNull()) {
			collect(child);
			child = registry.nextSibling(child);
		}
	};
	collect(root_entity);

	if (mesh_entities.empty()) {
		m_has_outline = false;
		return;
	}
	m_has_outline = true;

	// Write instance data for all mesh entities into the instance buffer
	uint32_t mask_instance_start = fi.instance_count;
	for (Entity e : mesh_entities) {
		if (fi.instance_count >= fi.instance_capacity)
			break;
		uint32_t idx = fi.instance_count++;
		fi.instance_data[idx].transform = registry.getWorldTransform(e);
		// No normals required for mask pass.
		fi.instance_data[idx].normal_transform[0] = glm::vec4(0.0f);
		fi.instance_data[idx].normal_transform[1] = glm::vec4(0.0f);
		fi.instance_data[idx].normal_transform[2] = glm::vec4(0.0f);
	}

	auto& cmd = fi.cmd();
	uint32_t frame = fi.current_frame;

	vk::ImageMemoryBarrier2 mask_to_attachment{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_mask_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = 1, 
			.pImageMemoryBarriers = &mask_to_attachment
		};
		cmd.pipelineBarrier2(dep);
	}

	vk::RenderingAttachmentInfo color_att{
		.imageView = *m_mask_images[frame]->getImageView(),
		.imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.loadOp = vk::AttachmentLoadOp::eClear,
		.storeOp = vk::AttachmentStoreOp::eStore,
		.clearValue = vk::ClearColorValue(std::array{0.0f, 0.0f, 0.0f, 0.0f}),
	};
	vk::RenderingInfo rendering_info{
		.renderArea = {.offset = {0, 0}, .extent = m_extent},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_att,
	};
	cmd.beginRendering(rendering_info);
	cmd.setViewport(0, vk::Viewport{
		.x = 0.0f, .y = 0.0f,
		.width = static_cast<float>(m_extent.width),
		.height = static_cast<float>(m_extent.height),
		.minDepth = 0.0f, .maxDepth = 1.0f
	});
	cmd.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = m_extent});

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_mask_pipeline->getPipeline());
	auto& descriptor_set = fi.cpu_global_descriptor_set ? *fi.cpu_global_descriptor_set : fi.global_descriptor_set;
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_mask_pipeline_layout,
		0, {*descriptor_set}, {});

	uint32_t instance_idx = mask_instance_start;
	VeMesh* bound_mesh = nullptr;
	for (Entity e : mesh_entities) {
		auto* mc = registry.getComponent<MeshComponent>(e);
		VeMesh* mesh = mc->getMesh();
		if (!mesh) {
			instance_idx++;
			continue;
		}

		auto* mat = mc->getMaterial();
		if (mat && mat->getAlphaProps().double_sided)
			cmd.setCullMode(vk::CullModeFlagBits::eNone);
		else
			cmd.setCullMode(vk::CullModeFlagBits::eBack);

		MaskPushConstant push{.instance_offset = instance_idx};
		cmd.pushConstants(
			*m_mask_pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
			vk::ArrayProxy<const uint8_t>(sizeof(push), reinterpret_cast<const uint8_t*>(&push)));

		if (mesh != bound_mesh) {
			bound_mesh = mesh;
			mesh->bindShadowVertexBuffer(cmd);
			mesh->bindLodIndexBuffer(cmd, 0);
		}
		mesh->drawIndexedLod(cmd, 0, 1, 0);
		instance_idx++;
	}

	cmd.endRendering();

	vk::ImageMemoryBarrier2 mask_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_mask_images[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &mask_to_read};
		cmd.pipelineBarrier2(dep);
	}
}

void OutlineSystem::dispatchJFA(VeFrameInfo& fi, float outline_width) {
	if (!m_has_outline)
		return;

	auto& cmd = fi.cmd();
	uint32_t frame = fi.current_frame;
	uint32_t w = m_extent.width, h = m_extent.height;
	uint32_t groups_x = (w + 15) / 16;
	uint32_t groups_y = (h + 15) / 16;

	// JFA Init: write seed coordinates into jfa_a
	vk::ImageMemoryBarrier2 jfa_a_to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eUndefined,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = m_jfa_images_a[frame]->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		vk::DependencyInfo dep{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &jfa_a_to_general};
		cmd.pipelineBarrier2(dep);
	}

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_jfa_init_pipeline);
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
		*m_jfa_init_pipeline_layout, 0, {*m_jfa_init_sets[frame]}, {});

	JfaInitPushConstant init_push{.image_size = glm::ivec2(w, h)};
	cmd.pushConstants(*m_jfa_init_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
		vk::ArrayProxy<const uint8_t>(sizeof(init_push), reinterpret_cast<const uint8_t*>(&init_push)));
	cmd.dispatch(groups_x, groups_y, 1);

	// JFA flood passes
	int max_reach = std::max(1, static_cast<int>(std::ceil(outline_width)));
	int step_size = 1;
	while (step_size < max_reach)
		step_size <<= 1;

	bool reading_a = true;

	while (step_size >= 1) {
		auto& src = reading_a ? m_jfa_images_a[frame] : m_jfa_images_b[frame];
		auto& dst = reading_a ? m_jfa_images_b[frame] : m_jfa_images_a[frame];

		vk::ImageMemoryBarrier2 src_barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
			.oldLayout = vk::ImageLayout::eGeneral,
			.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = src->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
		};
		vk::ImageMemoryBarrier2 dst_barrier{
			.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
			.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
			.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			.oldLayout = vk::ImageLayout::eUndefined,
			.newLayout = vk::ImageLayout::eGeneral,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = dst->getImage(),
			.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
		};
		{
			std::array barriers = {src_barrier, dst_barrier};
			vk::DependencyInfo dep{
				.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
				.pImageMemoryBarriers = barriers.data(),
			};
			cmd.pipelineBarrier2(dep);
		}

		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *m_jfa_step_pipeline);
		auto& step_set = reading_a ? m_jfa_step_a_to_b_sets[frame] : m_jfa_step_b_to_a_sets[frame];
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
			*m_jfa_step_pipeline_layout, 0, {*step_set}, {});

		JfaStepPushConstant step_push{.image_size = glm::ivec2(w, h), .step_size = step_size, ._pad = 0};
		cmd.pushConstants(*m_jfa_step_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
			vk::ArrayProxy<const uint8_t>(sizeof(step_push), reinterpret_cast<const uint8_t*>(&step_push)));
		cmd.dispatch(groups_x, groups_y, 1);

		reading_a = !reading_a;
		step_size >>= 1;
	}

	// reading_a was flipped after the last write, so last write went to the buffer
	// that reading_a now points to
	m_final_reads_a = reading_a;

	auto& final_jfa = m_final_reads_a ? m_jfa_images_a[frame] : m_jfa_images_b[frame];
	vk::ImageMemoryBarrier2 final_to_read{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderRead,
		.oldLayout = vk::ImageLayout::eGeneral,
		.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = final_jfa->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	auto& other_jfa = m_final_reads_a ? m_jfa_images_b[frame] : m_jfa_images_a[frame];
	vk::ImageMemoryBarrier2 other_to_general{
		.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.srcAccessMask = vk::AccessFlagBits2::eShaderRead,
		.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		.dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.newLayout = vk::ImageLayout::eGeneral,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = other_jfa->getImage(),
		.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
	};
	{
		std::array barriers = {final_to_read, other_to_general};
		vk::DependencyInfo dep{
			.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
			.pImageMemoryBarriers = barriers.data(),
		};
		cmd.pipelineBarrier2(dep);
	}
}

void OutlineSystem::composite(
	vk::raii::CommandBuffer& cmd, uint32_t frame,
	float outline_width, const glm::vec3& outline_color) {
	if (!m_has_outline)
		return;

	auto& composite_set = m_final_reads_a ? m_composite_a_sets[frame] : m_composite_b_sets[frame];

	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, m_composite_pipeline->getPipeline());
	cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
		*m_composite_pipeline_layout, 0, {*composite_set}, {});

	OutlineCompositePushConstant push{
		.image_size = glm::vec2(m_extent.width, m_extent.height),
		.outline_width = outline_width,
		._pad = 0.0f,
		.outline_color = glm::vec4(outline_color, 1.0f),
	};
	cmd.pushConstants<OutlineCompositePushConstant>(
		*m_composite_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, push);
	cmd.draw(3, 1, 0, 0);
}

void OutlineSystem::recreate(VeDescriptorPool& descriptor_pool, vk::Extent2D extent,
                             vk::Format composite_color_format) {
	// Precondition: device must be idle
	m_ve_device.assertDeviceIdle();
	m_extent = extent;
	createImages(extent);
	m_composite_pipeline.reset();
	createCompositePipeline(composite_color_format);
	createDescriptorSets(descriptor_pool);
}

} // namespace ve