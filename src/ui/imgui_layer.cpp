#include "pch.hpp"
#include "ui/imgui_layer.hpp"
#include "ve_window.hpp"
#include "ve_device.hpp"
#include "ve_renderer.hpp"
#include "ve_swap_chain.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>


namespace ve {

static VkDescriptorPool createImguiDescriptorPool(vk::raii::Device& device) {
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
    ImGui::StyleColorsDark();

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
    // Dynamic rendering: render on top of the current swapchain image view
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

void ImGuiLayer::renderUI(UIContext& context) {
	beginFrame();
	if (context.visible) {
		ImGui::ShowDemoWindow(nullptr);
		ImGui::SetNextWindowBgAlpha(0.9f);
		// popup window for particle controls
		if (ImGui::Begin("Particles", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			int count = static_cast<int>(context.pending_particle_count);
			ImGui::Text("Particle Controls");
			ImGui::Separator();
			ImGui::SliderInt("Count", &count, 1000, 1000000);
			ImGui::Separator();
			ImGui::SliderFloat("Velocity mean", &context.particle_velocity_mean, -60, 60);
			ImGui::Separator();
			ImGui::SliderFloat("Velocity stddev", &context.particle_velocity_stddev, 0, 60);


			if (count < 1) count = 1;
			context.pending_particle_count = static_cast<uint32_t>(count);
			if (ImGui::Button("Apply")) context.apply_particle_count = true;
			ImGui::SameLine();
			if (ImGui::Button("Reset")) context.reset_particle_count = true;
		}
		ImGui::End();

		// Ui displaying controls such as wasd movement, c to crouch, space to jump
		// 1,2,3,4 for particle behavior
		if (ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			// bottom right
			ImGui::SetWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 200, ImGui::GetIO().DisplaySize.y - 200), ImGuiCond_Always);
			ImGui::Text("Other Controls Here");
			ImGui::Text("WASD: Move");
			ImGui::Text("C: Crouch");
			ImGui::Text("Space: Jump");

			ImGui::Text("1: Particle Mode 1");
			ImGui::Text("2: Particle Mode 2");
			ImGui::Text("3: Particle Mode 3");
			ImGui::Text("4: Particle Mode 4");

		}
		ImGui::End();

		// crude performance window
		static auto time_start = std::chrono::high_resolution_clock::now();
		auto now = std::chrono::high_resolution_clock::now();
		auto time_elapsed = std::chrono::duration<float, std::chrono::seconds::period>(now - time_start).count();
		static int frame_count = 0;
		static float fps = 0.0f;
		static float frame_time_ms = 0.0f;
		frame_count++;
		if (frame_count >= 60) {
			frame_count = 0;
			fps = 1.0f / time_elapsed;
			frame_time_ms = time_elapsed * 1000.0f;
		}

		if (ImGui::Begin("Performance", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			//set location to top right
			ImGui::SetWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 200, 10), ImGuiCond_Always);
			ImGui::Separator();
			ImGui::Text("FPS: %.1f", fps);
			ImGui::Text("Frame Time: %.2f ms", frame_time_ms);
			//resolution
			auto extent = m_renderer.getExtent();
			ImGui::Text("Resolution: %d x %d", extent.width, extent.height);
		}
		ImGui::End();
		time_start = now;
	}
	endFrame(m_renderer.getCurrentCommandBuffer());
}

void ImGuiLayer::uploadFonts() {}


}
