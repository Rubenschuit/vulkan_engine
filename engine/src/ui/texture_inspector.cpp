#include "pch.hpp"
#include "ui/texture_inspector.hpp"
#include "vulkan/ve_image.hpp"
#include "resources/ve_texture.hpp"
#include "ve_config.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

namespace ve {

TextureInspector::~TextureInspector() {
	for (auto& entry : m_layer_cache)
		if (entry.descriptor_set != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(entry.descriptor_set);
	for (auto& pending : m_pending_deletions)
		for (auto& entry : pending.entries)
			if (entry.descriptor_set != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(entry.descriptor_set);
}

void TextureInspector::open(const VeImage* image, const vk::raii::Sampler& sampler, const char* name) {
	if (m_image == image && m_open)
		return;

	clearCache();
	m_image = image;
	m_sampler = &sampler;
	m_name = name;
	m_selected_layer = 0;
	m_open = true;

	if (m_image)
		rebuildCache();
}

void TextureInspector::open(const VeTexture* texture, const char* name) {
	if (!texture || !texture->getImage())
		return;
	open(texture->getImage(), texture->getSampler(), name);
}

void TextureInspector::close() {
	clearCache();
	m_image = nullptr;
	m_sampler = nullptr;
	m_open = false;
}

void TextureInspector::invalidateCache() {
	for (auto& entry : m_layer_cache)
		if (entry.descriptor_set != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(entry.descriptor_set);
	m_layer_cache.clear();
	for (auto& pending : m_pending_deletions)
		for (auto& entry : pending.entries)
			if (entry.descriptor_set != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(entry.descriptor_set);
	m_pending_deletions.clear();
	if (m_image && m_sampler)
		rebuildCache();
}

void TextureInspector::rebuildCache() {
	uint32_t layers = m_image->getArrayLayers();
	m_layer_cache.resize(layers);

	bool is_depth = static_cast<bool>(m_image->getAspectFlags() & vk::ImageAspectFlagBits::eDepth);
	VkImageLayout layout = is_depth
		? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
		: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	// Depth textures may sample as (D,0,0,1) on some drivers. Swizzle R into all
	// channels so they display as grayscale.
	vk::ComponentMapping depth_swizzle{
		vk::ComponentSwizzle::eR,
		vk::ComponentSwizzle::eR,
		vk::ComponentSwizzle::eR,
		vk::ComponentSwizzle::eOne};

	bool needs_custom_view = is_depth || layers > 1;
	for (uint32_t i = 0; i < layers; i++) {
		if (needs_custom_view)
			m_layer_cache[i].image_view = m_image->createLayerImageView(
				i, is_depth ? depth_swizzle : vk::ComponentMapping{});

		VkImageView view = needs_custom_view
			? static_cast<VkImageView>(*m_layer_cache[i].image_view)
			: static_cast<VkImageView>(*m_image->getImageView());

		m_layer_cache[i].descriptor_set = ImGui_ImplVulkan_AddTexture(
			static_cast<VkSampler>(**m_sampler),
			view,
			layout);
	}
}

void TextureInspector::clearCache() {
	if (!m_layer_cache.empty()) {
		m_pending_deletions.push_back({std::move(m_layer_cache), m_frame_counter});
		m_layer_cache.clear();
	}
}

void TextureInspector::flushPendingDeletions() {
	for (auto it = m_pending_deletions.begin(); it != m_pending_deletions.end(); ) {
		if (m_frame_counter - it->frame_queued > MAX_FRAMES_IN_FLIGHT) {
			for (auto& entry : it->entries)
				if (entry.descriptor_set != VK_NULL_HANDLE)
					ImGui_ImplVulkan_RemoveTexture(entry.descriptor_set);
			it = m_pending_deletions.erase(it);
		} else {
			++it;
		}
	}
}

void TextureInspector::render() {
	m_frame_counter++;
	flushPendingDeletions();

	if (!m_open || !m_image || m_layer_cache.empty())
		return;

	ImGui::SetNextWindowSize(ImVec2(400, 450), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(m_name.c_str(), &m_open)) {
		ImGui::End();
		return;
	}

	uint32_t layers = static_cast<uint32_t>(m_layer_cache.size());
	uint32_t w = m_image->getWidth();
	uint32_t h = m_image->getHeight();

	ImGui::Text("%ux%u  %u %s  %u %s",
		w, h,
		layers, layers == 1 ? "layer" : "layers",
		m_image->getMipLevels(), m_image->getMipLevels() == 1 ? "mip" : "mips");

	if (layers > 1)
		ImGui::SliderInt("Layer", &m_selected_layer, 0, static_cast<int>(layers) - 1);

	ImGui::SliderFloat("Exposure", &m_exposure, 0.01f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic);

	const char* channel_names[] = {"RGB", "R", "G", "B", "A"};
	for (int i = 0; i < 5; i++) {
		if (i > 0)
			ImGui::SameLine();
		if (ImGui::RadioButton(channel_names[i], m_channel_mode == i))
			m_channel_mode = i;
	}

	ImVec4 tint;
	switch (m_channel_mode) {
		case 1:  tint = ImVec4(m_exposure, 0.0f, 0.0f, 1.0f); break;
		case 2:  tint = ImVec4(0.0f, m_exposure, 0.0f, 1.0f); break;
		case 3:  tint = ImVec4(0.0f, 0.0f, m_exposure, 1.0f); break;
		case 4:  tint = ImVec4(0.0f, 0.0f, 0.0f, m_exposure); break;
		default: tint = ImVec4(m_exposure, m_exposure, m_exposure, 1.0f); break;
	}

	uint32_t layer = static_cast<uint32_t>(std::clamp(m_selected_layer, 0, static_cast<int>(layers) - 1));
	VkDescriptorSet ds = m_layer_cache[layer].descriptor_set;
	if (ds != VK_NULL_HANDLE) {
		ImVec2 avail = ImGui::GetContentRegionAvail();
		float aspect = static_cast<float>(w) / static_cast<float>(h);
		float display_w = avail.x;
		float display_h = display_w / aspect;
		if (display_h > avail.y) {
			display_h = avail.y;
			display_w = display_h * aspect;
		}

		ImGui::ImageWithBg(
			static_cast<ImTextureID>(reinterpret_cast<intptr_t>(ds)),
			ImVec2(display_w, display_h),
			ImVec2(0, 0), ImVec2(1, 1),
			ImVec4(0, 0, 0, 1),
			tint);
	}

	ImGui::End();

	if (!m_open)
		close();
}

} // namespace ve
