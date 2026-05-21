/* AssetLoadingSystem - orchestrates two-phase async asset loading.
 * Phase 1 (background thread): CPU work (glTF parse, image decode, mesh processing)
 * Phase 2 (main thread): GPU uploads (VeBuffer, VeImage, descriptor sets)
 */
#pragma once
#include "ve_export.hpp"
#include "resources/loaded_asset_data.hpp"
#include "resources/ve_model.hpp"
#include "resources/ve_resource_manager.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

namespace ve {

enum class LoadState { IDLE, CPU_LOADING, GPU_UPLOADING, FINALIZING, READY, FAILED };

class VENGINE_API AssetLoadingSystem {
public:
	explicit AssetLoadingSystem(VeResourceManager& resource_manager);
	~AssetLoadingSystem();

	AssetLoadingSystem(const AssetLoadingSystem&) = delete;
	AssetLoadingSystem& operator=(const AssetLoadingSystem&) = delete;

	// Start async loading of a glTF model
	void beginModelLoad(const std::filesystem::path& gltf_path,
	                    bool extract_lights, bool flip_tex_coord_v);

	// Call once per frame from main thread. Performs batched GPU uploads.
	void tick();

	// Cancel current load and join the background thread
	void cancel();

	LoadState getState() const { return m_state; }
	float getProgress() const;
	std::string getStatusMessage();
	std::string getModelName() const { return m_model_name; }
	float getLastLoadSeconds() const { return m_last_load_seconds; }

	// Retrieve completed model (transitions state to IDLE)
	std::unique_ptr<VeModel> takeModel();

private:
	void joinWorker();

	VeResourceManager& m_resource_manager;
	LoadState m_state{LoadState::IDLE};
	LoadProgress m_progress;

	std::thread m_worker;
	LoadedAssetData m_asset_data;
	std::string m_model_name;

	// GPU upload tracking
	uint32_t m_mesh_upload_cursor{0};
	uint32_t m_mat_upload_cursor{0};
	std::vector<ResourceHandle<VeMesh>> m_uploaded_meshes;
	std::vector<ResourceHandle<VeMaterial>> m_uploaded_materials;

	std::unique_ptr<VeModel> m_completed_model;
	std::chrono::steady_clock::time_point m_load_start_time;
	float m_last_load_seconds{0.f};

	static constexpr uint32_t MAX_UPLOADS_PER_FRAME = 8;
};

} // namespace ve
