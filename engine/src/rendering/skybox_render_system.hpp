/* SkyboxRenderSystem is responsible for rendering a skybox.
 * It creates a pipeline, loads a cube model and renders it.
 * Size is hardcoded in .cpp for now.
 * Owns texture loading, descriptor set, and skybox discovery from directory.
 * Supports rotation toggle, exposure, and day/night tint.
 */

#pragma once
#include "ve_export.hpp"
#include "ve_config.hpp"
#include "rendering/ve_frame_info.hpp"
#include "resources/ve_resource_manager.hpp"
#include "scene/ve_component.hpp"

#include <memory>
#include <vector>
#include <filesystem>
#include <string>
#include <optional>

namespace ve {
	class VeDevice;
	class VePipeline;
	class VeResourceManager;
	class VeDescriptorPool;
	class VeDescriptorSetLayout;
	class EventBus;
}

namespace ve {

struct SkyboxEntry {
	std::filesystem::path path;
	std::string display_name;
	bool has_ibl = false;
};

/** Settings exposed for UI. All mutable by app. */
struct SkyboxSettings {
	bool rotate = false;
	float exposure = 1.0f;
	bool is_day = true;  // day: warmer tint, night: cooler tint
};

class VENGINE_API SkyboxRenderSystem {
public:
	SkyboxRenderSystem(VeDevice& device,
						VeResourceManager& resource_manager,
						VeDescriptorPool& descriptor_pool,
						VeDescriptorSetLayout& material_set_layout,
						const vk::raii::DescriptorSetLayout& global_set_layout,
						std::filesystem::path skybox_base_path,
						std::filesystem::path shader_path,
						const std::filesystem::path& cube_model_path,
						vk::Format color_format,
						vk::SampleCountFlagBits sample_count,
						EventBus& event_bus);
	~SkyboxRenderSystem();

	SkyboxRenderSystem(const SkyboxRenderSystem&) = delete;
	SkyboxRenderSystem& operator=(const SkyboxRenderSystem&) = delete;

	// Process deferred skybox load (call before frame recording begins)
	void processPendingLoad();

	// Slowly rotate the skybox over time and render it
	void render(VeFrameInfo& frame_info);
	void recreatePipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count) {
		m_ve_pipeline.reset();
		createPipeline(color_format, sample_count);
	}

	// Descriptor set for cubemap; used by frame_info.
	vk::raii::DescriptorSet& getCubemapDescriptorSet() { return m_cubemap_descriptor_set; }
	const vk::raii::DescriptorSet& getCubemapDescriptorSet() const { return m_cubemap_descriptor_set; }

	// Available skyboxes discovered from skybox_base_path (.ktx, .ktx2).
	const std::vector<SkyboxEntry>& getAvailableSkyboxes() const { return m_available_skyboxes; }

	// Select skybox by index. Reloads texture if changed.
	void setSkybox(size_t index);
	size_t getCurrentSkyboxIndex() const { return m_current_index; }

	// Mutable settings for UI.
	SkyboxSettings& getSettings() { return m_settings; }
	const SkyboxSettings& getSettings() const { return m_settings; }

	bool isLoading() const { return m_pending_load.has_value(); }

private:
	void discoverSkyboxes();
	void loadSkyboxTexture(const std::filesystem::path& path);
	void loadCubeModel(VeResourceManager& resource_manager, const std::filesystem::path& cube_model_path);
	void createPipelineLayout(const vk::raii::DescriptorSetLayout& global_set_layout, const vk::raii::DescriptorSetLayout& material_set_layout);
	void createPipeline(vk::Format color_format, vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1);

	VeDevice& m_ve_device;
	EventBus& m_event_bus;
	VeResourceManager& m_resource_manager;
	VeDescriptorPool& m_descriptor_pool;
	VeDescriptorSetLayout& m_material_set_layout;
	std::filesystem::path m_skybox_base_path;
	std::filesystem::path m_shader_path;

	vk::raii::PipelineLayout m_pipeline_layout{nullptr};
	std::unique_ptr<VePipeline> m_ve_pipeline;
	ResourceHandle<VeMesh> m_cube_mesh;
	TransformComponent m_cube_transform;

	std::vector<SkyboxEntry> m_available_skyboxes;
	size_t m_current_index = 0;
	std::optional<size_t> m_pending_load;
	ResourceHandle<VeTexture> m_skybox_handle;
	vk::raii::DescriptorSet m_cubemap_descriptor_set{nullptr};
	bool m_has_cubemap_descriptor = false;

	SkyboxSettings m_settings;
};

} // namespace ve
