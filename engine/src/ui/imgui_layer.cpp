#include "game/ve_frame_info.hpp"
#include "pch.hpp"
#include "ui/imgui_layer.hpp"
#include "core/ve_window.hpp"
#include "core/ve_device.hpp"
#include "core/ve_renderer.hpp"

#include <cmath>
#include <chrono>
#include <imgui.h>
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
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * static_cast<uint32_t>(pool_sizes.size());
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
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
	// Set style
	//ImGui::StyleColorsLight();
    //ImGui::StyleColorsDark();
	ImGui::StyleColorsClassic();


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

void ImGuiLayer::endFrame(vk::raii::CommandBuffer& cmd) {
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    // Render on top of the current swapchain image view
    VkImageView color_view = static_cast<VkImageView>(*m_renderer.getSwapChainImageView(m_renderer.getCurrentImageIndex()));
    const vk::RenderingAttachmentInfo color_attachment{
        .imageView = color_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore
    };
    const auto extent = m_renderer.getExtent();

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

void ImGuiLayer::renderEngineWindows(UIContext& context) {

	if (!context.show_performance) return;

	static auto s_last_time = std::chrono::high_resolution_clock::now();
	auto now = std::chrono::high_resolution_clock::now();
	float dt = std::chrono::duration<float, std::chrono::seconds::period>(now - s_last_time).count();
	s_last_time = now;

	static int frame_count = 0;
	static float fps = 0.0f;
	static float frame_time_ms = 0.0f;
	static float cpu_time_ms = 0.0f;
	static float gpu_time_ms = 0.0f;
	static float cpu_time_sum = 0.0f;
	static float gpu_time_sum = 0.0f;
	static float accumulated_dt = 0.0f;
	static float fps_history[120] = {};
	static int history_offset = 0;
	static float graph_update_timer = 0.0f;
	static int graph_frames = 0;

	// Text display updates (every 60 frames)
	cpu_time_sum += context.cpu_time;
	gpu_time_sum += context.gpu_time;
	accumulated_dt += dt;
	frame_count++;

	if (frame_count >= 60) {
		fps = (accumulated_dt > 0.0f) ? (60.0f / accumulated_dt) : 0.0f;
		frame_time_ms = (accumulated_dt / 60.0f) * 1000.0f;
		cpu_time_ms = cpu_time_sum / 60.0f;
		gpu_time_ms = gpu_time_sum / 60.0f;
		frame_count = 0;
		cpu_time_sum = 0.0f;
		gpu_time_sum = 0.0f;
		accumulated_dt = 0.0f;
	}

	// Graph updates (every 0.1 seconds)
	graph_update_timer += dt;
	graph_frames++;
	if (graph_update_timer >= 0.1f) {
		fps_history[history_offset] = (float)graph_frames / graph_update_timer;
		history_offset = (history_offset + 1) % 120;
		graph_update_timer = 0.0f;
		graph_frames = 0;
	}

	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;
	if (context.visible) {
		flags = ImGuiWindowFlags_AlwaysAutoResize;
	}

	if (ImGui::Begin("Performance", &context.show_performance, flags)) {
		ImGui::Text("FPS: %.1f", fps);
		ImGui::Text("Frame Time: %.2f ms", frame_time_ms);
		ImGui::Text("CPU Time:   %.2f ms", cpu_time_ms);
		ImGui::Text("GPU Time:   %.2f ms", gpu_time_ms);

		float max_fps = 0.0f;
		for (float f : fps_history) if (f > max_fps) max_fps = f;
		if (max_fps < 60.0f) max_fps = 60.0f;

		// Axis labels and graph
		ImGui::BeginGroup();
		ImGui::Text("%.0f", max_fps);
		ImGui::Dummy(ImVec2(0.0f, 42.0f)); // Spacer
		ImGui::Text("0");
		ImGui::EndGroup();
		ImGui::SameLine();

		ImGui::PlotLines("##FPS", fps_history, 120, history_offset, "FPS", 0.0f, max_fps * 1.1f, ImVec2(250, 80));

		ImGui::Separator();
		//resolution
		auto extent = m_renderer.getExtent();
		ImGui::Text("Resolution: %d x %d", extent.width, extent.height);
	}
	ImGui::End();
}

void ImGuiLayer::renderUI(UIContext& context, std::function<void(UIContext&)> appUiCallback) {
	beginFrame();
	renderEngineWindows(context);
	if (context.visible) {
		if (appUiCallback) {
			appUiCallback(context);
		}
	}
	endFrame(m_renderer.getCurrentCommandBuffer());
}

void ImGuiLayer::recreatePipeline() {
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
}

void ImGuiLayer::uploadFonts() {}


}
