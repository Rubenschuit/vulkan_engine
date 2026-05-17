#pragma once
#include "ve_export.hpp"
#include "ui/ui_context.hpp"
#include "events/event_bus.hpp"

#include <vulkan/vulkan.hpp>
#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>
#include <functional>

namespace ve {

class VeWindow;
class VeDevice;
class VeRenderer;
struct EditorState;

class VENGINE_API ImGuiLayer {
public:
    ImGuiLayer(VeWindow& window, VeDevice& device, VeRenderer& renderer, EventBus& event_bus);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Call once per frame before your rendering
    void beginFrame();
    // Record UI draw data into the active command buffer.
    // When clear_target is true, the swapchain attachment uses eClear (editor mode, swapchain is fresh).
    void endFrame(vk::raii::CommandBuffer& cmd, bool clear_target = false);

	// Render the UI. appUiCallback renders all panels and app-specific windows.
	void renderUI(UIContext& context, EditorState& editor_state,
	              std::function<void(UIContext&)> appUiCallback = nullptr);

	// Viewport image registration for render-to-texture
	void registerViewportImage(VkSampler sampler, VkImageView image_view, VkImageLayout layout);
	void unregisterViewportImage();
	VkDescriptorSet getViewportTextureId() const { return m_viewport_texture_id; }

	// Set the app-specific settings window name
	void setAppSettingsWindowName(const std::string& name) { m_app_settings_window_name = name; }
	const std::string& getAppSettingsWindowName() const { return m_app_settings_window_name; }

private:
	void renderDockSpace();
	void applyEditorTheme();
    void uploadFonts();
    void recreatePipeline();

    VeDevice& m_device;
    VeRenderer& m_renderer;
    EventBus& m_event_bus;
    VkDescriptorPool m_descriptor_pool = VK_NULL_HANDLE;
    VkFormat m_color_format = VK_FORMAT_UNDEFINED;
	VkDescriptorSet m_viewport_texture_id = VK_NULL_HANDLE;
	std::string m_app_settings_window_name = "Settings";

	EventSubscriptionId m_swap_chain_recreated_sub = 0;
};
}
