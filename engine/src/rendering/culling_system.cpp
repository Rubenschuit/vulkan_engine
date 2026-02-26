#include "pch.hpp"
#include "rendering/culling_system.hpp"
#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "resources/ve_mesh.hpp"
#include "utils/ve_frustum.hpp"
#include "vulkan/ve_thread_pool.hpp"
#include "ve_config.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace ve {

CullingSystem::CullingSystem(VeCamera& camera)
	: m_camera(&camera) {
}

// Thread-safe: each worker processes a disjoint range of dense indices,
// so writes to mesh.cached_lod touch distinct MeshComponent objects.
void CullingSystem::processEntity(const CullParams& params, uint32_t dense_idx,
	std::vector<VisibleObject>& out, uint32_t& mesh_count) {

	auto& registry = *params.registry;
	uint32_t entity_idx = params.entity_indices[dense_idx];
	if (!registry.isActiveAtIndex(entity_idx))
		return;
	if (!registry.transforms().has(entity_idx))
		return;

	auto& mesh = registry.meshes().data()[dense_idx];
	if (!mesh.hasMesh() || !mesh.hasMaterial())
		return;

	mesh_count++;

	const VeMesh::AABB world_aabb = mesh.getWorldAABB();
	if (m_culling_enabled && !isAABBInFrustum(world_aabb, params.planes))
		return;

	uint32_t lod = 0;
	VeMesh* mesh_ptr = mesh.getMesh();
	if (m_force_lod >= 0) {
		lod = std::min(static_cast<uint32_t>(m_force_lod), mesh_ptr ? mesh_ptr->getLodCount() - 1 : 0u);
		mesh.cached_lod = lod;
	} else if (mesh_ptr && mesh_ptr->getLodCount() > 1) {
		glm::vec3 center = (world_aabb.min + world_aabb.max) * 0.5f;
		float radius = glm::length(world_aabb.max - world_aabb.min) * 0.5f;
		float dist = glm::length(center - params.cam_pos);
		if (dist > 0.001f) {
			float screen_fraction = radius / (dist * params.half_tan_fov);
			uint32_t prev_lod = mesh.cached_lod;
			for (uint32_t t = 0; t < ve::MAX_LOD_LEVELS - 1 && t < mesh_ptr->getLodCount() - 1; t++) {
				float threshold = m_lod_thresholds[t];
				if (prev_lod > t)
					threshold *= (1.0f + m_lod_hysteresis);
				if (screen_fraction < threshold)
					lod = t + 1;
			}
			mesh.cached_lod = lod;
		}
	}

	Entity entity = registry.entityFromIndex(entity_idx);
	out.push_back({entity, &mesh, lod});
}

void CullingSystem::cullObjects(VeFrameInfo& frame_info, VeThreadPool* thread_pool) {
	m_visible_objects.clear();
	m_last_total_mesh_objects = 0;
	m_last_visible_count = 0;

	FrustumPlane planes[6];
	if (m_culling_enabled) {
		m_camera->updateIfDirty();
		const glm::mat4 view_proj = m_camera->getProj() * m_camera->getView();
		extractFrustumPlanes(view_proj, planes);
	}

	auto& registry = *frame_info.registry;
	auto& mesh_pool = registry.meshes();
	const uint32_t total = mesh_pool.size();

	CullParams params{
		.registry = &registry,
		.entity_indices = mesh_pool.entityIndexData(),
		.planes = planes,
		.cam_pos = m_camera->getPosition(),
		.half_tan_fov = std::tan(m_camera->getFovY() * 0.5f),
	};

	uint32_t N = thread_pool ? thread_pool->workerCount() : 0;

	if (N > 0 && total >= m_min_parallel_entities) {
		// ── Parallel path ──
		std::vector<std::vector<VisibleObject>> staging(N);
		std::vector<uint32_t> local_totals(N, 0);

		thread_pool->dispatch([&](uint32_t wi, ThreadSlot) {
			uint32_t chunk = (total + N - 1) / N;
			uint32_t begin = wi * chunk;
			uint32_t end = std::min(begin + chunk, total);

			staging[wi].reserve((end - begin) / 2);

			for (uint32_t d = begin; d < end; d++)
				processEntity(params, d, staging[wi], local_totals[wi]);
		});

		// Merge per-worker results
		uint32_t total_visible = 0;
		uint32_t total_mesh = 0;
		for (uint32_t i = 0; i < N; i++) {
			total_visible += static_cast<uint32_t>(staging[i].size());
			total_mesh += local_totals[i];
		}
		m_visible_objects.reserve(total_visible);
		for (uint32_t i = 0; i < N; i++)
			m_visible_objects.insert(m_visible_objects.end(), staging[i].begin(), staging[i].end());

		m_last_total_mesh_objects = total_mesh;
		m_last_visible_count = total_visible;
	} else {
		// ── Single-threaded path ──
		m_visible_objects.reserve(total);
		for (uint32_t d = 0; d < total; d++)
			processEntity(params, d, m_visible_objects, m_last_total_mesh_objects);
		m_last_visible_count = static_cast<uint32_t>(m_visible_objects.size());
	}
}

} // namespace ve
