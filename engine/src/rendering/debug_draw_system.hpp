#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_resource_manager.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace ve {

class VeDevice;
class VePipeline;
class VeBuffer;
class VeMesh;
class EventBus;

// Editor overlay drawing: add*() appends colored lines; render() draws and
// clears it. Also owns the static world-axes mesh.
class VENGINE_API DebugDrawSystem {
public:
	DebugDrawSystem(
		VeDevice& device,
		VeResourceManager& resource_manager,
		const vk::raii::DescriptorSetLayout& global_set_layout,
		vk::Format color_format,
		vk::SampleCountFlagBits sample_count,
		std::filesystem::path line_shader_path,
		std::filesystem::path axes_shader_path,
		EventBus& event_bus);
	~DebugDrawSystem();

	DebugDrawSystem(const DebugDrawSystem&) = delete;
	DebugDrawSystem& operator=(const DebugDrawSystem&) = delete;

	void addLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);
	void addAabb(const glm::vec3& mn, const glm::vec3& mx, const glm::vec3& color);
	void addRect(const glm::vec3& center, const glm::vec3& right_half, const glm::vec3& up_half, const glm::vec3& color);
	void addArrow(const glm::vec3& from, const glm::vec3& to, const glm::vec3& color);

	void addVisibleAabbs(const VeFrameInfo& frame_info);
	void addAreaLightGizmos(const VeFrameInfo& frame_info);

	void renderAxes(VeFrameInfo& frame_info) const;
	void render(VeFrameInfo& frame_info);
	void recreatePipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count);

private:
	struct LineVertex {
		glm::vec3 pos;
		uint32_t color; // packed rgba8
	};

	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout);
	void createPipelines(vk::Format color_format, vk::SampleCountFlagBits sample_count);
	void createAxesMesh(VeResourceManager& resource_manager);

	VeDevice& m_ve_device;
	std::filesystem::path m_line_shader_path;
	std::filesystem::path m_axes_shader_path;
	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_line_pipeline;
	std::unique_ptr<VePipeline> m_axes_pipeline;
	ResourceHandle<VeMesh> m_axes_mesh;

	std::vector<LineVertex> m_lines;
	std::array<std::unique_ptr<VeBuffer>, MAX_FRAMES_IN_FLIGHT> m_vertex_buffers;
	std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_buffer_capacity{}; // in vertices
};

}