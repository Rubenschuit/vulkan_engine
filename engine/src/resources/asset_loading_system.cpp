#include "pch.hpp"
#include "resources/asset_loading_system.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_texture.hpp"
#include "resources/internal/asset_upload.hpp"
#include "resources/internal/gltf_loader.hpp"
#include "resources/internal/upload_context.hpp"
#include "resources/internal/staging_arena.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "utils/ve_log.hpp"
#include "utils/ve_path.hpp"

namespace ve {

AssetLoadingSystem::AssetLoadingSystem(VeResourceManager& resource_manager)
	: m_resource_manager(resource_manager) {
	vk::SemaphoreTypeCreateInfo type_info{
		.sType = vk::StructureType::eSemaphoreTypeCreateInfo,
		.semaphoreType = vk::SemaphoreType::eTimeline,
		.initialValue = 0,
	};
	vk::SemaphoreCreateInfo create_info{ .pNext = &type_info };
	m_transfer_timeline = vk::raii::Semaphore(m_resource_manager.getDevice().getDevice(), create_info);
	m_graphics_timeline = vk::raii::Semaphore(m_resource_manager.getDevice().getDevice(), create_info);
}

AssetLoadingSystem::~AssetLoadingSystem() {
	cancel();
}

void AssetLoadingSystem::beginModelLoad(const std::filesystem::path& gltf_path,
                                        bool extract_lights, bool flip_tex_coord_v) {
	cancel();

	m_progress.reset();
	m_state = LoadState::CPU_LOADING;
	m_model_name = pathToUtf8(gltf_path.filename());
	m_cache_key = pathToUtf8Generic(gltf_path.lexically_normal());
	m_upload_cursor = {};
	m_uploaded = {};
	m_completed_model.reset();
	m_load_start_time = std::chrono::steady_clock::now();

	m_progress.setStatus("Parsing " + m_model_name + "...");

	GpuCaps caps{
		.supports_bc = m_resource_manager.getDevice().supportsBC(),
		.supports_astc = m_resource_manager.getDevice().supportsASTC(),
	};

	m_worker = std::thread([this, gltf_path, extract_lights, flip_tex_coord_v, caps]() {
		try {
			m_asset_data = ve::gltf::load(gltf_path, extract_lights, flip_tex_coord_v, m_progress, caps);
		} catch (const std::exception& e) {
			VE_LOGE("Async load exception: " << e.what());
			m_progress.cpu_failed = true;
		} catch (...) {
			VE_LOGE("Async load: unknown exception");
			m_progress.cpu_failed = true;
		}
	});
}

float AssetLoadingSystem::getProgress() const {
	if (m_state == LoadState::IDLE || m_state == LoadState::FAILED)
		return 0.f;
	if (m_state == LoadState::READY)
		return 1.f;

	if (m_state == LoadState::CPU_LOADING) {
		float cpu = m_progress.progress();
		if (cpu <= 0.f)
			return -1.f;  // indeterminate (parsing phase)
		return cpu * 0.5f;
	}

	// GPU phase gets 0.5..1.0
	uint32_t total_gpu = static_cast<uint32_t>(m_asset_data.textures.size()
	                   + m_asset_data.meshes.size()
	                   + m_asset_data.materials.size());
	if (total_gpu == 0)
		return 0.95f;
	uint32_t done_gpu = m_upload_cursor.tex + m_upload_cursor.mesh + m_upload_cursor.mat;
	float gpu_frac = static_cast<float>(done_gpu) / static_cast<float>(total_gpu);
	return 0.5f + gpu_frac * 0.5f;
}

std::string AssetLoadingSystem::getStatusMessage() {
	if (m_state == LoadState::CPU_LOADING)
		return m_progress.getStatus();
	if (m_state == LoadState::GPU_UPLOADING) {
		if (m_upload_cursor.tex < m_asset_data.textures.size())
			return "Uploading texture " + std::to_string(m_upload_cursor.tex + 1)
			     + "/" + std::to_string(m_asset_data.textures.size());
		if (m_upload_cursor.mat < m_asset_data.materials.size())
			return "Creating material " + std::to_string(m_upload_cursor.mat + 1)
			     + "/" + std::to_string(m_asset_data.materials.size());
		if (m_upload_cursor.mesh < m_asset_data.meshes.size())
			return "Uploading mesh " + std::to_string(m_upload_cursor.mesh + 1)
			     + "/" + std::to_string(m_asset_data.meshes.size());
	}
	if (m_state == LoadState::FINALIZING)
		return "Finalizing...";
	return {};
}

void AssetLoadingSystem::tick() {
	if (m_state == LoadState::IDLE || m_state == LoadState::READY || m_state == LoadState::FAILED)
		return;

	if (m_state == LoadState::CPU_LOADING) {
		if (m_progress.cpu_failed.load()) {
			joinWorker();
			VE_LOGE("Async model load failed: " << m_model_name);
			cancel();
			m_state = LoadState::FAILED;
			return;
		}
		if (m_progress.cancelled.load()) {
			joinWorker();
			m_state = LoadState::IDLE;
			return;
		}
		if (m_progress.cpu_done.load()) {
			joinWorker();
			m_state = LoadState::GPU_UPLOADING;
			VE_LOGI("CPU loading complete, beginning GPU upload for " << m_model_name);
		}
		return;
	}

	if (m_state == LoadState::GPU_UPLOADING) {
		auto& device = m_resource_manager.getDevice();

		// Drain retired batches from the front. Their arenas return to the pool.
		// Counter >= batch_value proves the batch's
		// graphics CB (and its awaited transfer CB) completed.
		uint64_t current_value = m_graphics_timeline.getCounterValue();
		while (!m_in_flight.empty() && m_in_flight.front().batch_value <= current_value) {
			auto& batch = m_in_flight.front();
			if (batch.arena) {
				batch.arena->reset();
				m_arena_pool.push_back(std::move(batch.arena));
			}
			m_in_flight.pop_front();
		}

		auto cursorExhausted = [&]() {
			return m_upload_cursor.tex >= m_asset_data.textures.size()
			    && m_upload_cursor.mesh >= m_asset_data.meshes.size()
			    && m_upload_cursor.mat >= m_asset_data.materials.size();
		};

		// Fill the pipeline up to MAX_IN_FLIGHT_BATCHES
		while (!cursorExhausted() && m_in_flight.size() < MAX_IN_FLIGHT_BATCHES) {
			// Acquire a staging arena
			std::unique_ptr<StagingArena> arena;
			if (!m_arena_pool.empty()) {
				arena = std::move(m_arena_pool.back());
				m_arena_pool.pop_back();
			} else {
				arena = std::make_unique<StagingArena>(device, MAX_STAGING_BYTES_PER_FRAME);
			}

			// Record next batch.
			auto transfer_cmd = device.beginSingleTimeCommands(QueueKind::Transfer);
			auto graphics_cmd = device.beginSingleTimeCommands(QueueKind::Graphics);
			size_t bytes_in_flight = 0;
			bool transfer_has_work = false;
			bool graphics_has_work = false;
			UploadContext ctx{*arena, *transfer_cmd, *graphics_cmd, bytes_in_flight, transfer_has_work, graphics_has_work};

			uploadLoadedAssetStep(m_resource_manager, m_asset_data, m_upload_cursor, m_uploaded,
			                      MAX_UPLOADS_PER_FRAME, MAX_STAGING_BYTES_PER_FRAME, ctx);

			// Mesh-only batches: graphics_cmd stays empty
			if (transfer_has_work)
				graphics_has_work = true;

			transfer_cmd->end();
			graphics_cmd->end();

			// Return the arena, try again.
			if (!transfer_has_work && !graphics_has_work) {
				m_arena_pool.push_back(std::move(arena));
				continue;
			}

			// Both queues signal their own timeline with the same batch value:
			uint64_t batch_value = ++m_batch_counter;
			vk::Semaphore transfer_sem = *m_transfer_timeline;
			vk::Semaphore graphics_sem = *m_graphics_timeline;

			{
				vk::TimelineSemaphoreSubmitInfo timeline_info{
					.sType = vk::StructureType::eTimelineSemaphoreSubmitInfo,
					.signalSemaphoreValueCount = 1,
					.pSignalSemaphoreValues = &batch_value,
				};
				vk::CommandBuffer cb = **transfer_cmd;
				vk::SubmitInfo submit_info{
					.pNext = &timeline_info,
					.commandBufferCount = 1,
					.pCommandBuffers = &cb,
					.signalSemaphoreCount = 1,
					.pSignalSemaphores = &transfer_sem,
				};
				device.getTransferQueue().submit(submit_info, nullptr);
			}

			{
				// Texture batches: graphics_cmd's first op is a layout barrier with
				// srcStage=eTransfer. Mesh-only batches: graphics_cmd is empty and
				// the semaphore wait alone bridges the queues.
				vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
				vk::TimelineSemaphoreSubmitInfo timeline_info{
					.sType = vk::StructureType::eTimelineSemaphoreSubmitInfo,
					.waitSemaphoreValueCount = 1,
					.pWaitSemaphoreValues = &batch_value,
					.signalSemaphoreValueCount = 1,
					.pSignalSemaphoreValues = &batch_value,
				};
				vk::CommandBuffer cb = **graphics_cmd;
				vk::SubmitInfo submit_info{
					.pNext = &timeline_info,
					.waitSemaphoreCount = 1,
					.pWaitSemaphores = &transfer_sem,
					.pWaitDstStageMask = &wait_stage,
					.commandBufferCount = 1,
					.pCommandBuffers = &cb,
					.signalSemaphoreCount = 1,
					.pSignalSemaphores = &graphics_sem,
				};
				device.getQueue().submit(submit_info, nullptr);
			}

			InFlightBatch batch;
			batch.transfer_cmd = std::move(transfer_cmd);
			batch.graphics_cmd = std::move(graphics_cmd);
			batch.arena = std::move(arena);
			batch.batch_value = batch_value;
			m_in_flight.push_back(std::move(batch));
		}

		if (cursorExhausted() && m_in_flight.empty())
			m_state = LoadState::FINALIZING;
		return;
	}

	if (m_state == LoadState::FINALIZING) {
		m_completed_model = std::make_shared<VeModel>(m_cache_key,
			std::move(m_asset_data), std::move(m_uploaded));
		auto elapsed = std::chrono::steady_clock::now() - m_load_start_time;
		m_last_load_seconds = std::chrono::duration<float>(elapsed).count();
		m_state = LoadState::READY;
		VE_LOGI("Async load complete: " << m_model_name << " in " << m_last_load_seconds << "s");
	}
}

void AssetLoadingSystem::waitForInFlight() {
	if (m_in_flight.empty())
		return;
	// Wait on the latest batch
	uint64_t target = m_in_flight.back().batch_value;
	vk::Semaphore sem = *m_graphics_timeline;
	vk::SemaphoreWaitInfo wait_info{
		.sType = vk::StructureType::eSemaphoreWaitInfo,
		.semaphoreCount = 1,
		.pSemaphores = &sem,
		.pValues = &target,
	};
	(void)m_resource_manager.getDevice().getDevice().waitSemaphores(wait_info, UINT64_MAX);
	for (auto& batch : m_in_flight) {
		if (batch.arena) {
			batch.arena->reset();
			m_arena_pool.push_back(std::move(batch.arena));
		}
	}
	m_in_flight.clear();
}

void AssetLoadingSystem::cancel() {
	if (m_worker.joinable()) {
		m_progress.cancelled = true;
		m_worker.join();
	}
	// Must wait on in-flight GPU work before dropping m_uploaded
	waitForInFlight();
	m_state = LoadState::IDLE;
	m_asset_data = {};
	m_uploaded = {};
	m_completed_model.reset();
}

std::shared_ptr<VeModel> AssetLoadingSystem::takeModel() {
	if (m_state != LoadState::READY)
		return nullptr;
	m_state = LoadState::IDLE;
	return std::move(m_completed_model);
}

void AssetLoadingSystem::joinWorker() {
	if (m_worker.joinable())
		m_worker.join();
}

} // namespace ve
