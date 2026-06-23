#include "pch.hpp"
#include "ui/panels/asset_browser_panel.hpp"
#include "ui/editor_icons.hpp"
#include "ui/texture_inspector.hpp"
#include "events/event_bus.hpp"
#include "scene/scene_manager.hpp"
#include "scene/camera_view.hpp"
#include "resources/ve_texture.hpp"
#include "platform/ve_file_system.hpp"
#include "ve_config.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_string.hpp"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <portable-file-dialogs.h>
#include <algorithm>
#include <cctype>

namespace ve {

AssetBrowserPanel::~AssetBrowserPanel() {
	for (auto& [key, t] : m_thumbnails)
		if (t.descriptor != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(t.descriptor);
	for (auto& p : m_pending_deletions)
		if (p.descriptor != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(p.descriptor);
}

void AssetBrowserPanel::setAssetRoot(const std::filesystem::path& root) {
	std::error_code ec;
	// Fall back to the working directory if no asset root was configured
	m_root = (!root.empty() && std::filesystem::is_directory(root, ec))
	           ? root : std::filesystem::current_path(ec);
	m_current_dir = m_root;
	m_selected.clear();
	m_dirty = true;
}

void AssetBrowserPanel::invalidateThumbnails() {
	for (auto& [key, t] : m_thumbnails)
		if (t.descriptor != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(t.descriptor);
	m_thumbnails.clear();
	for (auto& p : m_pending_deletions)
		if (p.descriptor != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(p.descriptor);
	m_pending_deletions.clear();
}

AssetBrowserPanel::AssetKind AssetBrowserPanel::classify(const std::filesystem::path& p) {
	std::string ext = toLower(p.extension().string());
	if (ext == ".gltf" || ext == ".glb")
		return AssetKind::Model;
	if (ext == ".ktx" || ext == ".ktx2" || ext == ".png" || ext == ".jpg"
	    || ext == ".jpeg" || ext == ".hdr" || ext == ".tga" || ext == ".bmp")
		return AssetKind::Texture;
	return AssetKind::Other;
}

const char* AssetBrowserPanel::iconFor(AssetKind kind) {
	switch (kind) {
		case AssetKind::Folder:  return ICON_GROUP;
		case AssetKind::Model:   return ICON_MESH;
		case AssetKind::Texture: return ICON_TEXTURE;
		default:                 return ICON_MATERIAL;
	}
}

bool AssetBrowserPanel::passesFilter(const Entry& e, const std::string& filter_lower) const {
	if (e.kind == AssetKind::Other)
		return false;
	if (e.kind == AssetKind::Model && !m_show_models)
		return false;
	if (e.kind == AssetKind::Texture && !m_show_textures)
		return false;
	if (!filter_lower.empty())
		return toLower(e.name).find(filter_lower) != std::string::npos;
	return true;
}

void AssetBrowserPanel::navigateTo(const std::filesystem::path& dir) {
	std::error_code ec;
	if (dir.empty() || !std::filesystem::is_directory(dir, ec))
		return;
	// Stay within the asset root.
	auto rel = std::filesystem::relative(dir, m_root, ec);
	if (!ec && rel.generic_string().rfind("..", 0) == 0)
		return;
	m_current_dir = dir;
	m_selected.clear();
	m_dirty = true;
}

void AssetBrowserPanel::refreshEntries() {
	m_entries.clear();
	clearThumbnails();
	m_dirty = false;

	std::error_code ec;
	if (m_current_dir.empty() || !std::filesystem::is_directory(m_current_dir, ec))
		return;

	if (m_search_active) {
		// Recursive: gather every asset under the current directory
		std::filesystem::recursive_directory_iterator it(m_current_dir, ec), end;
		for (; it != end; it.increment(ec)) {
			const std::filesystem::directory_entry& de = *it;
			std::string fname = de.path().filename().string();
			bool is_dir = de.is_directory(ec);
			if (!fname.empty() && fname.front() == '.') {  // skip (and don't descend into) hidden
				if (is_dir)
					it.disable_recursion_pending();
				continue;
			}
			if (is_dir)
				continue;  // results list assets only, not folders
			AssetKind kind = classify(de.path());
			if (kind == AssetKind::Other)
				continue;
			std::string rel = std::filesystem::relative(de.path(), m_current_dir, ec).generic_string();
			m_entries.push_back(Entry{de.path(), std::move(rel), kind});
		}
	} else {
		for (const auto& de : std::filesystem::directory_iterator(m_current_dir, ec)) {
			std::string name = de.path().filename().string();
			if (!name.empty() && name.front() == '.')  // skip hidden entries
				continue;
			bool is_dir = de.is_directory(ec);
			AssetKind kind = is_dir ? AssetKind::Folder : classify(de.path());
			if (kind == AssetKind::Other)
				continue;
			m_entries.push_back(Entry{de.path(), std::move(name), kind});
		}
	}

	std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
		if ((a.kind == AssetKind::Folder) != (b.kind == AssetKind::Folder))
			return a.kind == AssetKind::Folder;
		return toLower(a.name) < toLower(b.name);
	});
}

VkDescriptorSet AssetBrowserPanel::getThumbnail(const std::filesystem::path& path, bool bypass_limits) {
	if (!m_resource_manager)
		return VK_NULL_HANDLE;

	const std::string key = path.generic_string();
	auto it = m_thumbnails.find(key);
	if (it != m_thumbnails.end())
		return it->second.descriptor;
	if (m_failed_thumbnails.count(key))
		return VK_NULL_HANDLE;

	constexpr int MAX_THUMB_LOADS_PER_FRAME = 2;
	constexpr size_t MAX_THUMBNAILS = 512;
	if (!bypass_limits
	    && (m_thumb_loads_this_frame >= MAX_THUMB_LOADS_PER_FRAME || m_thumbnails.size() >= MAX_THUMBNAILS))
		return VK_NULL_HANDLE;
	m_thumb_loads_this_frame++;

	Thumbnail t;
	t.texture = VeTexture::loadFromPath(*m_resource_manager, path, TextureType::ALBEDO);
	if (!t.texture || !t.texture->getImage()) {
		m_failed_thumbnails.insert(key);
		return VK_NULL_HANDLE;
	}
	t.descriptor = ImGui_ImplVulkan_AddTexture(
		static_cast<VkSampler>(*t.texture->getSampler()),
		static_cast<VkImageView>(*t.texture->getImageView()),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	VkDescriptorSet ds = t.descriptor;
	m_thumbnails.emplace(key, std::move(t));
	return ds;
}

void AssetBrowserPanel::clearThumbnails() {
	for (auto& [key, t] : m_thumbnails)
		if (t.descriptor != VK_NULL_HANDLE)
			m_pending_deletions.push_back({t.descriptor, m_frame});
	m_thumbnails.clear();
	m_failed_thumbnails.clear();
}

void AssetBrowserPanel::flushThumbnailDeletions() {
	for (auto it = m_pending_deletions.begin(); it != m_pending_deletions.end();) {
		if (m_frame - it->frame_queued > MAX_FRAMES_IN_FLIGHT) {
			if (it->descriptor != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(it->descriptor);
			it = m_pending_deletions.erase(it);
		} else {
			++it;
		}
	}
}

void AssetBrowserPanel::pollMetadata() {
	if (m_metadata_future.valid()
	    && m_metadata_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		constexpr size_t MAX_METADATA_CACHE = 256;
		if (m_metadata_cache.size() >= MAX_METADATA_CACHE)
			m_metadata_cache.clear();
		m_metadata_cache[m_metadata_path.generic_string()] = m_metadata_future.get();
		m_metadata_future = {};
	}
}

void AssetBrowserPanel::render(Registry* /*registry*/, EditorState& state, UIContext& /*context*/) {
	m_frame++;
	m_thumb_loads_this_frame = 0;
	flushThumbnailDeletions();
	pollMetadata();

	if (!ImGui::Begin("Asset Browser", &state.show_asset_browser)) {
		ImGui::End();
		return;
	}

	// Entering/leaving search flips between the flat listing and recursive results.
	bool want_search = m_filter_buf[0] != '\0';
	if (want_search != m_search_active) {
		m_search_active = want_search;
		m_dirty = true;
	}
	if (m_dirty)
		refreshEntries();

	renderToolbar(state);
	ImGui::Separator();
	if (m_view_mode == ViewMode::Grid)
		renderGrid(state);
	else
		renderList(state);
	ImGui::SameLine();
	renderMetadata(state);

	ImGui::End();
}

void AssetBrowserPanel::renderToolbar(EditorState& state) {
	bool at_root = m_current_dir.empty() || m_current_dir == m_root;
	ImGui::BeginDisabled(at_root);
	if (ImGui::Button("< Up"))
		navigateTo(m_current_dir.parent_path());
	ImGui::EndDisabled();

	ImGui::SameLine();
	// Import a model from anywhere on disk via a native dialog.
	if (ImGui::Button("Import...") && m_event_bus) {
		auto selection = pfd::open_file(
			"Import glTF Model", "",
			{"glTF Files", "*.gltf *.glb"},
			pfd::opt::none).result();
		if (!selection.empty()) {
			m_event_bus->emitImmediate(AddModelRequestedEvent{
				.gltf_path = selection[0],
				.flip_tex_coord_v = state.import_flip_v});
			VE_LOGI("Asset browser: import model '" << selection[0] << "'");
		}
	}

	ImGui::SameLine();
	std::error_code ec;
	auto rel = std::filesystem::relative(m_current_dir, m_root, ec);
	std::string crumb = (ec || rel.empty() || rel == ".") ? m_root.filename().string()
	                                                       : rel.generic_string();
	ImGui::TextDisabled("%s", crumb.c_str());

	ImGui::SetNextItemWidth(FILTER_INPUT_WIDTH);
	ImGui::InputTextWithHint("##filter", "Filter", m_filter_buf, sizeof(m_filter_buf));
	ImGui::SameLine();
	ImGui::Checkbox("Models", &m_show_models);
	ImGui::SameLine();
	ImGui::Checkbox("Textures", &m_show_textures);
	ImGui::SameLine();
	ImGui::Checkbox("Flip V", &state.import_flip_v);
	ImGui::SameLine();

	auto viewButton = [this](const char* label, ViewMode mode) {
		bool active = m_view_mode == mode;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
		if (ImGui::SmallButton(label))
			m_view_mode = mode;
		if (active)
			ImGui::PopStyleColor();
	};
	viewButton("List", ViewMode::List);
	ImGui::SameLine();
	viewButton("Grid", ViewMode::Grid);

	if (m_view_mode == ViewMode::Grid) {
		ImGui::SameLine();
		ImGui::SetNextItemWidth(SIZE_SLIDER_WIDTH);
		ImGui::SliderFloat("Size", &m_thumb_size, MIN_THUMB_SIZE, MAX_THUMB_SIZE, "%.0f");
	}
}

void AssetBrowserPanel::handleEntryClick(const Entry& e, bool clicked, bool double_clicked, EditorState& state) {
	if (clicked)
		m_selected = e.path;
	if (!double_clicked)
		return;
	if (e.kind == AssetKind::Folder)
		navigateTo(e.path);
	else if (e.kind == AssetKind::Model)
		emitAddModel(e.path, glm::vec3(0.0f), state);
}

void AssetBrowserPanel::beginModelDragSource(const Entry& e) {
	if (e.kind == AssetKind::Model && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
		std::string p = e.path.generic_string();
		ImGui::SetDragDropPayload("ASSET_GLTF_PATH", p.c_str(), p.size() + 1);
		ImGui::TextUnformatted(e.name.c_str());
		ImGui::EndDragDropSource();
	}
}

void AssetBrowserPanel::emitAddModel(const std::filesystem::path& path, const glm::vec3& translation, EditorState& state) {
	if (!m_event_bus)
		return;
	m_event_bus->emitImmediate(AddModelRequestedEvent{
		.gltf_path = path, .translation = translation, .flip_tex_coord_v = state.import_flip_v});
	VE_LOGI("Asset browser: add model '" << path.filename().string() << "'");
}

glm::vec3 AssetBrowserPanel::placeInFrontOfCamera() const {
	if (!m_camera_view)
		return glm::vec3(0.0f);
	return m_camera_view->position + m_camera_view->forward * PLACE_DISTANCE;
}

void AssetBrowserPanel::inspectTexture(const std::filesystem::path& path) {
	if (!m_texture_inspector)
		return;
	getThumbnail(path, /*bypass_limits=*/true);  // ensure the texture is loaded
	auto it = m_thumbnails.find(path.generic_string());
	if (it == m_thumbnails.end() || !it->second.texture)
		return;
	m_inspected_texture = it->second.texture;  // keep alive while the inspector references it
	m_texture_inspector->open(m_inspected_texture.get(), path.filename().string().c_str());
}

void AssetBrowserPanel::renderContextMenu(const Entry& e, EditorState& state) {
	if (!ImGui::BeginPopupContextItem())
		return;
	m_selected = e.path;

	switch (e.kind) {
		case AssetKind::Folder:
			if (ImGui::MenuItem("Open"))
				navigateTo(e.path);
			break;
		case AssetKind::Model:
			if (ImGui::MenuItem("Add to Scene"))
				emitAddModel(e.path, glm::vec3(0.0f), state);
			if (ImGui::MenuItem("Add in front of camera", nullptr, false, m_camera_view != nullptr))
				emitAddModel(e.path, placeInFrontOfCamera(), state);
			break;
		case AssetKind::Texture:
			if (ImGui::MenuItem("Inspect", nullptr, false, m_texture_inspector != nullptr))
				inspectTexture(e.path);
			break;
		default:
			break;
	}

	ImGui::Separator();
	if (e.kind != AssetKind::Folder && ImGui::MenuItem("Open containing folder"))
		navigateTo(e.path.parent_path());
	if (ImGui::MenuItem("Reveal in file manager"))
		VeFileSystem::revealInFileManager(e.path);
	if (ImGui::MenuItem("Copy path"))
		ImGui::SetClipboardText(e.path.string().c_str());

	ImGui::EndPopup();
}

void AssetBrowserPanel::renderBackgroundMenu() {
	if (!ImGui::BeginPopupContextWindow("##bg", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		return;
	if (ImGui::MenuItem("Refresh"))
		m_dirty = true;
	if (ImGui::MenuItem("Reveal current folder"))
		VeFileSystem::revealInFileManager(m_current_dir);
	ImGui::EndPopup();
}

void AssetBrowserPanel::renderList(EditorState& state) {
	ImGui::BeginChild("##list", ImVec2(-METADATA_PANEL_WIDTH, 0), ImGuiChildFlags_Borders);

	const std::string filter_lower = (m_filter_buf[0] != '\0') ? toLower(m_filter_buf) : std::string{};

	int idx = 0;
	for (const Entry& e : m_entries) {
		if (!passesFilter(e, filter_lower))
			continue;

		ImGui::PushID(idx++);
		ImGui::TextUnformatted(iconFor(e.kind));
		ImGui::SameLine();

		bool selected = (e.path == m_selected);
		bool clicked = ImGui::Selectable(e.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick);

		beginModelDragSource(e);

		handleEntryClick(e, clicked, clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left), state);
		renderContextMenu(e, state);
		ImGui::PopID();
	}

	renderBackgroundMenu();
	ImGui::EndChild();
}

void AssetBrowserPanel::renderGrid(EditorState& state) {
	ImGui::BeginChild("##grid", ImVec2(-METADATA_PANEL_WIDTH, 0), ImGuiChildFlags_Borders);

	ImGuiStyle& style = ImGui::GetStyle();
	float avail = ImGui::GetContentRegionAvail().x;
	float cell = m_thumb_size + style.ItemSpacing.x;
	int columns = std::max(1, static_cast<int>(avail / cell));
	int max_chars = std::max(MIN_LABEL_CHARS, static_cast<int>(m_thumb_size / LABEL_CHAR_WIDTH_PX));

	const std::string filter_lower = (m_filter_buf[0] != '\0') ? toLower(m_filter_buf) : std::string{};

	int idx = 0;
	int col = 0;
	const ImVec2 btn_size(m_thumb_size, m_thumb_size);
	const ImVec2 image_size(
		std::max(1.0f, m_thumb_size - 2.0f * style.FramePadding.x),
		std::max(1.0f, m_thumb_size - 2.0f * style.FramePadding.y));

	for (const Entry& e : m_entries) {
		if (!passesFilter(e, filter_lower))
			continue;

		ImGui::PushID(idx++);
		ImGui::BeginGroup();

		bool selected = (e.path == m_selected);
		if (selected)
			ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);

		VkDescriptorSet thumb = (e.kind == AssetKind::Texture && ImGui::IsRectVisible(btn_size))
			? getThumbnail(e.path) : VK_NULL_HANDLE;
		bool clicked;
		if (thumb != VK_NULL_HANDLE)
			clicked = ImGui::ImageButton("##thumb",
				static_cast<ImTextureID>(reinterpret_cast<intptr_t>(thumb)), image_size);
		else
			clicked = ImGui::Button(iconFor(e.kind), btn_size);

		if (selected)
			ImGui::PopStyleColor();

		bool hovered = ImGui::IsItemHovered();

		beginModelDragSource(e);

		handleEntryClick(e, clicked, hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left), state);
		renderContextMenu(e, state);

		if (hovered)
			ImGui::SetTooltip("%s", e.name.c_str());

		// Grid tiles are narrow: label with the filename
		std::string display = e.path.filename().string();
		std::string label = (display.size() > static_cast<size_t>(max_chars))
			? display.substr(0, static_cast<size_t>(max_chars) - 1) + "..."
			: display;
		ImGui::TextUnformatted(label.c_str());

		ImGui::EndGroup();
		ImGui::PopID();

		if (++col % columns != 0)
			ImGui::SameLine();
	}

	renderBackgroundMenu();
	ImGui::EndChild();
}

void AssetBrowserPanel::renderMetadata(EditorState& state) {
	ImGui::BeginChild("##meta", ImVec2(0, 0), ImGuiChildFlags_Borders);

	if (m_selected.empty()) {
		ImGui::TextDisabled("Select an asset");
		ImGui::EndChild();
		return;
	}

	ImGui::TextWrapped("%s", m_selected.filename().string().c_str());
	ImGui::Separator();

	std::error_code ec;
	auto bytes = std::filesystem::file_size(m_selected, ec);
	if (!ec)
		ImGui::Text("Size: %.1f KB", static_cast<double>(bytes) / 1024.0);

	AssetKind kind = classify(m_selected);
	if (kind == AssetKind::Model) {
		const std::string key = m_selected.generic_string();
		auto it = m_metadata_cache.find(key);
		if (it == m_metadata_cache.end()) {
			if (!m_metadata_future.valid()) {
				m_metadata_path = m_selected;
				std::filesystem::path p = m_selected;
				m_metadata_future = std::async(std::launch::async,
					[p]() { return gltf::probeMetadata(p); });
			}
			ImGui::TextDisabled("Reading metadata...");
		} else if (!it->second.ok) {
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Parse failed");
			if (!it->second.error.empty())
				ImGui::TextWrapped("%s", it->second.error.c_str());
		} else {
			const gltf::GltfMetadata& m = it->second;
			ImGui::Text("Meshes:     %u", m.mesh_count);
			ImGui::Text("Materials:  %u", m.material_count);
			ImGui::Text("Nodes:      %u", m.node_count);
			ImGui::Text("Textures:   %u", m.texture_count);
			ImGui::Text("Triangles:  %llu", static_cast<unsigned long long>(m.triangle_count));
			ImGui::Spacing();
			ImGui::Text("Animations: %u", m.animation_count);
			ImGui::Text("Skins:      %u", m.skin_count);
			ImGui::Text("Lights:     %u", m.light_count);
			ImGui::Text("Cameras:    %u", m.camera_count);
		}
		ImGui::Separator();
		if (ImGui::Button("Add to Scene") && m_event_bus)
			m_event_bus->emitImmediate(AddModelRequestedEvent{
				.gltf_path = m_selected, .flip_tex_coord_v = state.import_flip_v});
		ImGui::SameLine();
		ImGui::TextDisabled("(or drag to viewport)");
	} else if (kind == AssetKind::Texture) {
		VkDescriptorSet preview = getThumbnail(m_selected, /*bypass_limits=*/true);
		auto it = m_thumbnails.find(m_selected.generic_string());
		if (it != m_thumbnails.end() && it->second.texture) {
			const VeTexture* tex = it->second.texture.get();
			ImGui::Text("Dimensions: %u x %u", tex->getWidth(), tex->getHeight());
			ImGui::Text("Mip levels: %u", tex->getMipLevels());
			if (preview != VK_NULL_HANDLE && tex->getWidth() > 0) {
				float w = ImGui::GetContentRegionAvail().x;
				float aspect = static_cast<float>(tex->getHeight()) / static_cast<float>(tex->getWidth());
				ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(preview)),
					ImVec2(w, w * aspect));
			}
		} else {
			ImGui::TextDisabled("Loading preview...");
		}
	}

	ImGui::EndChild();
}

} // namespace ve
