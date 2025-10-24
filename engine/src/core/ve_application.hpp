#pragma once

#include "core/ve_window.hpp"
#include "core/ve_device.hpp"
#include "core/ve_renderer.hpp"
#include "ui/imgui_layer.hpp"
#include "core/ve_buffer.hpp"
#include "core/ve_descriptors.hpp"
#include "input/input_controller.hpp"
#include "game/ve_camera.hpp"
#include "game/ve_frame_info.hpp"

#include <filesystem>
#include <memory>
#include <vector>
#include <chrono>

namespace ve {

class VENGINE_API VeApplication {
public:

	// Move this these as constructors parameters together with working_directory
	static constexpr int WIDTH = 1920;
	static constexpr int HEIGHT = 1080;
	const char* APP_NAME = "Vulkan Engine!";

	VeApplication();
    virtual ~VeApplication() = default;

    // Pure virtual method to be implemented by derived classes
    virtual void run() = 0;

protected:
	void updateCamera();
	void updateUniformBuffer(uint32_t current_frame, UniformBufferObject& ubo);
	void updateWindowTitle();
	void updateFrameTime();

	VeWindow m_ve_window;
	VeDevice m_ve_device;
	VeRenderer m_ve_renderer;
	std::unique_ptr<ImGuiLayer> imgui_layer{}; // created in cpp
	std::vector<std::unique_ptr<VeBuffer>> m_uniform_buffers{};

	// Descriptor pool, layouts, sets
	std::shared_ptr<VeDescriptorPool> m_global_pool{};

	std::unique_ptr<VeDescriptorSetLayout> m_global_set_layout{};
	std::unique_ptr<VeDescriptorSetLayout> m_material_set_layout{};

	std::vector<vk::raii::DescriptorSet> m_global_descriptor_sets{};
	vk::raii::DescriptorSet m_material_descriptor_set{nullptr};
	vk::raii::DescriptorSet m_cubemap_descriptor_set{nullptr};

	// Input handling
	InputController m_input_controller;

	// Camera settings
	VeCamera m_camera;
	float m_fov = glm::radians(80.0f);
	float m_near_plane = 0.1f;
	float m_far_plane = 100000.0f;
	float m_last_aspect{0.0f};

	// FPS/frametime tracking
	using clock = std::chrono::steady_clock;
	clock::time_point m_last_frame_time{clock::now()};
	clock::time_point m_fps_window_start{clock::now()};
	uint32_t m_fps_frame_count{0};
	double m_sum_frame_ms{0.0};
	float m_frame_time{0.0f};

	// Window title update settings
	static constexpr std::chrono::milliseconds WINDOW_TITLE_UPDATE_INTERVAL{100};

private:
	void updateFPSStats();
	bool shouldUpdateWindowTitle() const;
	std::string formatWindowTitle(double fps, double avg_ms) const;

};

} // namespace ve

