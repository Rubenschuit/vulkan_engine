#include "rendering/ve_frame_info.hpp"
#include "pch.hpp"
#include "ui/imgui_layer.hpp"
#include "ui/editor_state.hpp"
#include "platform/ve_window.hpp"
#include "vulkan/ve_device.hpp"
#include "rendering/ve_renderer.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>


namespace ve {

static VkDescriptorPool createImguiDescriptorPool(vk::raii::Device& device) {
	// complete overkill for our use case but whats the harm
    std::array<VkDescriptorPoolSize, 11> pool_sizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
    };
    VkDescriptorPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1000 * static_cast<uint32_t>(pool_sizes.size()),
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data()
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult res = vkCreateDescriptorPool(*device, &pool_info, nullptr, &pool);
    if (res != VK_SUCCESS) {
        throw std::runtime_error("ImGuiLayer: Failed to create descriptor pool");
    }
    return pool;
}

ImGuiLayer::ImGuiLayer(VeWindow& window, VeDevice& device, VeRenderer& renderer)
    : m_device(device), m_renderer(renderer) {
    // Create ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

	// Enable docking
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	// Set style
	applyEditorTheme();

    // Init GLFW backend
    ImGui_ImplGlfw_InitForVulkan(window.getGLFWwindow(), true);

    // Init Vulkan backend with dynamic rendering
    m_descriptor_pool = createImguiDescriptorPool(m_device.getDevice());
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = *m_device.getInstance();
    init_info.PhysicalDevice = *m_device.getPhysicalDevice();
    init_info.Device = *m_device.getDevice();
    init_info.QueueFamily = m_device.getGraphicsQueueFamilyIndex();
    init_info.Queue = *m_device.getQueue();
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = m_descriptor_pool;
    init_info.MinImageCount = static_cast<uint32_t>(m_renderer.getImageCount());
    init_info.ImageCount = static_cast<uint32_t>(m_renderer.getImageCount());
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = nullptr;
    init_info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE; // Using dynamic rendering
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    // Dynamic Rendering
    init_info.UseDynamicRendering = true;
    m_color_format = static_cast<VkFormat>(m_renderer.getSwapChainImageFormat());
    memset(&init_info.PipelineInfoMain.PipelineRenderingCreateInfo, 0, sizeof(init_info.PipelineInfoMain.PipelineRenderingCreateInfo));
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_color_format;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    ImGui_ImplVulkan_Init(&init_info);
}

ImGuiLayer::~ImGuiLayer() {
	unregisterViewportImage();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
	if (m_descriptor_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(*m_device.getDevice(), m_descriptor_pool, nullptr);
		m_descriptor_pool = VK_NULL_HANDLE;
	}
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame(vk::raii::CommandBuffer& cmd, bool clear_target) {
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    // Render on top of the current swapchain image view
    VkImageView color_view = static_cast<VkImageView>(*m_renderer.getSwapChainImageView(m_renderer.getCurrentImageIndex()));
    const vk::RenderingAttachmentInfo color_attachment{
        .imageView = color_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = clear_target ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearColorValue(0.1f, 0.1f, 0.1f, 1.0f)
    };
    const auto extent = m_renderer.getSwapChainExtent();

    const vk::RenderingInfo rendering_info{
        .renderArea = { {0, 0}, extent },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment
    };
    cmd.beginRendering(rendering_info);
    ImGui_ImplVulkan_RenderDrawData(draw_data, *cmd);
    cmd.endRendering();
}

void ImGuiLayer::renderDockSpace() {
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
		ImGuiWindowFlags_NoBackground;

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("DockSpace", nullptr, window_flags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

	// Set default layout if dockspace has no configured layout
	ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
	if (!node || !node->ChildNodes[0]) {
		ImGui::DockBuilderRemoveNode(dockspace_id);
		ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

		ImGuiID dock_main = dockspace_id;
		ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Left, 0.28f, nullptr, &dock_main);
		ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.22f, nullptr, &dock_main);
		ImGuiID dock_left_bottom = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.5f, nullptr, &dock_left);
		ImGuiID dock_right_bottom = ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.45f, nullptr, &dock_right);

		ImGui::DockBuilderDockWindow("Scene Hierarchy", dock_left);
		ImGui::DockBuilderDockWindow(m_app_settings_window_name.c_str(), dock_left);
		ImGui::DockBuilderDockWindow("Viewport", dock_main);
		ImGui::DockBuilderDockWindow("Inspector", dock_left_bottom);
		ImGui::DockBuilderDockWindow("Environment", dock_left_bottom);
		ImGui::DockBuilderDockWindow("Graphics", dock_right);
		ImGui::DockBuilderDockWindow("Performance", dock_right_bottom);
		ImGui::DockBuilderFinish(dockspace_id);
	}

	ImGui::End();
}

void ImGuiLayer::applyEditorTheme() {
	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.TabRounding = 3.0f;
	style.ChildRounding = 3.0f;
	style.PopupRounding = 3.0f;
	style.ScrollbarRounding = 3.0f;
	style.FrameBorderSize = 0.0f;
	style.WindowBorderSize = 1.0f;
	style.WindowPadding = ImVec2(8, 8);
	style.FramePadding = ImVec2(5, 3);
	style.ItemSpacing = ImVec2(8, 4);

	const ImVec4 orange       = ImVec4(0.88f, 0.40f, 0.10f, 1.0f);
	const ImVec4 orange_hover = ImVec4(0.98f, 0.48f, 0.14f, 1.0f);
	const ImVec4 orange_dim   = ImVec4(0.65f, 0.30f, 0.07f, 1.0f);
	const ImVec4 orange_bg    = ImVec4(0.88f, 0.40f, 0.10f, 0.40f);

	auto& colors = style.Colors;

	// Backgrounds
	colors[ImGuiCol_WindowBg]       = ImVec4(0.05f, 0.05f, 0.06f, 1.0f);
	colors[ImGuiCol_ChildBg]        = ImVec4(0.05f, 0.05f, 0.06f, 1.0f);
	colors[ImGuiCol_PopupBg]        = ImVec4(0.04f, 0.04f, 0.05f, 0.95f);
	colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.03f, 0.03f, 0.03f, 1.0f);
	colors[ImGuiCol_MenuBarBg]      = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);

	// Title bar
	colors[ImGuiCol_TitleBg]          = ImVec4(0.03f, 0.03f, 0.04f, 1.0f);
	colors[ImGuiCol_TitleBgActive]    = ImVec4(0.05f, 0.05f, 0.06f, 1.0f);
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.03f, 0.03f, 0.04f, 0.75f);

	// Tabs
	colors[ImGuiCol_Tab]                 = ImVec4(0.07f, 0.07f, 0.08f, 1.0f);
	colors[ImGuiCol_TabHovered]          = orange;
	colors[ImGuiCol_TabSelected]         = orange_dim;
	colors[ImGuiCol_TabSelectedOverline] = orange;
	colors[ImGuiCol_TabDimmed]           = ImVec4(0.05f, 0.05f, 0.06f, 1.0f);
	colors[ImGuiCol_TabDimmedSelected]   = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);

	// Headers
	colors[ImGuiCol_Header]        = orange_bg;
	colors[ImGuiCol_HeaderHovered] = ImVec4(0.88f, 0.40f, 0.10f, 0.65f);
	colors[ImGuiCol_HeaderActive]  = orange;

	// Frame backgrounds
	colors[ImGuiCol_FrameBg]        = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.14f, 0.15f, 1.0f);
	colors[ImGuiCol_FrameBgActive]  = ImVec4(0.18f, 0.18f, 0.19f, 1.0f);

	// Buttons
	colors[ImGuiCol_Button]        = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
	colors[ImGuiCol_ButtonHovered] = orange;
	colors[ImGuiCol_ButtonActive]  = orange_hover;

	// Sliders / grabs
	colors[ImGuiCol_SliderGrab]       = orange_dim;
	colors[ImGuiCol_SliderGrabActive] = orange;
	colors[ImGuiCol_CheckMark]        = orange;

	// Scrollbar
	colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.04f, 0.04f, 0.05f, 0.5f);
	colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.24f, 0.24f, 0.25f, 1.0f);
	colors[ImGuiCol_ScrollbarGrabActive]  = orange_dim;

	// Separators
	colors[ImGuiCol_Separator]        = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
	colors[ImGuiCol_SeparatorHovered] = orange;
	colors[ImGuiCol_SeparatorActive]  = orange_hover;

	// Resize grip
	colors[ImGuiCol_ResizeGrip]        = ImVec4(0.88f, 0.40f, 0.10f, 0.20f);
	colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.88f, 0.40f, 0.10f, 0.60f);
	colors[ImGuiCol_ResizeGripActive]  = orange;

	// Docking
	colors[ImGuiCol_DockingPreview] = ImVec4(0.88f, 0.40f, 0.10f, 0.70f);

	// Text
	colors[ImGuiCol_Text]         = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
	colors[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);

	// Borders
	colors[ImGuiCol_Border]       = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
	colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

	// Nav / selection
	colors[ImGuiCol_NavHighlight]   = orange;
	colors[ImGuiCol_TextSelectedBg] = ImVec4(0.88f, 0.40f, 0.10f, 0.30f);

	// Plot
	colors[ImGuiCol_PlotLines]            = orange;
	colors[ImGuiCol_PlotLinesHovered]     = orange_hover;
	colors[ImGuiCol_PlotHistogram]        = orange;
	colors[ImGuiCol_PlotHistogramHovered] = orange_hover;

	// Table
	colors[ImGuiCol_TableHeaderBg]     = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
	colors[ImGuiCol_TableBorderStrong] = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
	colors[ImGuiCol_TableBorderLight]  = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
	colors[ImGuiCol_TableRowBg]        = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
	colors[ImGuiCol_TableRowBgAlt]     = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
}

void ImGuiLayer::renderUI(UIContext& context, EditorState& editor_state,
						   std::function<void(UIContext&)> appUiCallback) {
	beginFrame();

	if (editor_state.editor_mode) {
		renderDockSpace();
		if (appUiCallback)
			appUiCallback(context);

		// In editor mode: transition swapchain for ImGui, then clear-render onto it
		m_renderer.beginEditorUIRender(m_renderer.getCurrentCommandBuffer());
		endFrame(m_renderer.getCurrentCommandBuffer(), true);
		m_renderer.endEditorUIRender(m_renderer.getCurrentCommandBuffer());
	} else {
		// Fullscreen mode: render app callback (performance panel, etc.)
		if (appUiCallback)
			appUiCallback(context);
		endFrame(m_renderer.getCurrentCommandBuffer());
	}
}

void ImGuiLayer::registerViewportImage(VkSampler sampler, VkImageView image_view, VkImageLayout layout) {
	unregisterViewportImage();
	m_viewport_texture_id = ImGui_ImplVulkan_AddTexture(sampler, image_view, layout);
}

void ImGuiLayer::unregisterViewportImage() {
	if (m_viewport_texture_id != VK_NULL_HANDLE) {
		ImGui_ImplVulkan_RemoveTexture(m_viewport_texture_id);
		m_viewport_texture_id = VK_NULL_HANDLE;
	}
}

void ImGuiLayer::recreatePipeline() {
	// Viewport texture will be invalidated by ImGui_ImplVulkan_Shutdown
	m_viewport_texture_id = VK_NULL_HANDLE;

	// Save main viewport's platform handle — ImGui_ImplVulkan_Shutdown() calls
	// DestroyPlatformWindows() which clears PlatformHandle on all viewports,
	// but the GLFW window is still alive (only the Vulkan backend is being recreated).
	ImGuiViewport* main_vp = ImGui::GetMainViewport();
	void* saved_platform_handle = main_vp->PlatformHandle;

	ImGui_ImplVulkan_Shutdown();

	ImGui_ImplVulkan_InitInfo init_info{};
	init_info.Instance = *m_device.getInstance();
	init_info.PhysicalDevice = *m_device.getPhysicalDevice();
	init_info.Device = *m_device.getDevice();
	init_info.QueueFamily = m_device.getGraphicsQueueFamilyIndex();
	init_info.Queue = *m_device.getQueue();
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = m_descriptor_pool;
	init_info.MinImageCount = static_cast<uint32_t>(m_renderer.getImageCount());
	init_info.ImageCount = static_cast<uint32_t>(m_renderer.getImageCount());
	init_info.Allocator = nullptr;
	init_info.CheckVkResultFn = nullptr;
	init_info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
	init_info.PipelineInfoMain.Subpass = 0;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.UseDynamicRendering = true;

	m_color_format = static_cast<VkFormat>(m_renderer.getSwapChainImageFormat());
	memset(&init_info.PipelineInfoMain.PipelineRenderingCreateInfo, 0, sizeof(init_info.PipelineInfoMain.PipelineRenderingCreateInfo));
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_color_format;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

	ImGui_ImplVulkan_Init(&init_info);

	// Restore platform handle so GLFW backend can still access the window
	main_vp->PlatformHandle = saved_platform_handle;
}

void ImGuiLayer::uploadFonts() {}


}
