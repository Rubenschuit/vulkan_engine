#include "pch.hpp"
#include "rendering/skybox_render_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "resources/ve_resource_manager.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_log.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"

#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>

namespace ve {

struct SkyboxPushConstantData {
	alignas(16) glm::mat4 transform;
	alignas(16) glm::vec4 params;  // x=exposure, yzw=tint
};
static_assert(sizeof(SkyboxPushConstantData) == 80, "SkyboxPushConstantData size mismatch");

SkyboxRenderSystem::SkyboxRenderSystem(
	VeDevice& device,
	VeResourceManager& resource_manager,
	VeDescriptorPool& descriptor_pool,
	VeDescriptorSetLayout& material_set_layout,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	std::filesystem::path skybox_base_path,
	std::filesystem::path shader_path,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	EventBus& event_bus)
	: m_ve_device(device),
	  m_event_bus(event_bus),
	  m_resource_manager(resource_manager),
	  m_descriptor_pool(descriptor_pool),
	  m_material_set_layout(material_set_layout),
	  m_skybox_base_path(std::move(skybox_base_path)),
	  m_shader_path(std::move(shader_path)) {

	discoverSkyboxes();
	createPipelineLayout(global_set_layout, material_set_layout.getDescriptorSetLayout());
	createPipeline(color_format, sample_count);
	createCubeMesh(resource_manager);

	m_event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		recreatePipeline(e.offscreen_format, e.sample_count);
	});

	if (!m_available_skyboxes.empty()) {
		size_t default_index = 0;
		for (size_t i = 0; i < m_available_skyboxes.size(); ++i) {
			if (m_available_skyboxes[i].display_name == "clouds") {
				default_index = i;
				break;
			}
		}
		loadSkyboxTexture(m_available_skyboxes[default_index].path);
		m_current_index = default_index;
		m_event_bus.emitImmediate(SkyboxChangedEvent{m_available_skyboxes[default_index].path});
	} else {
		VE_LOGW("No skybox textures found in " << m_skybox_base_path.generic_string());
	}
}

SkyboxRenderSystem::~SkyboxRenderSystem() {}

void SkyboxRenderSystem::discoverSkyboxes() {
	m_available_skyboxes.clear();
	if (!std::filesystem::exists(m_skybox_base_path)) {
		VE_LOGW("Skybox base path does not exist: " << m_skybox_base_path.generic_string());
		return;
	}
	for (auto it = std::filesystem::recursive_directory_iterator(m_skybox_base_path);
	     it != std::filesystem::recursive_directory_iterator{};
	     ++it) {
		if (!it->is_regular_file())
			continue;
		auto ext = it->path().extension();
		if (ext != ".ktx" && ext != ".ktx2")
			continue;
		auto path = it->path().lexically_normal();
		std::string stem = path.stem().generic_string();

		// Only include skybox cubemap files (must end with _skybox)
		if (stem.size() <= 7 || stem.compare(stem.size() - 7, 7, "_skybox") != 0)
			continue;

		// Display name: strip _skybox suffix for cmgen skybox files
		std::string display_name = stem;
		if (display_name.size() > 7 && display_name.compare(display_name.size() - 7, 7, "_skybox") == 0)
			display_name = display_name.substr(0, display_name.size() - 7);

		// Check for IBL companion files (sh.txt + _ibl.ktx in same directory)
		auto parent = path.parent_path();
		std::string base_name = display_name;
		bool has_ibl = std::filesystem::exists(parent / (base_name + "_ibl.ktx"))
			&& std::filesystem::exists(parent / "sh.txt");

		m_available_skyboxes.push_back({path, display_name, has_ibl});
	}
	std::sort(m_available_skyboxes.begin(), m_available_skyboxes.end(),
		[](const SkyboxEntry& a, const SkyboxEntry& b) { return a.display_name < b.display_name; });
	VE_LOGD("Discovered " << m_available_skyboxes.size() << " skybox textures");
}

void SkyboxRenderSystem::loadSkyboxTexture(const std::filesystem::path& path) {
	m_skybox_handle = m_resource_manager.load<VeTexture>(path.lexically_normal().generic_string());
	auto cubemap_image_info = m_skybox_handle.get()->getDescriptorInfo();
	auto writer = VeDescriptorWriter(m_material_set_layout, m_descriptor_pool).writeImage(0, &cubemap_image_info);
	if (m_has_cubemap_descriptor)
		writer.overwrite(m_cubemap_descriptor_set);
	else {
		writer.build(m_cubemap_descriptor_set);
		m_has_cubemap_descriptor = true;
	}
}

void SkyboxRenderSystem::setSkybox(size_t index) {
	if (index >= m_available_skyboxes.size())
		return;
	if (index == m_current_index && !m_pending_load.has_value())
		return;
	m_pending_load = index;
}

void SkyboxRenderSystem::createCubeMesh(VeResourceManager& resource_manager) {
	constexpr float s = 4.0f * 1500.0f;
	constexpr float u = 1.0f;
	const glm::vec4 t0{0.0f};
	std::vector<VeMesh::Vertex> verts = {
		// +X
		{{ u, -u, -u}, { 1,  0,  0}, {0, 0}, t0},
		{{ u,  u, -u}, { 1,  0,  0}, {1, 0}, t0},
		{{ u,  u,  u}, { 1,  0,  0}, {1, 1}, t0},
		{{ u, -u,  u}, { 1,  0,  0}, {0, 1}, t0},
		// -X
		{{-u,  u, -u}, {-1,  0,  0}, {0, 0}, t0},
		{{-u, -u, -u}, {-1,  0,  0}, {1, 0}, t0},
		{{-u, -u,  u}, {-1,  0,  0}, {1, 1}, t0},
		{{-u,  u,  u}, {-1,  0,  0}, {0, 1}, t0},
		// +Y
		{{ u,  u, -u}, { 0,  1,  0}, {0, 0}, t0},
		{{-u,  u, -u}, { 0,  1,  0}, {1, 0}, t0},
		{{-u,  u,  u}, { 0,  1,  0}, {1, 1}, t0},
		{{ u,  u,  u}, { 0,  1,  0}, {0, 1}, t0},
		// -Y
		{{-u, -u, -u}, { 0, -1,  0}, {0, 0}, t0},
		{{ u, -u, -u}, { 0, -1,  0}, {1, 0}, t0},
		{{ u, -u,  u}, { 0, -1,  0}, {1, 1}, t0},
		{{-u, -u,  u}, { 0, -1,  0}, {0, 1}, t0},
		// +Z
		{{-u, -u,  u}, { 0,  0,  1}, {0, 0}, t0},
		{{ u, -u,  u}, { 0,  0,  1}, {1, 0}, t0},
		{{ u,  u,  u}, { 0,  0,  1}, {1, 1}, t0},
		{{-u,  u,  u}, { 0,  0,  1}, {0, 1}, t0},
		// -Z
		{{-u,  u, -u}, { 0,  0, -1}, {0, 0}, t0},
		{{ u,  u, -u}, { 0,  0, -1}, {1, 0}, t0},
		{{ u, -u, -u}, { 0,  0, -1}, {1, 1}, t0},
		{{-u, -u, -u}, { 0,  0, -1}, {0, 1}, t0},
	};
	std::vector<uint32_t> indices;
	indices.reserve(36);
	for (uint32_t f = 0; f < 6; ++f) {
		uint32_t b = f * 4;
		indices.push_back(b + 0);
		indices.push_back(b + 1);
		indices.push_back(b + 2);
		indices.push_back(b + 0);
		indices.push_back(b + 2);
		indices.push_back(b + 3);
	}
	m_cube_mesh = resource_manager.createMesh("engine::skybox_cube", verts, indices);
	m_cube_transform.setScale({s, s, s});
}

void SkyboxRenderSystem::createPipelineLayout(
	const vk::raii::DescriptorSetLayout& global_set_layout,
	const vk::raii::DescriptorSetLayout& material_set_layout) {

	vk::PushConstantRange push_constant_range{
		.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		.offset = 0,
		.size = sizeof(SkyboxPushConstantData)
	};
	vk::DescriptorSetLayout layouts[2] = {*global_set_layout, *material_set_layout};
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 2,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void SkyboxRenderSystem::createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo pipeline_config{};
	VePipeline::defaultPipelineConfigInfo(pipeline_config, m_ve_device);
	pipeline_config.multisample_info.rasterizationSamples = sample_count;

	pipeline_config.color_format = color_format;
	pipeline_config.rasterization_info.cullMode = vk::CullModeFlagBits::eFront;
	pipeline_config.depth_stencil_info.depthWriteEnable = VK_TRUE;
	pipeline_config.depth_stencil_info.depthCompareOp = vk::CompareOp::eGreaterOrEqual;
	auto attribute_descriptions = VeMesh::Vertex::getAttributeDescriptionsSimple();
	pipeline_config.attribute_descriptions = {attribute_descriptions[0]};

	pipeline_config.pipeline_layout = *m_pipeline_layout;
	m_ve_pipeline = std::make_unique<VePipeline>(
		m_ve_device,
		m_shader_path,
		pipeline_config
	);
	assert(m_ve_pipeline && "Failed to create skybox pipeline");
}

// Draws a big cube and binds cubemap texture for shader
// TODO: move update logic to a separate function
void SkyboxRenderSystem::processPendingLoad() {
	if (!m_pending_load.has_value())
		return;
	m_ve_device.getDevice().waitIdle();
	size_t idx = *m_pending_load;
	loadSkyboxTexture(m_available_skyboxes[idx].path);
	m_current_index = idx;
	m_pending_load = std::nullopt;
	m_event_bus.emitImmediate(SkyboxChangedEvent{m_available_skyboxes[idx].path});
}

void SkyboxRenderSystem::render(VeFrameInfo& frame_info) {
	if (!m_has_cubemap_descriptor)
		return;

	frame_info.cmd().bindPipeline(vk::PipelineBindPoint::eGraphics, m_ve_pipeline->getPipeline());
	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		{},
		{*frame_info.global_descriptor_set, *frame_info.cubemap_descriptor_set},
		{}
	);

	assert(m_cube_mesh.isValid() && "Cube mesh must be loaded");
	VeMesh* cube = m_cube_mesh.get();

	if (m_settings.rotate) {
		float speed = 0.004f;
		glm::vec3 euler_delta{-speed * frame_info.frame_time, 0.2f * speed * frame_info.frame_time, 0.0f};
		m_cube_transform.setRotation(m_cube_transform.getRotation() * glm::quat_cast(glm::eulerAngleZYX(euler_delta.z, euler_delta.y, euler_delta.x)));
	}

	SkyboxPushConstantData push{};
	push.transform = m_cube_transform.getTransform();
	push.params.x = m_settings.exposure;
	// Day: slight warm tint, Night: cool tint
	push.params.y = m_settings.is_day ? 1.05f : 0.9f;   // R
	push.params.z = m_settings.is_day ? 1.0f : 0.95f;   // G
	push.params.w = m_settings.is_day ? 0.95f : 1.1f;   // B
	frame_info.cmd().pushConstants(
		*m_pipeline_layout,
		vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		0,
		vk::ArrayProxy<const uint8_t>(sizeof(SkyboxPushConstantData), reinterpret_cast<const uint8_t*>(&push))
	);

	cube->bindVertexBuffer(frame_info.cmd());
	cube->bindIndexBuffer(frame_info.cmd());
	cube->drawIndexed(frame_info.cmd());
}

} // namespace ve
