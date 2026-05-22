/* AssetLoadingSystem - orchestrates two-phase async asset loading.
 * Phase 1 (background thread): CPU work (glTF parse, image decode, mesh processing)
 * Phase 2 (main thread): GPU uploads, batched into one transfer-queue CB +
 *   (when textures are uploaded) one graphics-queue CB per tick, joined by a
 *   single timeline semaphore. Polled across frames before advancing to FINALIZING.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/internal/loaded_asset_data.hpp"
#include "resources/ve_model.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/internal/asset_upload.hpp"
#include "resources/internal/staging_arena.hpp"
#include "vulkan/ve_buffer.hpp"

#define VULKAN_HPP_ENABLE_RAII
#include <vulkan/vulkan_raii.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace ve {

enum class LoadState { IDLE, CPU_LOADING, GPU_UPLOADING, FINALIZING, READY, FAILED };

class AssetLoadingSystem {
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

	// Cancel current load and join the background thread. Blocks on any
	// in-flight upload batch before dropping resource handles.
	void cancel();

	LoadState getState() const { return m_state; }
	float getProgress() const;
	std::string getStatusMessage();
	std::string getModelName() const { return m_model_name; }
	const std::string& getCacheKey() const { return m_cache_key; }
	float getLastLoadSeconds() const { return m_last_load_seconds; }

	// Retrieve completed model
	std::shared_ptr<VeModel> takeModel();

private:
	void joinWorker();
	void waitForInFlight();

	VeResourceManager& m_resource_manager;
	LoadState m_state{LoadState::IDLE};
	LoadProgress m_progress;

	std::thread m_worker;
	LoadedAssetData m_asset_data;
	std::string m_model_name;
	std::string m_cache_key;

	// GPU upload tracking
	UploadCursor m_upload_cursor;
	UploadedHandles m_uploaded;

	// One entry per recorded-and-submitted batch that we're still waiting on.
	// FIFO: the front retires first (matches submit order on each queue).
	struct InFlightBatch {
		std::unique_ptr<vk::raii::CommandBuffer> transfer_cmd;
		std::unique_ptr<vk::raii::CommandBuffer> graphics_cmd;
		std::unique_ptr<StagingArena> arena;
		uint64_t target_timeline_value = 0;
	};
	std::deque<InFlightBatch> m_in_flight;

	// Released arenas, ready for the next batch. Implicitly capped at
	// MAX_IN_FLIGHT_BATCHES
	std::vector<std::unique_ptr<StagingArena>> m_arena_pool;

	// Persistent across the whole loader lifetime. Incremented monotonically
	// per submit; each batch consumes 1 (mesh-only) or 2 (textures) values.
	vk::raii::Semaphore m_upload_timeline{nullptr};
	uint64_t m_last_signaled_value = 0;

	std::shared_ptr<VeModel> m_completed_model;
	std::chrono::steady_clock::time_point m_load_start_time;
	float m_last_load_seconds{0.f};

	static constexpr uint32_t MAX_UPLOADS_PER_FRAME = 32;
	// Soft byte cap on staging memory recorded per tick. A single asset larger
	// than this lands in a batch of one
	static constexpr size_t MAX_STAGING_BYTES_PER_FRAME = 64ull * 1024ull * 1024ull;
	// Cap on batches pipelined to the GPU concurrently. Each holds one arena
	static constexpr uint32_t MAX_IN_FLIGHT_BATCHES = 3;
};

}
