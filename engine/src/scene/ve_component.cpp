#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ecs_event_dispatcher.hpp"
#include "resources/ve_mesh.hpp"
#include "ve_config.hpp"

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <algorithm>
#include <cmath>

namespace ve {

size_t ComponentTypeIDSystem::m_next_type_id = 0;

// Explicit instantiations ensure single definition across DLL boundary.
// Without these, engine and app each implicitly instantiate the template with
// different static locals so type ID mismatch and getComponent returns nullptr.
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<DirectionalLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SpotLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<RigidbodyComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<AnimatorComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SkinComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<CameraComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<ParticleEmitterComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MorphComponent>();


// ---------------------------------------------------------------------------
// TransformComponent
// ---------------------------------------------------------------------------

const glm::mat4& TransformComponent::getTransform() const {
	if (m_transform_dirty)
		updateTransform();
	return m_cached_transform;
}

const glm::mat3& TransformComponent::getNormalTransform() const {
	if (m_transform_dirty)
		updateTransform();
	return m_cached_normal_transform;
}

void TransformComponent::setRotationEuler(glm::vec3 euler_rad) {
	// Match previous Euler order: R = Rz(z) * Ry(y) * Rx(x)
	rotation = glm::quat_cast(glm::eulerAngleZYX(euler_rad.z, euler_rad.y, euler_rad.x));
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

void TransformComponent::setTranslation(glm::vec3 pos) {
	translation = pos;
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

void TransformComponent::setRotation(glm::quat q) {
	rotation = q;
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

void TransformComponent::setScale(glm::vec3 s) {
	scale = s;
	m_transform_dirty = true;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}

// Updates mutable private members m_cached_transform, m_cached_normal_transform
// and m_transform_dirty.
void TransformComponent::updateTransform() const {
	const glm::mat4 R = glm::mat4_cast(rotation);
	const glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
	const glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
	m_cached_transform = T * R * S;

	const glm::vec3 inverse_scale = 1.0f / scale;
	const glm::mat3 R3 = glm::mat3_cast(rotation);
	const glm::mat3 S3(inverse_scale.x, 0.0f, 0.0f, 0.0f, inverse_scale.y, 0.0f, 0.0f, 0.0f, inverse_scale.z);
	m_cached_normal_transform = R3 * S3;

	m_transform_dirty = false;
	if (m_registry)
		m_registry->invalidateWorldTransform(m_entity);
}


// ---------------------------------------------------------------------------
// PointLightComponent
// ---------------------------------------------------------------------------

void PointLightComponent::setIntensity(float v) {
	m_intensity = v;
	m_range_dirty = true;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void PointLightComponent::setColor(const glm::vec3& v) {
	m_color = v;
	m_range_dirty = true;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void PointLightComponent::setRange(float v) {
	m_range = v;
	m_range_dirty = true;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

float PointLightComponent::getEffectiveRange() const {
	if (m_range_dirty)
		updateEffectiveRange();
	return m_effective_range;
}

void PointLightComponent::updateEffectiveRange() const {
	if (m_range > 0.0f) {
		m_effective_range = m_range;
	} else {
		float max_i = std::max({m_color.r * m_intensity, m_color.g * m_intensity, m_color.b * m_intensity});
		m_effective_range = std::min(std::sqrt(max_i / CLUSTER_LIGHT_CUTOFF), CLUSTER_MAX_EFFECTIVE_RANGE);
	}
	m_range_dirty = false;
}

void PointLightComponent::setRotates(bool v) {
	m_rotates = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void PointLightComponent::setCastsShadow(bool v) {
	m_casts_shadow = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void PointLightComponent::update(float /*delta_time*/) {}

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------

const VeMesh::AABB& MeshComponent::getWorldAABB() const {
	if (m_world_aabb_dirty)
		updateWorldAABB();
	return m_cached_world_aabb;
}

void MeshComponent::invalidateWorldAABB() {
	m_world_aabb_dirty = true;
}

void MeshComponent::editMaterialFactors(const std::function<void(MaterialFactors&)>& fn) {
	VeMaterial* mat = getMaterial();
	if (!mat)
		return;
	MaterialFactors factors = mat->getMaterialFactors();
	fn(factors);
	mat->setMaterialFactors(factors);
	if (m_registry)
		m_registry->events().emit(MeshDataChangedEvent{m_entity});
}

void MeshComponent::updateWorldAABB() const {
	assert(m_registry && "MeshComponent must have Registry context for world AABB");
	const glm::mat4& model = m_registry->getWorldTransform(m_entity);
	const VeMesh* mesh = getMesh();
	const bool has_morph = mesh && mesh->hasMorphTargets()
		&& m_registry->hasComponent<MorphComponent>(m_entity);

	const auto* skin = m_registry->getComponent<SkinComponent>(m_entity);
	const auto& joint_local = skin ? skin->getJointLocalExtents() : std::vector<VeMesh::AABB>{};
	const auto& joints = skin ? skin->getJointEntities() : std::vector<Entity>{};
	bool have_skinned_bound = false;
	if (skin && !joint_local.empty() && joint_local.size() == joints.size()) {
		bool first = true;
		for (size_t j = 0; j < joints.size(); j++) {
			Entity je = joints[j];
			if (je.isNull() || !m_registry->isAlive(je))
				continue;
			VeMesh::AABB world_j = transformAABB(joint_local[j], m_registry->getWorldTransform(je));
			m_cached_world_aabb = first ? world_j : unionAABB(m_cached_world_aabb, world_j);
			first = false;
		}
		have_skinned_bound = !first;
	}

	if (!have_skinned_bound) {
		// No skinning: a morph-only mesh uses the conservative bound; a missing mesh
		// degenerates to a point at the entity origin.
		VeMesh::AABB local{glm::vec3(0.0f), glm::vec3(0.0f)};
		if (has_morph)
			local = mesh->getMorphLocalAABB();
		else if (mesh)
			local = mesh->getLocalAABB();
		m_cached_world_aabb = transformAABB(local, model);
	} else if (has_morph) {
		// The skinned bound tracks the animated base mesh; union so morph
		// displacement is conservatively included too.
		m_cached_world_aabb = unionAABB(m_cached_world_aabb,
		                                transformAABB(mesh->getMorphLocalAABB(), model));
	}
	m_world_aabb_dirty = false;
}

void MeshComponent::render() {
// unused, render systems handle this for now
}

// ---------------------------------------------------------------------------
// DirectionalLightComponent
// ---------------------------------------------------------------------------

void DirectionalLightComponent::setDirection(const glm::vec3& v) {
	m_direction = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void DirectionalLightComponent::setColor(const glm::vec3& v) {
	m_color = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void DirectionalLightComponent::setIntensity(float v) {
	m_intensity = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void DirectionalLightComponent::setCastsShadow(bool v) {
	m_casts_shadow = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void DirectionalLightComponent::setCelestialType(CelestialType t) {
	m_celestial_type = t;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

// ---------------------------------------------------------------------------
// SpotLightComponent
// ---------------------------------------------------------------------------

void SpotLightComponent::setIntensity(float v) {
	m_intensity = v;
	m_range_dirty = true;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void SpotLightComponent::setColor(const glm::vec3& v) {
	m_color = v;
	m_range_dirty = true;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void SpotLightComponent::setRange(float v) {
	m_range = v;
	m_range_dirty = true;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void SpotLightComponent::setDirection(const glm::vec3& v) {
	m_direction = glm::normalize(v);
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void SpotLightComponent::setInnerConeAngle(float radians) {
	m_inner_cone_angle = radians;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void SpotLightComponent::setOuterConeAngle(float radians) {
	m_outer_cone_angle = radians;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void SpotLightComponent::setCastsShadow(bool v) {
	m_casts_shadow = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

float SpotLightComponent::getEffectiveRange() const {
	if (m_range_dirty)
		updateEffectiveRange();
	return m_effective_range;
}

void SpotLightComponent::updateEffectiveRange() const {
	if (m_range > 0.0f) {
		m_effective_range = m_range;
	} else {
		float max_i = std::max({m_color.r * m_intensity, m_color.g * m_intensity, m_color.b * m_intensity});
		m_effective_range = std::min(std::sqrt(max_i / CLUSTER_LIGHT_CUTOFF), CLUSTER_MAX_EFFECTIVE_RANGE);
	}
	m_range_dirty = false;
}

// ---------------------------------------------------------------------------
// RigidbodyComponent
// ---------------------------------------------------------------------------

void RigidbodyComponent::setMotionType(PhysicsMotionType t) {
	m_motion_type = t;
	m_dirty = true;
	if (m_registry)
		m_registry->events().emit(RigidbodyChangedEvent{m_entity});
}

void RigidbodyComponent::setShapeDesc(const PhysicsShapeDesc& s) {
	m_shape = s;
	m_dirty = true;
	if (m_registry)
		m_registry->events().emit(RigidbodyChangedEvent{m_entity});
}

void RigidbodyComponent::setMass(float m) {
	m_mass = m;
	m_dirty = true;
	if (m_registry)
		m_registry->events().emit(RigidbodyChangedEvent{m_entity});
}

void RigidbodyComponent::setFriction(float f) {
	m_friction = f;
	m_dirty = true;
	if (m_registry)
		m_registry->events().emit(RigidbodyChangedEvent{m_entity});
}

void RigidbodyComponent::setRestitution(float r) {
	m_restitution = r;
	m_dirty = true;
	if (m_registry)
		m_registry->events().emit(RigidbodyChangedEvent{m_entity});
}

void RigidbodyComponent::setHullTolerance(float t) {
	m_hull_tolerance = t;
	m_dirty = true;
	if (m_registry)
		m_registry->events().emit(RigidbodyChangedEvent{m_entity});
}

// ---------------------------------------------------------------------------
// AnimatorComponent - sampling helpers
// ---------------------------------------------------------------------------

// Find the keyframe interval for time t. Returns the lower index.
static uint32_t findKeyframe(const std::vector<float>& timestamps, float t) {
	if (timestamps.size() <= 1)
		return 0;
	// Binary search for the last timestamp <= t
	auto it = std::upper_bound(timestamps.begin(), timestamps.end(), t);
	if (it == timestamps.begin())
		return 0;
	uint32_t idx = static_cast<uint32_t>(std::distance(timestamps.begin(), it)) - 1;
	if (idx >= timestamps.size() - 1)
		return static_cast<uint32_t>(timestamps.size()) - 2;
	return idx;
}

static glm::vec3 sampleVec3(const AnimationSampler& sampler, float t, AnimationInterpolation interp) {
	if (sampler.timestamps.empty())
		return glm::vec3(0.0f);
	if (sampler.timestamps.size() == 1 || t <= sampler.timestamps.front()) {
		const float* v = &sampler.values[interp == AnimationInterpolation::CubicSpline ? 3 : 0];
		return {v[0], v[1], v[2]};
	}
	if (t >= sampler.timestamps.back()) {
		size_t last = sampler.timestamps.size() - 1;
		size_t offset = interp == AnimationInterpolation::CubicSpline ? (last * 3 + 1) * 3 : last * 3;
		const float* v = &sampler.values[offset];
		return {v[0], v[1], v[2]};
	}

	uint32_t i = findKeyframe(sampler.timestamps, t);
	float t0 = sampler.timestamps[i];
	float t1 = sampler.timestamps[i + 1];
	float alpha = (t - t0) / (t1 - t0);

	if (interp == AnimationInterpolation::Step) {
		const float* v = &sampler.values[i * 3];
		return {v[0], v[1], v[2]};
	}

	if (interp == AnimationInterpolation::Linear) {
		const float* v0 = &sampler.values[i * 3];
		const float* v1 = &sampler.values[(i + 1) * 3];
		return glm::mix(glm::vec3(v0[0], v0[1], v0[2]), glm::vec3(v1[0], v1[1], v1[2]), alpha);
	}

	// CubicSpline: each keyframe has 3 vec3 values (in-tangent, value, out-tangent)
	float dt = t1 - t0;
	size_t stride = 3 * 3; // 3 components * 3 (in, val, out)
	const float* k0_val = &sampler.values[i * stride + 3];       // value at i
	const float* k0_out = &sampler.values[i * stride + 6];       // out-tangent at i
	const float* k1_in  = &sampler.values[(i + 1) * stride];     // in-tangent at i+1
	const float* k1_val = &sampler.values[(i + 1) * stride + 3]; // value at i+1

	float a2 = alpha * alpha;
	float a3 = a2 * alpha;
	float h00 = 2.0f * a3 - 3.0f * a2 + 1.0f;
	float h10 = a3 - 2.0f * a2 + alpha;
	float h01 = -2.0f * a3 + 3.0f * a2;
	float h11 = a3 - a2;

	glm::vec3 p0(k0_val[0], k0_val[1], k0_val[2]);
	glm::vec3 m0(k0_out[0], k0_out[1], k0_out[2]);
	glm::vec3 p1(k1_val[0], k1_val[1], k1_val[2]);
	glm::vec3 m1(k1_in[0], k1_in[1], k1_in[2]);

	return h00 * p0 + h10 * dt * m0 + h01 * p1 + h11 * dt * m1;
}

static glm::quat sampleQuat(const AnimationSampler& sampler, float t, AnimationInterpolation interp) {
	if (sampler.timestamps.empty())
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	if (sampler.timestamps.size() == 1 || t <= sampler.timestamps.front()) {
		size_t offset = interp == AnimationInterpolation::CubicSpline ? 4 : 0;
		const float* v = &sampler.values[offset];
		return glm::normalize(glm::quat(v[3], v[0], v[1], v[2]));
	}
	if (t >= sampler.timestamps.back()) {
		size_t last = sampler.timestamps.size() - 1;
		size_t offset = interp == AnimationInterpolation::CubicSpline ? (last * 3 + 1) * 4 : last * 4;
		const float* v = &sampler.values[offset];
		return glm::normalize(glm::quat(v[3], v[0], v[1], v[2]));
	}

	uint32_t i = findKeyframe(sampler.timestamps, t);
	float t0 = sampler.timestamps[i];
	float t1 = sampler.timestamps[i + 1];
	float alpha = (t - t0) / (t1 - t0);

	if (interp == AnimationInterpolation::Step) {
		const float* v = &sampler.values[i * 4];
		return glm::normalize(glm::quat(v[3], v[0], v[1], v[2]));
	}

	if (interp == AnimationInterpolation::Linear) {
		const float* v0 = &sampler.values[i * 4];
		const float* v1 = &sampler.values[(i + 1) * 4];
		glm::quat q0(v0[3], v0[0], v0[1], v0[2]);
		glm::quat q1(v1[3], v1[0], v1[1], v1[2]);
		return glm::normalize(glm::slerp(q0, q1, alpha));
	}

	// Cubic spline
	float dt = t1 - t0;
	size_t stride = 3 * 4;
	const float* k0_val = &sampler.values[i * stride + 4];
	const float* k0_out = &sampler.values[i * stride + 8];
	const float* k1_in  = &sampler.values[(i + 1) * stride];
	const float* k1_val = &sampler.values[(i + 1) * stride + 4];

	float a2 = alpha * alpha;
	float a3 = a2 * alpha;
	float h00 = 2.0f * a3 - 3.0f * a2 + 1.0f;
	float h10 = a3 - 2.0f * a2 + alpha;
	float h01 = -2.0f * a3 + 3.0f * a2;
	float h11 = a3 - a2;

	glm::quat p0(k0_val[3], k0_val[0], k0_val[1], k0_val[2]);
	glm::quat m0(k0_out[3], k0_out[0], k0_out[1], k0_out[2]);
	glm::quat p1(k1_val[3], k1_val[0], k1_val[1], k1_val[2]);
	glm::quat m1(k1_in[3], k1_in[0], k1_in[1], k1_in[2]);

	glm::quat result;
	result.w = h00 * p0.w + h10 * dt * m0.w + h01 * p1.w + h11 * dt * m1.w;
	result.x = h00 * p0.x + h10 * dt * m0.x + h01 * p1.x + h11 * dt * m1.x;
	result.y = h00 * p0.y + h10 * dt * m0.y + h01 * p1.y + h11 * dt * m1.y;
	result.z = h00 * p0.z + h10 * dt * m0.z + h01 * p1.z + h11 * dt * m1.z;
	return glm::normalize(result);
}

// `n` is how many weights this mesh wants, 
// `m` is how many the animation stores per keyframe. 
// They match for normal meshes, but a multi-primitive mesh split across nodes 
// can share one channel whose width differs from a given primitive's target count.
// Weights are unclamped.
static void sampleScalarArray(const AnimationSampler& sampler, float t,
                              AnimationInterpolation interp, uint32_t n, std::vector<float>& out) {
	out.assign(n, 0.0f);
	const uint32_t m = sampler.weights_target_count;
	if (n == 0 || m == 0 || sampler.timestamps.empty() || sampler.values.empty())
		return;

	const size_t stride = (interp == AnimationInterpolation::CubicSpline) ? 3u * m : m;
	const size_t val_ofs = (interp == AnimationInterpolation::CubicSpline) ? m : 0u;
	const uint32_t cn = std::min(n, m);

	if (sampler.timestamps.size() == 1 || t <= sampler.timestamps.front()) {
		const float* v = &sampler.values[val_ofs];
		for (uint32_t c = 0; c < cn; c++) out[c] = v[c];
		return;
	}
	if (t >= sampler.timestamps.back()) {
		size_t last = sampler.timestamps.size() - 1;
		const float* v = &sampler.values[last * stride + val_ofs];
		for (uint32_t c = 0; c < cn; c++) out[c] = v[c];
		return;
	}

	uint32_t i = findKeyframe(sampler.timestamps, t);
	float t0 = sampler.timestamps[i];
	float t1 = sampler.timestamps[i + 1];
	float alpha = (t - t0) / (t1 - t0);

	if (interp == AnimationInterpolation::Step) {
		const float* v = &sampler.values[i * stride + val_ofs];
		for (uint32_t c = 0; c < cn; c++) out[c] = v[c];
		return;
	}

	if (interp == AnimationInterpolation::Linear) {
		const float* v0 = &sampler.values[i * stride + val_ofs];
		const float* v1 = &sampler.values[(i + 1) * stride + val_ofs];
		for (uint32_t c = 0; c < cn; c++) out[c] = glm::mix(v0[c], v1[c], alpha);
		return;
	}

	// Cubic spline
	float dt = t1 - t0;
	const float* k0_val = &sampler.values[i * stride + m];
	const float* k0_out = &sampler.values[i * stride + 2u * m];
	const float* k1_in  = &sampler.values[(i + 1) * stride];
	const float* k1_val = &sampler.values[(i + 1) * stride + m];

	float a2 = alpha * alpha;
	float a3 = a2 * alpha;
	float h00 = 2.0f * a3 - 3.0f * a2 + 1.0f;
	float h10 = a3 - 2.0f * a2 + alpha;
	float h01 = -2.0f * a3 + 3.0f * a2;
	float h11 = a3 - a2;

	for (uint32_t c = 0; c < cn; c++)
		out[c] = h00 * k0_val[c] + h10 * dt * k0_out[c] + h01 * k1_val[c] + h11 * dt * k1_in[c];
}

// ---------------------------------------------------------------------------
// AnimatorComponent
// ---------------------------------------------------------------------------

bool AnimatorComponent::hasPlayingClips() const {
	for (const auto& b : m_clip_bindings)
		if (b.playing && b.clip)
			return true;
	return false;
}

std::vector<Entity> AnimatorComponent::getAnimatedEntities() const {
	std::vector<Entity> result;
	for (const auto& binding : m_clip_bindings) {
		if (!binding.clip)
			continue;
		for (uint32_t loaded_idx : binding.clip->target_node_indices) {
			if (loaded_idx < m_node_to_entity.size())
				result.push_back(m_node_to_entity[loaded_idx]);
		}
	}
	// Deduplicate
	std::sort(result.begin(), result.end(), [](Entity a, Entity b) { return a.id() < b.id(); });
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

void AnimatorComponent::updateAnimatedFlags() {
	if (!m_registry)
		return;
	bool has_playing = hasPlayingClips();
	for (Entity target : getAnimatedEntities()) {
		if (m_registry->isAlive(target))
			m_registry->setAnimated(target, has_playing);
	}
}

uint32_t AnimatorComponent::addClip(std::shared_ptr<VeAnimationClip> clip, bool auto_play, bool loop) {
	uint32_t idx = static_cast<uint32_t>(m_clip_bindings.size());
	m_clip_bindings.push_back({
		.clip = std::move(clip),
		.current_time = 0.0f,
		.speed = 1.0f,
		.playing = auto_play,
		.loop = loop
	});
	if (auto_play && m_registry) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
	return idx;
}

void AnimatorComponent::play(uint32_t clip_index) {
	if (clip_index < m_clip_bindings.size()) {
		m_clip_bindings[clip_index].playing = true;
		if (m_registry) {
			updateAnimatedFlags();
			m_registry->events().emit(AnimationStateChangedEvent{m_entity});
		}
	}
}

void AnimatorComponent::pause(uint32_t clip_index) {
	if (clip_index < m_clip_bindings.size()) {
		m_clip_bindings[clip_index].playing = false;
		if (m_registry) {
			updateAnimatedFlags();
			m_registry->events().emit(AnimationStateChangedEvent{m_entity});
		}
	}
}

void AnimatorComponent::stop(uint32_t clip_index) {
	if (clip_index < m_clip_bindings.size()) {
		m_clip_bindings[clip_index].playing = false;
		m_clip_bindings[clip_index].current_time = 0.0f;
		if (m_registry) {
			updateAnimatedFlags();
			m_registry->events().emit(AnimationStateChangedEvent{m_entity});
		}
	}
}

void AnimatorComponent::setSpeed(uint32_t clip_index, float speed) {
	if (clip_index < m_clip_bindings.size())
		m_clip_bindings[clip_index].speed = speed;
}

void AnimatorComponent::setLoop(uint32_t clip_index, bool loop) {
	if (clip_index < m_clip_bindings.size())
		m_clip_bindings[clip_index].loop = loop;
}

void AnimatorComponent::setTime(uint32_t clip_index, float time) {
	if (clip_index < m_clip_bindings.size())
		m_clip_bindings[clip_index].current_time = time;
}

void AnimatorComponent::playAll() {
	bool changed = false;
	for (auto& b : m_clip_bindings) {
		if (b.clip && !b.playing) {
			b.playing = true;
			changed = true;
		}
	}
	if (changed && m_registry) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
}

void AnimatorComponent::pauseAll() {
	bool changed = false;
	for (auto& b : m_clip_bindings) {
		if (b.clip && b.playing) {
			b.playing = false;
			changed = true;
		}
	}
	if (changed && m_registry) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
}

void AnimatorComponent::stopAll() {
	bool state_changed = false;
	for (auto& b : m_clip_bindings) {
		if (!b.clip)
			continue;
		if (b.playing) {
			b.playing = false;
			state_changed = true;
		}
		b.current_time = 0.0f;
	}
	if (state_changed && m_registry) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
}

void AnimatorComponent::remapEntities(const std::unordered_map<uint32_t, Entity>& old_to_new) {
	for (auto& entity : m_node_to_entity) {
		auto it = old_to_new.find(entity.index());
		if (it != old_to_new.end())
			entity = it->second;
	}
}

void SkinComponent::remapEntities(const std::unordered_map<uint32_t, Entity>& old_to_new) {
	for (auto& entity : m_joint_entities) {
		auto it = old_to_new.find(entity.index());
		if (it != old_to_new.end())
			entity = it->second;
	}
	if (!m_skeleton_root.isNull()) {
		auto it = old_to_new.find(m_skeleton_root.index());
		if (it != old_to_new.end())
			m_skeleton_root = it->second;
	}
}

void AnimatorComponent::update(float delta_time) {
	if (!m_registry)
		return;

	bool clip_finished = false;
	for (auto& binding : m_clip_bindings) {
		if (!binding.playing || !binding.clip)
			continue;

		binding.current_time += delta_time * binding.speed;
		if (binding.clip->duration > 0.0f) {
			if (binding.loop) {
				binding.current_time = std::fmod(binding.current_time, binding.clip->duration);
				if (binding.current_time < 0.0f)
					binding.current_time += binding.clip->duration;
			} else {
				if (binding.current_time >= binding.clip->duration) {
					binding.current_time = binding.clip->duration;
					binding.playing = false;
					clip_finished = true;
				} else if (binding.current_time < 0.0f) {
					binding.current_time = 0.0f;
					binding.playing = false;
					clip_finished = true;
				}
			}
		}

		for (const auto& channel : binding.clip->channels) {
			if (channel.target_slot >= binding.clip->target_node_indices.size())
				continue;
			uint32_t loaded_node_idx = binding.clip->target_node_indices[channel.target_slot];
			if (loaded_node_idx >= m_node_to_entity.size())
				continue;

			Entity target = m_node_to_entity[loaded_node_idx];
			auto* tc = m_registry->getComponent<TransformComponent>(target);
			if (!tc)
				continue;

			if (channel.sampler_index >= binding.clip->samplers.size())
				continue;
			const auto& sampler = binding.clip->samplers[channel.sampler_index];
			float t = binding.current_time;

			switch (channel.path) {
				case AnimationPath::Translation:
					tc->setTranslation(sampleVec3(sampler, t, channel.interpolation));
					break;
				case AnimationPath::Rotation:
					tc->setRotation(sampleQuat(sampler, t, channel.interpolation));
					break;
				case AnimationPath::Scale:
					tc->setScale(sampleVec3(sampler, t, channel.interpolation));
					break;
				case AnimationPath::Weights: {
					auto* morph = m_registry->getComponent<MorphComponent>(target);
					if (morph && morph->targetCount() > 0)
						sampleScalarArray(sampler, t, channel.interpolation,
						                  static_cast<uint32_t>(morph->targetCount()), morph->weights());
					break;
				}
			}
		}
	}

	if (clip_finished) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
}

} // namespace ve
