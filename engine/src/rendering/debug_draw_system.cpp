#include "pch.hpp"
#include "rendering/debug_draw_system.hpp"
#include "vulkan/ve_device.hpp"
#include "vulkan/ve_pipeline.hpp"
#include "vulkan/ve_buffer.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "events/event_bus.hpp"
#include "events/render_events.hpp"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

uint32_t packColor(const glm::vec3& c) {
	auto to8 = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
	return to8(c.r) | (to8(c.g) << 8) | (to8(c.b) << 16) | (0xFFu << 24);
}

}

DebugDrawSystem::DebugDrawSystem(
	VeDevice& device,
	VeResourceManager& resource_manager,
	const vk::raii::DescriptorSetLayout& global_set_layout,
	vk::Format color_format,
	vk::SampleCountFlagBits sample_count,
	std::filesystem::path line_shader_path,
	std::filesystem::path axes_shader_path,
	EventBus& event_bus)
	: m_ve_device(device),
	  m_line_shader_path(std::move(line_shader_path)),
	  m_axes_shader_path(std::move(axes_shader_path)) {

	event_bus.subscribe<PipelineRecreateEvent>([this](const PipelineRecreateEvent& e) {
		recreatePipelines(e.offscreen_format, e.sample_count);
	});

	createPipelineLayout(global_set_layout);
	createPipelines(color_format, sample_count);
	createAxesMesh(resource_manager);
}

DebugDrawSystem::~DebugDrawSystem() = default;

void DebugDrawSystem::createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout) {
	vk::PipelineLayoutCreateInfo pipeline_layout_info{
		.sType = vk::StructureType::ePipelineLayoutCreateInfo,
		.setLayoutCount = 1,
		.pSetLayouts = &*global_set_layout,
	};
	m_pipeline_layout = vk::raii::PipelineLayout(m_ve_device.getDevice(), pipeline_layout_info);
}

void DebugDrawSystem::createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	PipelineConfigInfo config{};
	VePipeline::defaultPipelineConfigInfo(config, m_ve_device);
	config.multisample_info.rasterizationSamples = sample_count;
	config.input_assembly_info.topology = vk::PrimitiveTopology::eLineList;
	config.depth_stencil_info.depthTestEnable = VK_TRUE;
	config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	config.color_format = color_format;
	config.pipeline_layout = *m_pipeline_layout;
	config.binding_descriptions = {{
		.binding = 0,
		.stride = sizeof(LineVertex),
		.inputRate = vk::VertexInputRate::eVertex,
	}};
	config.attribute_descriptions = {
		{.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(LineVertex, pos)},
		{.location = 1, .binding = 0, .format = vk::Format::eR8G8B8A8Unorm, .offset = offsetof(LineVertex, color)},
	};
	m_line_pipeline = std::make_unique<VePipeline>(m_ve_device, m_line_shader_path, config);

	PipelineConfigInfo axes_config{};
	VePipeline::defaultPipelineConfigInfo(axes_config, m_ve_device);
	axes_config.multisample_info.rasterizationSamples = sample_count;
	axes_config.input_assembly_info.topology = vk::PrimitiveTopology::eTriangleList;
	axes_config.depth_stencil_info.depthTestEnable = VK_TRUE;
	axes_config.depth_stencil_info.depthWriteEnable = VK_FALSE;
	axes_config.rasterization_info.cullMode = vk::CullModeFlagBits::eNone;
	axes_config.color_format = color_format;
	axes_config.pipeline_layout = *m_pipeline_layout;
	m_axes_pipeline = std::make_unique<VePipeline>(m_ve_device, m_axes_shader_path, axes_config);
}

void DebugDrawSystem::createAxesMesh(VeResourceManager& resource_manager) {
	// 3 axes as cylinders from origin to +L along each axis, colored RGB
	std::vector<VeMesh::Vertex> vertices;
	constexpr int SEGMENTS = 16;
	constexpr float L = 5000.0f;
	constexpr float R = 0.05f;
	vertices.reserve(3 * SEGMENTS * 6);

	auto push_tri = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& color) {
		vertices.push_back({a, color});
		vertices.push_back({b, color});
		vertices.push_back({c, color});
	};

	constexpr float two_pi = glm::two_pi<float>();
	for (int axis = 0; axis < 3; ++axis) {
		glm::vec3 col(0.f);
		col[axis] = 1.f;
		glm::vec3 dir(0.f);
		dir[axis] = 1.f;
		// ring axes perpendicular to the cylinder direction
		glm::vec3 e1(0.f), e2(0.f);
		e1[(axis + 1) % 3] = 1.f;
		e2[(axis + 2) % 3] = 1.f;
		for (int i = 0; i < SEGMENTS; ++i) {
			float a0 = two_pi * (static_cast<float>(i) / SEGMENTS);
			float a1 = two_pi * (static_cast<float>(i + 1) / SEGMENTS);
			glm::vec3 r0 = e1 * (R * std::cos(a0)) + e2 * (R * std::sin(a0));
			glm::vec3 r1 = e1 * (R * std::cos(a1)) + e2 * (R * std::sin(a1));
			push_tri(r0, dir * L + r0, dir * L + r1, col);
			push_tri(dir * L + r1, r1, r0, col);
		}
	}
	// Generate dummy indices for draw call. TODO add createMesh() with indices.
	std::vector<uint32_t> indices(vertices.size());
	m_axes_mesh = resource_manager.createMesh("__axes_mesh", vertices, indices);
}

void DebugDrawSystem::addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color) {
	uint32_t packed = packColor(color);
	m_lines.push_back({a, packed});
	m_lines.push_back({b, packed});
}

void DebugDrawSystem::addAabb(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& color) {
	const glm::vec3 v0(mn.x, mn.y, mn.z);
	const glm::vec3 v1(mx.x, mn.y, mn.z);
	const glm::vec3 v2(mx.x, mx.y, mn.z);
	const glm::vec3 v3(mn.x, mx.y, mn.z);
	const glm::vec3 v4(mn.x, mn.y, mx.z);
	const glm::vec3 v5(mx.x, mn.y, mx.z);
	const glm::vec3 v6(mx.x, mx.y, mx.z);
	const glm::vec3 v7(mn.x, mx.y, mx.z);

	addLine(v0, v1, color);
	addLine(v1, v2, color);
	addLine(v2, v3, color);
	addLine(v3, v0, color);
	addLine(v4, v5, color);
	addLine(v5, v6, color);
	addLine(v6, v7, color);
	addLine(v7, v4, color);
	addLine(v0, v4, color);
	addLine(v1, v5, color);
	addLine(v2, v6, color);
	addLine(v3, v7, color);
}

void DebugDrawSystem::addRect(const glm::vec3& center, const glm::vec3& right_half, const glm::vec3& up_half, const glm::vec3& color) {
	glm::vec3 p0 = center - right_half - up_half;
	glm::vec3 p1 = center + right_half - up_half;
	glm::vec3 p2 = center + right_half + up_half;
	glm::vec3 p3 = center - right_half + up_half;
	addLine(p0, p1, color);
	addLine(p1, p2, color);
	addLine(p2, p3, color);
	addLine(p3, p0, color);
}

void DebugDrawSystem::addArrow(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color) {
	addLine(from, to, color);
	glm::vec3 dir = to - from;
	float len = glm::length(dir);
	if (len < 1e-6f)
		return;
	dir /= len;
	glm::vec3 side = std::abs(dir.z) < 0.9f ? glm::cross(dir, glm::vec3(0.f, 0.f, 1.f))
	                                        : glm::cross(dir, glm::vec3(1.f, 0.f, 0.f));
	side = glm::normalize(side);
	glm::vec3 side2 = glm::cross(dir, side);
	glm::vec3 base = to - dir * (0.25f * len);
	float hw = 0.12f * len;
	addLine(to, base + side * hw, color);
	addLine(to, base - side * hw, color);
	addLine(to, base + side2 * hw, color);
	addLine(to, base - side2 * hw, color);
}

void DebugDrawSystem::addVisibleAabbs(const VeFrameInfo& frame_info) {
	constexpr size_t MAX_AABB_BOXES = 2000;
	const glm::vec3 color(1.0f, 0.1f, 0.0f);
	size_t boxes = 0;
	for (auto& entry : frame_info.visible_objects) {
		MeshComponent* mesh_comp = entry.mesh;
		if (!mesh_comp || !mesh_comp->hasMesh())
			continue;
		VeMesh::AABB world_aabb = mesh_comp->getWorldAABB();
		addAabb(world_aabb.min, world_aabb.max, color);
		if (++boxes >= MAX_AABB_BOXES)
			break;
	}
}

void DebugDrawSystem::addAreaLightGizmos(const VeFrameInfo& frame_info) {
	if (!frame_info.registry)
		return;
	Registry& registry = *frame_info.registry;

	for (auto [entity, al, tc] : registry.view<AreaLightComponent, TransformComponent>()) {
		if (!al.getShowGizmo())
			continue;

		const glm::mat4& world = registry.getWorldTransform(entity);
		AreaLightBasis basis = areaLightWorldBasis(world);
		glm::vec3 hr = basis.right_half;
		glm::vec3 hu = basis.up_half;
		float rlen = glm::length(hr);
		float ulen = glm::length(hu);
		if (rlen < 1e-6f || ulen < 1e-6f)
			continue;
		glm::vec3 center = glm::vec3(world[3]);
		glm::vec3 unit_r = hr / rlen;
		glm::vec3 unit_u = hu / ulen;
		glm::vec3 color = al.getColor();
		float s = 2.0f * std::min(rlen, ulen);

		addRect(center, hr, hu, color);

		// Corner brackets
		float inset = 0.08f * s;
		float bracket = 0.16f * s;
		for (int cx = -1; cx <= 1; cx += 2)
			for (int cy = -1; cy <= 1; cy += 2) {
				glm::vec3 corner = center + hr * float(cx) + hu * float(cy);
				glm::vec3 in_r = unit_r * float(-cx);
				glm::vec3 in_u = unit_u * float(-cy);
				glm::vec3 ci = corner + (in_r + in_u) * inset;
				addLine(ci, ci + in_r * bracket, color);
				addLine(ci, ci + in_u * bracket, color);
			}

		// Emit-direction arrow(s)
		glm::vec3 emit_dir = glm::normalize(glm::cross(unit_u, unit_r));
		float arrow_len = 0.4f * s;
		addArrow(center, center + emit_dir * arrow_len, color);
		if (al.getTwoSided())
			addArrow(center, center - emit_dir * arrow_len, color);
		else {
			glm::vec3 back = center - emit_dir * (0.02f * s);
			glm::vec3 dr = unit_r * (0.08f * s);
			glm::vec3 du = unit_u * (0.08f * s);
			addLine(back - dr - du, back + dr + du, color);
			addLine(back - dr + du, back + dr - du, color);
		}
	}
}

void DebugDrawSystem::renderAxes(VeFrameInfo& frame_info) const {
	frame_info.cmd().bindPipeline(vk::PipelineBindPoint::eGraphics, m_axes_pipeline->getPipeline());
	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*frame_info.global_descriptor_set},
		{}
	);
	m_axes_mesh->bindVertexBuffer(frame_info.cmd());
	m_axes_mesh->draw(frame_info.cmd());
}

void DebugDrawSystem::render(VeFrameInfo& frame_info) {
	if (m_lines.empty())
		return;

	uint32_t frame = frame_info.current_frame;
	uint32_t count = static_cast<uint32_t>(m_lines.size());
	if (count > m_buffer_capacity[frame]) {
		uint32_t new_capacity = std::max(count * 2, 4096u);
		m_vertex_buffers[frame] = std::make_unique<VeBuffer>(
			m_ve_device,
			sizeof(LineVertex),
			new_capacity,
			vk::BufferUsageFlagBits::eVertexBuffer,
			vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
			1
		);
		m_vertex_buffers[frame]->map();
		m_buffer_capacity[frame] = new_capacity;
	}
	m_vertex_buffers[frame]->writeToBuffer(m_lines.data(), count * sizeof(LineVertex));

	frame_info.cmd().bindPipeline(vk::PipelineBindPoint::eGraphics, m_line_pipeline->getPipeline());
	frame_info.cmd().bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		*m_pipeline_layout,
		0,
		{*frame_info.global_descriptor_set},
		{}
	);
	vk::Buffer buffers[] = {m_vertex_buffers[frame]->getBuffer()};
	vk::DeviceSize offsets[] = {0};
	frame_info.cmd().bindVertexBuffers(0, buffers, offsets);
	frame_info.cmd().draw(count, 1, 0, 0);

	m_lines.clear();
}

void DebugDrawSystem::recreatePipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
	m_line_pipeline.reset();
	m_axes_pipeline.reset();
	createPipelines(color_format, sample_count);
}

} // namespace ve