// Dockable browser over the project's asset folders.
// Has a list/grid view, search/filter, and metadata panel.
// Allows drag-and-drop of models into the scene.

#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/gltf_metadata.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>
#include <glm/vec3.hpp>

namespace ve {

class EventBus;
class VeTexture;
class TextureInspector;
struct CameraView;


class VENGINE_API AssetBrowserPanel : public EditorPanel {
public:
	AssetBrowserPanel() = default;
	~AssetBrowserPanel();

	AssetBrowserPanel(const AssetBrowserPanel&) = delete;
	AssetBrowserPanel& operator=(const AssetBrowserPanel&) = delete;

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Asset Browser"; }

	void setEventBus(EventBus* bus) { m_event_bus = bus; }
	void setResourceManager(VeResourceManager* rm) { m_resource_manager = rm; }
	void setAssetRoot(const std::filesystem::path& root);
	void setCameraView(const CameraView* view) { m_camera_view = view; }
	void setTextureInspector(TextureInspector* inspector) { m_texture_inspector = inspector; }

	// Drop cached ImGui texture descriptors when the Vulkan backend is recreated.
	void invalidateThumbnails();

private:
	enum class AssetKind { Folder, Model, Texture, Other };
	enum class ViewMode { List, Grid };

	struct Entry {
		std::filesystem::path path;
		std::string name;
		AssetKind kind = AssetKind::Other;
	};

	struct Thumbnail {
		ResourceHandle<VeTexture> texture;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
	};

	void refreshEntries();
	void renderToolbar(EditorState& state);
	void renderList(EditorState& state);
	void renderGrid(EditorState& state);
	void renderMetadata(EditorState& state);
	void navigateTo(const std::filesystem::path& dir);

	void handleEntryClick(const Entry& e, bool clicked, bool double_clicked, EditorState& state);
	void beginModelDragSource(const Entry& e);
	void renderContextMenu(const Entry& e, EditorState& state);
	void renderBackgroundMenu();

	void emitAddModel(const std::filesystem::path& path, const glm::vec3& translation, EditorState& state);
	glm::vec3 placeInFrontOfCamera() const;
	void inspectTexture(const std::filesystem::path& path);

	VkDescriptorSet getThumbnail(const std::filesystem::path& path, bool bypass_limits = false);
	void clearThumbnails();
	void flushThumbnailDeletions();

	void pollMetadata();

	static AssetKind classify(const std::filesystem::path& p);
	static const char* iconFor(AssetKind kind);
	bool passesFilter(const Entry& e, const std::string& filter_lower) const;

	EventBus* m_event_bus = nullptr;
	VeResourceManager* m_resource_manager = nullptr;
	const CameraView* m_camera_view = nullptr;
	TextureInspector* m_texture_inspector = nullptr;

	std::filesystem::path m_root;
	std::filesystem::path m_current_dir;
	std::vector<Entry> m_entries;
	bool m_dirty = true;

	std::filesystem::path m_selected;

	char m_filter_buf[128]{};
	bool m_show_models = true;
	bool m_show_textures = false;
	bool m_search_active = false;
	ViewMode m_view_mode = ViewMode::List;

	static constexpr float MIN_THUMB_SIZE = 32.0f;
	static constexpr float MAX_THUMB_SIZE = 256.0f;
	static constexpr float METADATA_PANEL_WIDTH = 260.0f;
	static constexpr float FILTER_INPUT_WIDTH = 160.0f;
	static constexpr float SIZE_SLIDER_WIDTH = 110.0f;
	static constexpr float LABEL_CHAR_WIDTH_PX = 7.0f;
	static constexpr int MIN_LABEL_CHARS = 6;
	static constexpr float PLACE_DISTANCE = 8.0f;  // "add in front of camera" distance
	float m_thumb_size = 48.0f;

	std::unordered_map<std::string, Thumbnail> m_thumbnails;
	std::unordered_set<std::string> m_failed_thumbnails;
	ResourceHandle<VeTexture> m_inspected_texture;
	struct PendingDeletion {
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
		uint64_t frame_queued = 0;
	};
	std::deque<PendingDeletion> m_pending_deletions;
	uint64_t m_frame = 0;
	int m_thumb_loads_this_frame = 0;

	// One async header-probe in flight
	std::future<gltf::GltfMetadata> m_metadata_future;
	std::filesystem::path m_metadata_path;
	std::unordered_map<std::string, gltf::GltfMetadata> m_metadata_cache;
};

} // namespace ve