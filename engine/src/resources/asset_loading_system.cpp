#include "pch.hpp"
#include "resources/asset_loading_system.hpp"
#include "resources/ve_material.hpp"
#include "vulkan/ve_descriptors.hpp"
#include "utils/ve_log.hpp"

namespace ve {

AssetLoadingSystem::AssetLoadingSystem(VeResourceManager& resource_manager)
	: m_resource_manager(resource_manager) {}

AssetLoadingSystem::~AssetLoadingSystem() {
	cancel();
}

void AssetLoadingSystem::beginModelLoad(const std::filesystem::path& gltf_path,
                                        bool extract_lights, bool flip_tex_coord_v) {
	cancel();

	m_progress.reset();
	m_state = LoadState::CPU_LOADING;
	m_model_name = gltf_path.filename().string();
	m_mesh_upload_cursor = 0;
	m_mat_upload_cursor = 0;
	m_uploaded_meshes.clear();
	m_uploaded_materials.clear();
	m_completed_model.reset();
	m_load_start_time = std::chrono::steady_clock::now();

	m_progress.setStatus("Parsing " + m_model_name + "...");

	m_worker = std::thread([this, gltf_path, extract_lights, flip_tex_coord_v]() {
		try {
			m_asset_data = VeModel::loadFromGltfCpu(gltf_path, extract_lights, flip_tex_coord_v, m_progress);
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
	uint32_t total_gpu = static_cast<uint32_t>(m_asset_data.meshes.size()
	                   + m_asset_data.materials.size());
	if (total_gpu == 0)
		return 0.95f;
	uint32_t done_gpu = m_mesh_upload_cursor + m_mat_upload_cursor;
	float gpu_frac = static_cast<float>(done_gpu) / static_cast<float>(total_gpu);
	return 0.5f + gpu_frac * 0.5f;
}

std::string AssetLoadingSystem::getStatusMessage() {
	if (m_state == LoadState::CPU_LOADING)
		return m_progress.getStatus();
	if (m_state == LoadState::GPU_UPLOADING) {
		if (m_mesh_upload_cursor < m_asset_data.meshes.size())
			return "Uploading mesh " + std::to_string(m_mesh_upload_cursor + 1)
			     + "/" + std::to_string(m_asset_data.meshes.size());
		if (m_mat_upload_cursor < m_asset_data.materials.size())
			return "Creating material " + std::to_string(m_mat_upload_cursor + 1)
			     + "/" + std::to_string(m_asset_data.materials.size());
	}
	if (m_state == LoadState::FINALIZING)
		return "Finalizing...";
	return {};
}

void AssetLoadingSystem::tick(VeDescriptorPool* pool, VeDescriptorSetLayout* layout) {
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
		uint32_t uploads_this_frame = 0;

		// Upload meshes
		while (m_mesh_upload_cursor < m_asset_data.meshes.size() && uploads_this_frame < MAX_UPLOADS_PER_FRAME) {
			auto& mesh = m_asset_data.meshes[m_mesh_upload_cursor];
			auto handle = m_resource_manager.createMeshFromData(mesh.resource_id, mesh);
			m_uploaded_meshes.push_back(std::move(handle));
			mesh.vertices.clear();
			mesh.vertices.shrink_to_fit();
			mesh.indices.clear();
			mesh.indices.shrink_to_fit();
			mesh.lod_indices.clear();
			mesh.lod_indices.shrink_to_fit();
			m_mesh_upload_cursor++;
			uploads_this_frame++;
		}

		// Create materials (after all meshes uploaded).
		// Each material loads its own textures via VeTexture::loadOrDefault,
		// which handles format suffixes and caching correctly.
		if (m_mesh_upload_cursor >= m_asset_data.meshes.size()) {
			while (m_mat_upload_cursor < m_asset_data.materials.size() && uploads_this_frame < MAX_UPLOADS_PER_FRAME) {
				auto& pm = m_asset_data.materials[m_mat_upload_cursor];

				auto getTexPath = [&](int idx) -> std::filesystem::path {
					if (idx < 0 || static_cast<size_t>(idx) >= m_asset_data.textures.size())
						return "default_albedo.png";
					const auto& tex = m_asset_data.textures[static_cast<size_t>(idx)];
					if (tex.is_default)
						return "default_albedo.png";
					return tex.file_path;
				};

				bool has_textured = false;
				for (int idx : {pm.albedo_tex_idx, pm.normal_tex_idx, pm.metallic_roughness_tex_idx,
				                pm.occlusion_tex_idx, pm.emissive_tex_idx,
				                pm.specular_tex_idx, pm.specular_color_tex_idx}) {
					if (idx >= 0 && static_cast<size_t>(idx) < m_asset_data.textures.size()
					    && !m_asset_data.textures[static_cast<size_t>(idx)].is_default)
						has_textured = true;
				}

				auto mat_handle = m_resource_manager.createMaterial(
					pm.resource_id,
					getTexPath(pm.albedo_tex_idx),
					getTexPath(pm.normal_tex_idx),
					getTexPath(pm.metallic_roughness_tex_idx),
					getTexPath(pm.occlusion_tex_idx),
					getTexPath(pm.emissive_tex_idx),
					getTexPath(pm.specular_tex_idx),
					getTexPath(pm.specular_color_tex_idx),
					pm.alpha_props, pm.factors,
					has_textured ? pool : nullptr,
					has_textured ? layout : nullptr,
					pm.flip_tex_coord_v);
				m_uploaded_materials.push_back(std::move(mat_handle));
				m_mat_upload_cursor++;
				uploads_this_frame++;
			}
		}

		if (m_mesh_upload_cursor >= m_asset_data.meshes.size() &&
		    m_mat_upload_cursor >= m_asset_data.materials.size()) {
			m_state = LoadState::FINALIZING;
		}
		return;
	}

	if (m_state == LoadState::FINALIZING) {
		VeTexture::clearEmbeddedCache();
		m_completed_model = VeModel::fromLoadedData(
			std::move(m_asset_data), m_uploaded_meshes, m_uploaded_materials);
		auto elapsed = std::chrono::steady_clock::now() - m_load_start_time;
		m_last_load_seconds = std::chrono::duration<float>(elapsed).count();
		m_state = LoadState::READY;
		VE_LOGI("Async load complete: " << m_model_name << " in " << m_last_load_seconds << "s");
	}
}

void AssetLoadingSystem::cancel() {
	if (m_worker.joinable()) {
		m_progress.cancelled = true;
		m_worker.join();
	}
	VeTexture::clearEmbeddedCache();
	m_state = LoadState::IDLE;
	m_asset_data = {};
	m_uploaded_meshes.clear();
	m_uploaded_materials.clear();
	m_completed_model.reset();
}

std::unique_ptr<VeModel> AssetLoadingSystem::takeModel() {
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
