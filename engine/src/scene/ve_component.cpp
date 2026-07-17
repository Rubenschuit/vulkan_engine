#include "scene/ve_component.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ecs_event_dispatcher.hpp"
#include "scene/camera_math.hpp"
#include "input/input_action.hpp"
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
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<AreaLightComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<RigidbodyComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<AnimatorComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SkinComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<CameraComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<ParticleEmitterComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MorphComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<CharacterControllerComponent>();
template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<FollowCameraComponent>();


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
// AreaLightComponent
// ---------------------------------------------------------------------------

void AreaLightComponent::setIntensity(float v) {
	m_intensity = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void AreaLightComponent::setColor(const glm::vec3& v) {
	m_color = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void AreaLightComponent::setTwoSided(bool v) {
	m_two_sided = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

void AreaLightComponent::setRange(float v) {
	m_range = v;
	if (m_registry)
		m_registry->events().emit(LightDataChangedEvent{m_entity});
}

float areaLightInfluenceRadius(float width, float height, const glm::vec3& color,
                               float intensity, float range) {
	float half_diag = 0.5f * std::sqrt(width * width + height * height);
	if (range > 0.0f)
		return std::min(range + half_diag, CLUSTER_MAX_EFFECTIVE_RANGE);
	// Distance where the rect's irradiance (radiance * area / (2*pi*d^2)) falls below the
	// cutoff.
	float max_radiance = std::max({color.r, color.g, color.b}) * std::max(intensity, 0.0f);
	float area = std::max(width * height, 1e-4f);
	float reach = std::sqrt(max_radiance * area / (glm::two_pi<float>() * AREA_LIGHT_CLUSTER_CUTOFF));
	return std::min(half_diag + reach, CLUSTER_MAX_EFFECTIVE_RANGE);
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
// CharacterControllerComponent
// ---------------------------------------------------------------------------

void CharacterControllerComponent::markChanged() {
	if (m_registry)
		m_registry->events().emit(CharacterControllerChangedEvent{m_entity});
}

void CharacterControllerComponent::setRadius(float r) {
	m_radius = r;
	markChanged();
}

void CharacterControllerComponent::setHalfHeight(float h) {
	m_half_height = h;
	markChanged();
}

void CharacterControllerComponent::setMaxSlopeDeg(float deg) {
	m_max_slope_deg = deg;
	markChanged();
}

void CharacterControllerComponent::setStepHeight(float h) {
	m_step_height = h;
	markChanged();
}

void CharacterControllerComponent::setStickToFloor(float d) {
	m_stick_to_floor = d;
	markChanged();
}

void CharacterControllerComponent::setMass(float m) {
	m_mass = m;
	markChanged();
}

void CharacterControllerComponent::setMaxStrength(float s) {
	m_max_strength = s;
	markChanged();
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

// Below this a clip contributes nothing to the blended pose.
static constexpr float BLEND_WEIGHT_EPSILON = 1e-4f;

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

int AnimatorComponent::findClip(std::string_view name) const {
	for (size_t i = 0; i < m_clip_bindings.size(); i++) {
		const auto& b = m_clip_bindings[i];
		if (b.clip && b.clip->name == name)
			return static_cast<int>(i);
	}
	return -1;
}

void AnimatorComponent::setClipWeight(uint32_t clip_index, float weight) {
	if (clip_index >= m_clip_bindings.size())
		return;
	auto& b = m_clip_bindings[clip_index];
	b.weight = b.target_weight = std::max(weight, 0.0f);
	b.fade_rate = 0.0f;
}

float AnimatorComponent::getClipWeight(uint32_t clip_index) const {
	return clip_index < m_clip_bindings.size() ? m_clip_bindings[clip_index].weight : 0.0f;
}

void AnimatorComponent::crossFadeTo(uint32_t clip_index, float fade_seconds) {
	if (clip_index >= m_clip_bindings.size() || !m_clip_bindings[clip_index].clip)
		return;

	// Manual fades own the weights until the next setBlendParameter.
	m_blend_space_active = false;

	bool changed = false;
	const bool snap = fade_seconds <= 0.0f;
	for (size_t i = 0; i < m_clip_bindings.size(); i++) {
		auto& b = m_clip_bindings[i];
		if (!b.clip)
			continue;
		b.target_weight = (i == clip_index) ? 1.0f : 0.0f;
		if (snap) {
			b.weight = b.target_weight;
			b.fade_rate = 0.0f;
			if (b.weight == 0.0f && b.playing) {
				b.playing = false;
				changed = true;
			}
		} else {
			b.fade_rate = 1.0f / fade_seconds;
		}
	}

	auto& target = m_clip_bindings[clip_index];
	if (!target.playing) {
		target.playing = true;
		// Bring an inactive clip in from the start
		target.current_time = target.speed < 0.0f ? target.clip->duration : 0.0f;
		if (!snap)
			target.weight = 0.0f;
		changed = true;
	}

	if (changed && m_registry) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
}

void AnimatorComponent::fadeClipWeight(uint32_t clip_index, float target, float fade_seconds) {
	if (clip_index >= m_clip_bindings.size() || !m_clip_bindings[clip_index].clip)
		return;
	auto& b = m_clip_bindings[clip_index];
	b.target_weight = glm::clamp(target, 0.0f, 1.0f);
	if (fade_seconds <= 0.0f) {
		b.weight = b.target_weight;
		b.fade_rate = 0.0f;
		if (b.weight == 0.0f && b.playing) {
			b.playing = false;
			if (m_registry) {
				updateAnimatedFlags();
				m_registry->events().emit(AnimationStateChangedEvent{m_entity});
			}
		}
		return;
	}
	b.fade_rate = 1.0f / fade_seconds;
}

bool AnimatorComponent::isPhaseSyncedMember(uint32_t clip_index) const {
	for (const auto& s : m_blend_samples)
		if (s.clip_index == clip_index)
			return true;
	return false;
}

void AnimatorComponent::setBlendSpace1D(std::vector<BlendSample1D> samples, bool phase_sync) {
	std::erase_if(samples, [this](const BlendSample1D& s) {
		return s.clip_index >= m_clip_bindings.size() || !m_clip_bindings[s.clip_index].clip;
	});
	std::sort(samples.begin(), samples.end(),
	          [](const BlendSample1D& a, const BlendSample1D& b) { return a.position < b.position; });
	m_blend_samples = std::move(samples);
	m_phase_sync = phase_sync;
	m_blend_space_active = !m_blend_samples.empty();
	if (!m_blend_space_active)
		return;

	// Seed the shared phase from the dominant member so enabling sync doesn't snap gaits.
	if (m_phase_sync) {
		float best_weight = -1.0f;
		m_phase = 0.0f;
		for (const auto& s : m_blend_samples) {
			const auto& b = m_clip_bindings[s.clip_index];
			if (b.playing && b.weight > best_weight && b.clip->duration > 0.0f) {
				best_weight = b.weight;
				m_phase = b.current_time / b.clip->duration;
				m_phase -= std::floor(m_phase);
			}
		}
	}

	applyBlendParameter();
}

void AnimatorComponent::setBlendParameter(float value) {
	m_blend_param = value;
	if (!hasBlendSpace())
		return;
	m_blend_space_active = true;  // resumes after a crossFadeTo
	applyBlendParameter();
}

void AnimatorComponent::applyBlendParameter() {
	if (m_blend_samples.empty())
		return;

	const float v = std::clamp(m_blend_param,
	                           m_blend_samples.front().position, m_blend_samples.back().position);
	size_t upper = m_blend_samples.size() - 1;
	for (size_t i = 0; i < m_blend_samples.size(); i++) {
		if (m_blend_samples[i].position >= v) {
			upper = i;
			break;
		}
	}
	const size_t lower = upper > 0 ? upper - 1 : 0;
	const float p0 = m_blend_samples[lower].position;
	const float p1 = m_blend_samples[upper].position;
	const float alpha = (p1 > p0) ? (v - p0) / (p1 - p0) : 0.0f;

	bool changed = false;
	for (size_t i = 0; i < m_blend_samples.size(); i++) {
		auto& b = m_clip_bindings[m_blend_samples[i].clip_index];
		float w = 0.0f;
		if (i == lower)
			w += 1.0f - alpha;
		if (i == upper)
			w += alpha;
		b.weight = b.target_weight = w;
		b.fade_rate = 0.0f;
		if (w > 0.0f && !b.playing) {
			b.playing = true;
			changed = true;
		}
	}
	// Zero-weight members are auto-stopped by update().

	if (changed && m_registry) {
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

void FollowCameraComponent::remapEntities(const std::unordered_map<uint32_t, Entity>& old_to_new) {
	if (!target.isNull()) {
		auto it = old_to_new.find(target.index());
		if (it != old_to_new.end())
			target = it->second;
	}
}

glm::vec2 FollowCameraComponent::pitchLimitsRad() const {
	const float lim = glm::radians(ORBIT_PITCH_LIMIT_DEG);
	float lo = glm::clamp(glm::radians(min_pitch_deg), -lim, lim);
	float hi = glm::clamp(glm::radians(max_pitch_deg), -lim, lim);
	return {lo, std::max(lo, hi)};
}

void FollowCameraComponent::writePose(const glm::vec3& pivot) {
	const glm::vec3 fwd = forwardFromYawPitch(yaw, pitch);
	const glm::vec3 right = glm::normalize(glm::cross(fwd, WORLD_UP));
	const glm::vec3 up = glm::cross(right, fwd);
	m_registry->setWorldPose(m_entity, pivot - fwd * current_distance,
	                         glm::quat_cast(glm::mat3(right, fwd, up)));
}

bool FollowCameraComponent::tick(const CameraSweepFn& sweep, const InputActions& actions, float dt) {
	if (!m_registry || target.isNull() || !m_registry->isAlive(target))
		return false;
	if (!m_registry->hasComponent<TransformComponent>(m_entity))
		return false;

	float yaw_delta = actions.look_yaw * look_speed * dt;
	float pitch_delta = actions.look_pitch * look_speed * dt;
	if (actions.mouse_look_enabled) {
		yaw_delta += glm::radians(actions.mouse_dx * mouse_sensitivity);
		pitch_delta += glm::radians(actions.mouse_dy * mouse_sensitivity);
	}
	yaw += yaw_delta;
	pitch += pitch_delta;
	wrapYaw(yaw);
	const glm::vec2 limits = pitchLimitsRad();
	clampPitchRange(pitch, limits.x, limits.y);

	const glm::vec3 pivot = glm::vec3(m_registry->getWorldTransform(target)[3]) + WORLD_UP * pivot_height;
	const glm::vec3 fwd = forwardFromYawPitch(yaw, pitch);

	float target_len = std::max(distance, min_distance);
	if (sweep) {
		if (auto hit = sweep(pivot, pivot - fwd * target_len, collision_radius))
			target_len = glm::clamp(*hit, min_distance, target_len);
	}
	// Pull in instantly, pull out gradually.
	if (target_len < current_distance)
		current_distance = target_len;
	else
		current_distance = std::min(target_len, current_distance + pull_out_speed * dt);

	writePose(pivot);
	return true;
}

void FollowCameraComponent::alignBehind() {
	if (!m_registry || target.isNull() || !m_registry->isAlive(target))
		return;
	if (!m_registry->hasComponent<TransformComponent>(m_entity))
		return;

	glm::vec3 heading = glm::vec3(m_registry->getWorldTransform(target)[1]);
	if (const auto* cc = m_registry->getComponent<CharacterControllerComponent>(target)) {
		float off = glm::radians(cc->facing_offset_deg);
		heading = glm::vec3(glm::rotate(glm::mat4(1.0f), -off, WORLD_UP) * glm::vec4(heading, 0.0f));
	}
	yaw = yawPitchFromForward(heading, yaw).x;
	const glm::vec2 limits = pitchLimitsRad();
	clampPitchRange(pitch, limits.x, limits.y);
	current_distance = distance;

	const glm::vec3 pivot = glm::vec3(m_registry->getWorldTransform(target)[3]) + WORLD_UP * pivot_height;
	writePose(pivot);
}

void AnimatorComponent::update(float delta_time) {
	if (!m_registry)
		return;

	bool state_changed = false;

	// Step fade weights; a clip faded out to a zero target stops.
	for (auto& b : m_clip_bindings) {
		if (!b.clip)
			continue;
		if (b.fade_rate > 0.0f && b.weight != b.target_weight) {
			float step = b.fade_rate * delta_time;
			if (b.weight < b.target_weight)
				b.weight = std::min(b.weight + step, b.target_weight);
			else
				b.weight = std::max(b.weight - step, b.target_weight);
			if (b.weight == b.target_weight)
				b.fade_rate = 0.0f;
		}
		if (b.playing && b.target_weight <= 0.0f && b.weight <= BLEND_WEIGHT_EPSILON) {
			b.weight = 0.0f;
			b.playing = false;
			state_changed = true;
		}
	}

	// Phase-synced blend space: one shared normalized phase drives member time so
	// gaits stay aligned. Cadence is the weighted mean clip rate scaled by the global
	// multiplier; per-clip speed biases a member's effective duration (duration/speed)
	// so it leans the shared phase faster/slower without breaking sync.
	if (m_blend_space_active && m_phase_sync) {
		float duration_sum = 0.0f;
		float weight_sum = 0.0f;
		for (const auto& s : m_blend_samples) {
			const auto& b = m_clip_bindings[s.clip_index];
			if (b.clip && b.playing && b.weight > BLEND_WEIGHT_EPSILON && b.speed > 1e-3f) {
				duration_sum += b.weight * b.clip->duration / b.speed;
				weight_sum += b.weight;
			}
		}
		float phase_rate = (weight_sum > 0.0f && duration_sum > 0.0f)
			? m_locomotion_cadence * weight_sum / duration_sum
			: 0.0f;
		m_phase += delta_time * phase_rate;
		m_phase -= std::floor(m_phase);
		for (const auto& s : m_blend_samples) {
			auto& b = m_clip_bindings[s.clip_index];
			if (b.clip && b.playing)
				b.current_time = m_phase * b.clip->duration;
		}
	}

	if (m_pose_scratch.size() < m_node_to_entity.size())
		m_pose_scratch.resize(m_node_to_entity.size());
	m_touched_nodes.clear();

	auto touch = [this](uint32_t node_idx) -> BlendPose& {
		BlendPose& pose = m_pose_scratch[node_idx];
		if (!pose.touched) {
			pose.t = glm::vec3(0.0f);
			pose.r = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
			pose.s = glm::vec3(0.0f);
			pose.t_weight = pose.r_weight = pose.s_weight = pose.morph_weight = 0.0f;
			pose.touched = true;
			m_touched_nodes.push_back(node_idx);
		}
		return pose;
	};

	bool clip_finished = false;
	for (uint32_t ci = 0; ci < static_cast<uint32_t>(m_clip_bindings.size()); ci++) {
		auto& binding = m_clip_bindings[ci];
		if (!binding.playing || !binding.clip)
			continue;

		const bool phase_synced = m_blend_space_active && m_phase_sync && isPhaseSyncedMember(ci);
		if (!phase_synced) {
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
		}

		const float w = binding.weight;
		if (w <= BLEND_WEIGHT_EPSILON)
			continue;

		for (const auto& channel : binding.clip->channels) {
			if (channel.target_slot >= binding.clip->target_node_indices.size())
				continue;
			uint32_t loaded_node_idx = binding.clip->target_node_indices[channel.target_slot];
			if (loaded_node_idx >= m_node_to_entity.size())
				continue;
			if (channel.sampler_index >= binding.clip->samplers.size())
				continue;
			const auto& sampler = binding.clip->samplers[channel.sampler_index];
			float t = binding.current_time;

			switch (channel.path) {
				case AnimationPath::Translation: {
					BlendPose& pose = touch(loaded_node_idx);
					pose.t += w * sampleVec3(sampler, t, channel.interpolation);
					pose.t_weight += w;
					break;
				}
				case AnimationPath::Rotation: {
					BlendPose& pose = touch(loaded_node_idx);
					glm::quat q = sampleQuat(sampler, t, channel.interpolation);
					// Short-arc: align against the accumulated sum before adding.
					if (pose.r_weight > 0.0f && glm::dot(pose.r, q) < 0.0f)
						q = -q;
					pose.r += q * w;
					pose.r_weight += w;
					break;
				}
				case AnimationPath::Scale: {
					BlendPose& pose = touch(loaded_node_idx);
					pose.s += w * sampleVec3(sampler, t, channel.interpolation);
					pose.s_weight += w;
					break;
				}
				case AnimationPath::Weights: {
					Entity target = m_node_to_entity[loaded_node_idx];
					auto* morph = m_registry->getComponent<MorphComponent>(target);
					if (!morph || morph->targetCount() == 0)
						break;
					const uint32_t n = static_cast<uint32_t>(morph->targetCount());
					sampleScalarArray(sampler, t, channel.interpolation, n, m_morph_sample_scratch);
					BlendPose& pose = touch(loaded_node_idx);
					if (pose.morph_weight == 0.0f)
						pose.morph.assign(n, 0.0f);
					else if (pose.morph.size() < n)
						pose.morph.resize(n, 0.0f);
					for (uint32_t c = 0; c < n; c++)
						pose.morph[c] += w * m_morph_sample_scratch[c];
					pose.morph_weight += w;
					break;
				}
			}
		}
	}

	// Write each touched node once, normalized by its accumulated weight; nodes
	// a clip doesn't animate keep that clip's share out of the normalization.
	for (uint32_t node_idx : m_touched_nodes) {
		BlendPose& pose = m_pose_scratch[node_idx];
		Entity target = m_node_to_entity[node_idx];
		if (auto* tc = m_registry->getComponent<TransformComponent>(target)) {
			if (pose.t_weight > BLEND_WEIGHT_EPSILON)
				tc->setTranslation(pose.t / pose.t_weight);
			if (pose.r_weight > BLEND_WEIGHT_EPSILON) {
				float len2 = glm::dot(pose.r, pose.r);
				// Opposite quats can cancel; keep the previous rotation then.
				if (len2 > 1e-8f)
					tc->setRotation(pose.r / std::sqrt(len2));
			}
			if (pose.s_weight > BLEND_WEIGHT_EPSILON)
				tc->setScale(pose.s / pose.s_weight);
		}
		if (pose.morph_weight > BLEND_WEIGHT_EPSILON) {
			if (auto* morph = m_registry->getComponent<MorphComponent>(target)) {
				auto& out = morph->weights();
				const size_t n = std::min(out.size(), pose.morph.size());
				for (size_t c = 0; c < n; c++)
					out[c] = pose.morph[c] / pose.morph_weight;
			}
		}
		pose.touched = false;
	}

	if (clip_finished || state_changed) {
		updateAnimatedFlags();
		m_registry->events().emit(AnimationStateChangedEvent{m_entity});
	}
}

} // namespace ve
