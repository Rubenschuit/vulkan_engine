#include "pch.hpp"
#include "resources/scene_instantiation.hpp"
#include "resources/ve_model.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_mesh.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <unordered_set>

namespace ve {

static constexpr float EMISSIVE_LIGHT_INTENSITY_SCALE = 10.0f;
static constexpr float KHR_PUNCTUAL_INTENSITY_SCALE = 1.0f / 1000.0f;

Entity instantiateModel(const VeModel& model, Registry& registry,
                        const std::string& name,
                        const glm::vec3& root_translation,
                        const glm::vec3& root_rotation,
                        const glm::vec3& root_scale) {
	// Suppress per-entity events during bulk creation
	registry.events().beginBatch();

	// Wrapper entity for root transform
	Entity wrapper = registry.createGameObject(name);
	auto* wrapper_tc = registry.getComponent<TransformComponent>(wrapper);
	wrapper_tc->setTranslation(root_translation);
	wrapper_tc->setRotationEuler(root_rotation);
	wrapper_tc->setScale(root_scale);

	const auto& nodes = model.nodes();
	const auto& parent_links = model.parentLinks();
	const auto& root_indices = model.rootIndices();
	const auto& mesh_handles = model.meshHandles();
	const auto& material_handles = model.materialHandles();
	const auto& skins = model.skins();
	const auto& gltf_to_loaded_idx = model.gltfToLoadedIdx();
	const auto& animation_clips = model.animationClips();
	const auto& punctual_lights = model.punctualLights();
	const auto& emissive_lights = model.emissiveLights();
	const auto& cameras = model.cameras();

	// Build parent lookup (node index gives parent index)
	std::unordered_map<uint32_t, uint32_t> parent_of;
	for (const auto& [child_idx, parent_idx] : parent_links)
		parent_of[child_idx] = parent_idx;

	auto localMatrix = [](const ModelNode& n) -> glm::mat4 {
		return glm::translate(glm::mat4(1.0f), n.translation)
			* glm::mat4_cast(n.rotation)
			* glm::scale(glm::mat4(1.0f), n.scale);
	};
	auto worldTransform = [&](uint32_t idx) -> glm::mat4 {
		std::vector<uint32_t> chain;
		for (uint32_t cur = idx; ; ) {
			chain.push_back(cur);
			auto it = parent_of.find(cur);
			if (it == parent_of.end()) break;
			cur = it->second;
		}
		glm::mat4 world(1.0f);
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
			world *= localMatrix(nodes[*it]);
		return world;
	};

	// Dedup: same mesh+material at same world position
	struct DedupKey {
		const void* mesh;
		const void* material;
		int32_t qx, qy, qz;
		bool operator==(const DedupKey&) const = default;
	};
	struct DedupHash {
		size_t operator()(const DedupKey& k) const {
			size_t h = std::hash<const void*>{}(k.mesh);
			h ^= std::hash<const void*>{}(k.material) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.qx) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.qy) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int32_t>{}(k.qz) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};
	std::unordered_set<DedupKey, DedupHash> mesh_dedup;
	uint32_t dedup_count = 0;

	VE_LOGI("instantiateModel: nodes=" << nodes.size()
	        << " meshes=" << mesh_handles.size()
	        << " materials=" << material_handles.size()
	        << " skins=" << skins.size());

	uint32_t mesh_attached = 0;
	uint32_t mesh_skipped_invalid = 0;

	// Map node indices to new Entity IDs
	std::vector<Entity> index_to_entity(nodes.size());
	for (uint32_t i = 0; i < static_cast<uint32_t>(nodes.size()); i++) {
		const auto& node = nodes[i];
		Entity entity = registry.createEntity(node.name);
		index_to_entity[i] = entity;

		// TransformComponent (every node has TRS)
		auto& tc = registry.addComponent<TransformComponent>(entity);
		tc.setTranslation(node.translation);
		tc.setRotation(node.rotation);
		tc.setScale(node.scale);

		// MeshComponent (only if node has valid mesh+material)
		if (node.mesh_idx >= 0 && node.material_idx >= 0
		    && static_cast<size_t>(node.mesh_idx) < mesh_handles.size()
		    && static_cast<size_t>(node.material_idx) < material_handles.size()) {
			const auto& mesh_h = mesh_handles[static_cast<size_t>(node.mesh_idx)];
			const auto& mat_h = material_handles[static_cast<size_t>(node.material_idx)];
			bool skip_dedup = node.skin_idx >= 0;
			bool add_mesh = skip_dedup;
			if (!skip_dedup) {
				glm::vec3 pos(worldTransform(i)[3]);
				DedupKey key{
					mesh_h.get(), mat_h.get(),
					static_cast<int32_t>(std::round(pos.x * 1000.0f)),
					static_cast<int32_t>(std::round(pos.y * 1000.0f)),
					static_cast<int32_t>(std::round(pos.z * 1000.0f))
				};
				if (mesh_dedup.insert(key).second)
					add_mesh = true;
				else
					dedup_count++;
			}
			if (add_mesh) {
				auto& mc = registry.addComponent<MeshComponent>(entity, mesh_h, mat_h);
				auto* mat = mat_h.get();
				mc.has_texture = (mat && mat->getAlbedoTexture().isValid()) ? 1.0f : 0.0f;
				mesh_attached++;
			}
		} else if (node.mesh_idx >= 0 || node.material_idx >= 0) {
			mesh_skipped_invalid++;
		}
	}
	if (dedup_count > 0)
		VE_LOGI("instantiateModel: skipped " << dedup_count << " duplicate mesh instances");
	VE_LOGI("instantiateModel: attached " << mesh_attached << " MeshComponents; skipped "
	        << mesh_skipped_invalid << " nodes with invalid mesh/material idx");

	// Attach SkinComponents
	uint32_t skin_attached = 0;
	for (uint32_t i = 0; i < static_cast<uint32_t>(nodes.size()); i++) {
		const auto& node = nodes[i];
		if (node.skin_idx < 0 || static_cast<size_t>(node.skin_idx) >= skins.size())
			continue;
		Entity entity = index_to_entity[i];
		if (entity.isNull() || !registry.isAlive(entity))
			continue;
		const ModelSkin& skin = skins[static_cast<size_t>(node.skin_idx)];

		std::vector<Entity> joint_entities;
		joint_entities.reserve(skin.joint_node_indices.size());
		for (size_t j = 0; j < skin.joint_node_indices.size(); j++) {
			int gltf_joint_idx = skin.joint_node_indices[j];
			Entity je = Entity::null();
			auto it = gltf_to_loaded_idx.find(gltf_joint_idx);
			if (it != gltf_to_loaded_idx.end() && it->second < index_to_entity.size())
				je = index_to_entity[it->second];
			if (!je.isNull() && registry.isAlive(je) && registry.getName(je).empty())
				registry.setName(je, "Joint " + std::to_string(j));
			joint_entities.push_back(je);
		}

		Entity skeleton_root = Entity::null();
		if (skin.skeleton_root_node >= 0) {
			auto it = gltf_to_loaded_idx.find(skin.skeleton_root_node);
			if (it != gltf_to_loaded_idx.end() && it->second < index_to_entity.size())
				skeleton_root = index_to_entity[it->second];
		}

		auto& sc = registry.addComponent<SkinComponent>(entity);
		sc.setJointEntities(std::move(joint_entities));
		sc.setInverseBindMatrices(skin.inverse_bind_matrices);
		sc.setSkeletonRoot(skeleton_root);
		skin_attached++;

		// Bake IBM into the per-joint extents so runtime can do (joint_world * extent).
		auto* mc = registry.getComponent<MeshComponent>(entity);
		VeMesh* mesh = mc ? mc->getMesh() : nullptr;
		const auto& mesh_extents = mesh ? mesh->getJointMeshLocalExtents() : std::vector<VeMesh::AABB>{};
		if (!mesh_extents.empty()) {
			std::vector<VeMesh::AABB> joint_local;
			joint_local.reserve(skin.inverse_bind_matrices.size());
			for (size_t j = 0; j < skin.inverse_bind_matrices.size(); j++) {
				if (j < mesh_extents.size())
					joint_local.push_back(transformAABB(mesh_extents[j], skin.inverse_bind_matrices[j]));
				else
					joint_local.push_back({glm::vec3(0.0f), glm::vec3(0.0f)});
			}
			sc.setJointLocalExtents(std::move(joint_local));
		}
	}

	VE_LOGI("instantiateModel: attached " << skin_attached << " SkinComponents");

	// Set up hierarchy from parent links
	for (const auto& [child_idx, parent_idx] : parent_links)
		registry.setParent(index_to_entity[child_idx], index_to_entity[parent_idx]);
	// Make all glTF roots children of the wrapper
	for (uint32_t root_idx : root_indices)
		registry.setParent(index_to_entity[root_idx], wrapper);

	// Create AnimatorComponent on wrapper entity if model has animations
	if (!animation_clips.empty()) {
		auto& animator = registry.addComponent<AnimatorComponent>(wrapper);
		animator.setNodeToEntityMap(index_to_entity);
		// Only auto-play the first clip.
		for (size_t i = 0; i < animation_clips.size(); i++)
			animator.addClip(animation_clips[i], /*auto_play=*/i == 0, /*loop=*/true);
	}

	registry.events().endBatch();

	// Resolve the parent entity for an extracted light: use the source glTF node if known, else wrapper
	auto lightParent = [&](const ExtractedLight& L) -> Entity {
		if (L.node_idx >= 0) {
			auto it = gltf_to_loaded_idx.find(L.node_idx);
			if (it != gltf_to_loaded_idx.end() && it->second < index_to_entity.size())
				return index_to_entity[it->second];
		}
		return wrapper;
	};

	// L.position is in wrapper-local space (glTF world, before root transform).
	// Convert to parent-local space: first apply wrapper transform to get scene world, then invert parent.
	glm::mat4 wrapper_world = registry.getWorldTransform(wrapper);
	auto toLocalPos = [&](const glm::vec3& pos, Entity parent) -> glm::vec3 {
		if (parent == wrapper)
			return pos;
		glm::vec3 scene_world = glm::vec3(wrapper_world * glm::vec4(pos, 1.0f));
		glm::mat4 inv_parent = glm::inverse(registry.getWorldTransform(parent));
		return glm::vec3(inv_parent * glm::vec4(scene_world, 1.0f));
	};

	// Extracted lights: parent to source node entity (or wrapper as fallback)
	constexpr float size = 0.1f;
	for (const ExtractedLight& L : emissive_lights) {
		Entity parent = lightParent(L);
		Entity light = registry.createPointLight(L.intensity * EMISSIVE_LIGHT_INTENSITY_SCALE, size, L.color);
		registry.setName(light, L.name.empty() ? "Light (emissive)" : L.name);
		registry.setLightSource(light, LightSource::Emissive);
		auto* tc = registry.getComponent<TransformComponent>(light);
		tc->setTranslation(toLocalPos(L.position, parent));
		registry.setParent(light, parent);
		registry.setActive(light, false);  // emissive-derived; default OFF, user opts in per-light
	}
	for (const ExtractedLight& L : punctual_lights) {
		Entity parent = lightParent(L);
		float scaled_intensity = L.intensity * KHR_PUNCTUAL_INTENSITY_SCALE;
		if (L.type == ExtractedLightType::Directional) {
			Entity light = registry.createDirectionalLight(scaled_intensity, L.color, L.direction);
			registry.setName(light, L.name.empty() ? "Light (directional)" : L.name);
			registry.setLightSource(light, LightSource::Punctual);
			registry.setParent(light, parent);
			registry.setActive(light, false);
		} else if (L.type == ExtractedLightType::Spot) {
			Entity light = registry.createSpotLight(scaled_intensity, size, L.color,
				L.direction, L.inner_cone_angle, L.outer_cone_angle);
			registry.setName(light, L.name.empty() ? "Light (spot)" : L.name);
			registry.setLightSource(light, LightSource::Punctual);
			auto* tc = registry.getComponent<TransformComponent>(light);
			tc->setTranslation(toLocalPos(L.position, parent));
			registry.setParent(light, parent);
			auto* slc = registry.getComponent<SpotLightComponent>(light);
			if (slc) slc->setRange(L.range);
			registry.setActive(light, false);
		} else {
			Entity light = registry.createPointLight(scaled_intensity, size, L.color);
			registry.setName(light, L.name.empty() ? "Light (imported)" : L.name);
			registry.setLightSource(light, LightSource::Punctual);
			auto* tc = registry.getComponent<TransformComponent>(light);
			tc->setTranslation(toLocalPos(L.position, parent));
			registry.setParent(light, parent);
			auto* plc = registry.getComponent<PointLightComponent>(light);
			if (plc) plc->setRange(L.range);
			registry.setActive(light, false);
		}
	}

	// Cameras: attach a CameraComponent to the entity that owns the source node.
	int unnamed_perspective = 0;
	int unnamed_orthographic = 0;
	for (const ExtractedCamera& C : cameras) {
		auto it = gltf_to_loaded_idx.find(C.node_idx);
		if (it == gltf_to_loaded_idx.end() || it->second >= index_to_entity.size())
			continue;
		Entity e = index_to_entity[it->second];
		if (e.isNull() || registry.hasComponent<CameraComponent>(e))
			continue;
		auto& cc = registry.addComponent<CameraComponent>(e);
		cc.setProjection(C.perspective ? CameraComponent::ProjectionType::Perspective
		                               : CameraComponent::ProjectionType::Orthographic);
		cc.setFovY(C.yfov_radians);
		cc.setOrthoSize(C.ortho_size);
		cc.setNear(C.znear);
		cc.setFar(C.zfar);
		if (registry.getName(e).empty()) {
			if (!C.name.empty()) {
				registry.setName(e, C.name);
			} else {
				int& counter = C.perspective ? unnamed_perspective : unnamed_orthographic;
				std::string label = C.perspective ? "Camera (perspective)" : "Camera (orthographic)";
				if (counter > 0)
					label += " " + std::to_string(counter);
				registry.setName(e, label);
				counter++;
			}
		}
	}

	return wrapper;
}

} // namespace ve