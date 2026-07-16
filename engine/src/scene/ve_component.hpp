/* Component system for ECS entities. Components are plain data holders that
 * can be attached to entities. Systems operate on entities with specific component combinations.
 */
#pragma once
#include "ve_export.hpp"
#include "ve_entity.hpp"
#include "resources/ve_resource_manager.hpp"
#include "resources/ve_mesh.hpp"
#include "resources/ve_material.hpp"
#include "resources/ve_animation_clip.hpp"
#include "resources/ve_texture.hpp"

#include "rendering/particle_emitter_params.hpp"

#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ve {

// Forward declarations
class Registry;

// ---------------------------------------------------------------------------
// Component type ID system
// ---------------------------------------------------------------------------

// Assigns a unique ID to each different component type.
// Each template instantiation gets its own static local, therefore each
// component type has its own unique ID.
class VENGINE_API ComponentTypeIDSystem {
public:
	template <typename T>
	static size_t getTypeID() {
		static size_t type_id = m_next_type_id++; // Each template instantiation gets its own static local
		return type_id;
	}

private:
	static size_t m_next_type_id;
};

// ---------------------------------------------------------------------------
// Component base class
// ---------------------------------------------------------------------------
class VENGINE_API Component {
public:
	virtual ~Component() = default;

	virtual void initialize() {} // unused
	virtual void update(float /*delta_time*/) {}
	virtual void render() {} // unused

	void setContext(Entity entity, Registry* registry) { m_entity = entity; m_registry = registry; }
	Entity getEntity() const { return m_entity; }
	Registry* getRegistry() const { return m_registry; }

	template <typename T>
	static size_t getTypeID() {
		return ComponentTypeIDSystem::getTypeID<T>();
	}

protected:
	Entity m_entity;
	Registry* m_registry = nullptr;

};

// ---------------------------------------------------------------------------
// TransformComponent
// ---------------------------------------------------------------------------
class VENGINE_API TransformComponent : public Component {
public:
	const glm::mat4& getTransform() const;  // local transform
	const glm::mat3& getNormalTransform() const;

	const glm::vec3& getTranslation() const { return translation; }
	const glm::quat& getRotation() const { return rotation; }
	const glm::vec3& getScale() const { return scale; }

	void setTranslation(glm::vec3 pos);
	void setRotation(glm::quat q);
	// Set rotation from Euler angles in radians (order: X, Y, Z applied as Rz*Ry*Rx)
	void setRotationEuler(glm::vec3 euler_rad);
	void setScale(glm::vec3 s);

private:
	void updateTransform() const;

	glm::vec3 translation{0.0f};
	glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // identity
	glm::vec3 scale{1.0f};

	mutable glm::mat4 m_cached_transform{1.0f};
	mutable glm::mat3 m_cached_normal_transform{1.0f};
	mutable bool m_transform_dirty{true};
};

// ---------------------------------------------------------------------------
// PointLightComponent
// ---------------------------------------------------------------------------
class VENGINE_API PointLightComponent : public Component {
public:
	float getIntensity() const { return m_intensity; }
	const glm::vec3& getColor() const { return m_color; }
	float getRange() const { return m_range; }
	bool getRotates() const { return m_rotates; }
	bool getCastsShadow() const { return m_casts_shadow; }
	bool getShowBillboard() const { return m_show_billboard; }

	void setIntensity(float v);
	void setColor(const glm::vec3& v);
	void setRange(float v);
	void setRotates(bool v);
	void setCastsShadow(bool v);
	void setShowBillboard(bool v) { m_show_billboard = v; }

	/// Returns range if explicitly set, otherwise derives from intensity/color
	/// using the KHR_lights_punctual cutoff threshold. Cached until dirty.
	float getEffectiveRange() const;

	void update(float delta_time) override;

private:
	void updateEffectiveRange() const;

	float m_intensity{1.0f};
	glm::vec3 m_color{1.0f};
	float m_range{0.0f};
	bool m_rotates{false};
	bool m_casts_shadow{false};
	bool m_show_billboard{true};

	mutable float m_effective_range{0.0f};
	mutable bool m_range_dirty{true};
};

// ---------------------------------------------------------------------------
// MeshComponent
// ---------------------------------------------------------------------------
class VENGINE_API MeshComponent : public Component {
public:
	MeshComponent(ResourceHandle<VeMesh> mesh_h, ResourceHandle<VeMaterial> material_h)
		: mesh_handle(std::move(mesh_h)), material_handle(std::move(material_h)) {}

	void render() override;

	VeMesh* getMesh() const { return mesh_handle.get(); }
	VeMaterial* getMaterial() const { return material_handle.get(); }

	// Mutates this material's PBR factors and notifies the
	// renderer to re-upload
	void editMaterialFactors(const std::function<void(MaterialFactors&)>& fn);
	ResourceHandle<VeMesh> getMeshHandle() const { return mesh_handle; }
	ResourceHandle<VeMaterial> getMaterialHandle() const { return material_handle; }
	bool hasMesh() const { return mesh_handle.isValid(); }
	bool hasMaterial() const { return material_handle.isValid(); }

	// Returns mesh local AABB transformed by owner's model matrix. Cached until transform changes.
	const VeMesh::AABB& getWorldAABB() const;
	void invalidateWorldAABB();

	float has_texture{0.0f};
	bool has_shadow{true};
	mutable uint32_t cached_lod{0};

private:
	void updateWorldAABB() const;

	ResourceHandle<VeMesh> mesh_handle;
	ResourceHandle<VeMaterial> material_handle;
	mutable VeMesh::AABB m_cached_world_aabb;
	mutable bool m_world_aabb_dirty{true};
};

// ---------------------------------------------------------------------------
// DirectionalLightComponent
// ---------------------------------------------------------------------------
enum class CelestialType : uint8_t { Moon = 0, Sun = 1 };

class VENGINE_API DirectionalLightComponent : public Component {
public:
	const glm::vec3& getDirection() const { return m_direction; }
	const glm::vec3& getColor() const { return m_color; }
	float getIntensity() const { return m_intensity; }
	bool getCastsShadow() const { return m_casts_shadow; }
	bool getShowBillboard() const { return m_show_billboard; }
	CelestialType getCelestialType() const { return m_celestial_type; }

	void setDirection(const glm::vec3& v);
	void setColor(const glm::vec3& v);
	void setIntensity(float v);
	void setCastsShadow(bool v);
	void setShowBillboard(bool v) { m_show_billboard = v; }
	void setCelestialType(CelestialType t);

private:
	glm::vec3 m_direction{0.f, -1.f, -1.f};  // world-space light direction (toward surface)
	glm::vec3 m_color{1.f};
	float m_intensity{1.f};
	bool m_casts_shadow{false};
	bool m_show_billboard{true};
	CelestialType m_celestial_type{CelestialType::Sun};
};

// ---------------------------------------------------------------------------
// SpotLightComponent
// ---------------------------------------------------------------------------
class VENGINE_API SpotLightComponent : public Component {
public:
	float getIntensity() const { return m_intensity; }
	const glm::vec3& getColor() const { return m_color; }
	float getRange() const { return m_range; }
	const glm::vec3& getDirection() const { return m_direction; }
	float getInnerConeAngle() const { return m_inner_cone_angle; }
	float getOuterConeAngle() const { return m_outer_cone_angle; }
	bool getCastsShadow() const { return m_casts_shadow; }
	bool getShowBillboard() const { return m_show_billboard; }

	void setIntensity(float v);
	void setColor(const glm::vec3& v);
	void setRange(float v);
	void setDirection(const glm::vec3& v);
	void setInnerConeAngle(float radians);
	void setOuterConeAngle(float radians);
	void setCastsShadow(bool v);
	void setShowBillboard(bool v) { m_show_billboard = v; }

	/// Returns range if explicitly set, otherwise derives from intensity
	float getEffectiveRange() const;

private:
	void updateEffectiveRange() const;

	float m_intensity{1.0f};
	glm::vec3 m_color{1.0f};
	float m_range{0.0f};// 0 means derive from intensity
	glm::vec3 m_direction{0.f, 0.f, -1.f}; // local-space
	float m_inner_cone_angle{glm::radians(25.0f)};
	float m_outer_cone_angle{glm::radians(35.0f)};
	bool m_casts_shadow{false};
	bool m_show_billboard{true};

	mutable float m_effective_range{0.0f};
	mutable bool m_range_dirty{true};
};

// ---------------------------------------------------------------------------
// AreaLightComponent (LTC rectangular area light)
// ---------------------------------------------------------------------------
// The light is a unit quad in the entity's local XZ plane
class VENGINE_API AreaLightComponent : public Component {
public:
	float getIntensity() const { return m_intensity; }
	const glm::vec3& getColor() const { return m_color; }
	bool getTwoSided() const { return m_two_sided; }
	float getRange() const { return m_range; }
	bool getShowGizmo() const { return m_show_gizmo; }

	void setIntensity(float v);
	void setColor(const glm::vec3& v);
	void setTwoSided(bool v);
	void setRange(float v);
	void setShowGizmo(bool v) { m_show_gizmo = v; }

private:
	float m_intensity{1.0f};
	glm::vec3 m_color{1.0f};
	bool m_two_sided{false};
	float m_range{0.0f}; // explicit influence radius; 0 = auto-derive from intensity/size
	bool m_show_gizmo{true};
};

struct AreaLightBasis {
	glm::vec3 right_half; // 0.5 * world X column
	glm::vec3 up_half;    // 0.5 * world Z column
};

inline AreaLightBasis areaLightWorldBasis(const glm::mat4& world) {
	return {0.5f * glm::vec3(world[0]), 0.5f * glm::vec3(world[2])};
}

VENGINE_API float areaLightInfluenceRadius(float width, float height, const glm::vec3& color,
                                           float intensity, float range);

// ---------------------------------------------------------------------------
// RigidbodyComponent
// ---------------------------------------------------------------------------
enum class PhysicsShapeType : uint8_t {
	Box,
	Sphere,
	Capsule,
	ConvexHull,
	MeshStatic // triangle mesh from VeMesh CPU data
};

enum class PhysicsMotionType : uint8_t {
	Static,
	Kinematic,
	Dynamic
};

struct PhysicsShapeDesc {
	PhysicsShapeType type = PhysicsShapeType::Box;
	glm::vec3 half_extents{0.0f};  // zero = derive from mesh AABB
};

class VENGINE_API RigidbodyComponent : public Component {
public:
	RigidbodyComponent() = default;

	RigidbodyComponent(const RigidbodyComponent& other)
		: Component(other)
		, m_motion_type(other.m_motion_type)
		, m_shape(other.m_shape)
		, m_mass(other.m_mass)
		, m_friction(other.m_friction)
		, m_restitution(other.m_restitution)
		, m_hull_tolerance(other.m_hull_tolerance)
		, m_body_id(UINT32_MAX)
		, m_dirty(false) {}

	RigidbodyComponent& operator=(const RigidbodyComponent& other) {
		if (this != &other) {
			Component::operator=(other);
			m_motion_type = other.m_motion_type;
			m_shape = other.m_shape;
			m_mass = other.m_mass;
			m_friction = other.m_friction;
			m_restitution = other.m_restitution;
			m_hull_tolerance = other.m_hull_tolerance;
		}
		return *this;
	}

	PhysicsMotionType getMotionType() const { return m_motion_type; }
	void setMotionType(PhysicsMotionType t);

	const PhysicsShapeDesc& getShapeDesc() const { return m_shape; }
	void setShapeDesc(const PhysicsShapeDesc& s);

	float getMass() const { return m_mass; }
	void setMass(float m);

	float getFriction() const { return m_friction; }
	void setFriction(float f);

	float getRestitution() const { return m_restitution; }
	void setRestitution(float r);

	float getHullTolerance() const { return m_hull_tolerance; }
	void setHullTolerance(float t);

	uint32_t getBodyId() const { return m_body_id; }
	void setBodyId(uint32_t id) { m_body_id = id; }
	bool hasBody() const { return m_body_id != UINT32_MAX; }

	bool isDirty() const { return m_dirty; }
	void clearDirty() { m_dirty = false; }

private:
	PhysicsMotionType m_motion_type = PhysicsMotionType::Static;
	PhysicsShapeDesc m_shape;
	float m_mass = 1.0f;
	float m_friction = 0.5f;
	float m_restitution = 0.3f;
	float m_hull_tolerance = 0.05f;
	uint32_t m_body_id = UINT32_MAX;
	bool m_dirty = false;
};

// ---------------------------------------------------------------------------
// CharacterControllerComponent
// ---------------------------------------------------------------------------
// Jolt CharacterVirtual-backed character. The Jolt object lives in
// PhysicsSystem; this component carries capsule/movement params, the per-frame
// desired velocity, and post-step state.
class VENGINE_API CharacterControllerComponent : public Component {
public:
	float getRadius() const { return m_radius; }
	void setRadius(float r);

	// Cylinder half-height (total height = 2*(half_height+radius))
	float getHalfHeight() const { return m_half_height; }
	void setHalfHeight(float h);

	float getMaxSlopeDeg() const { return m_max_slope_deg; }
	void setMaxSlopeDeg(float deg);

	float getStepHeight() const { return m_step_height; }
	void setStepHeight(float h);

	float getStickToFloor() const { return m_stick_to_floor; }
	void setStickToFloor(float d);

	float getMass() const { return m_mass; }
	void setMass(float m);

	float getMaxStrength() const { return m_max_strength; }
	void setMaxStrength(float s);

	// Movement params
	float walk_speed = 2.0f;
	float run_speed = 4.0f;
	float acceleration = 20.0f;
	float turn_rate_deg = 540.0f;
	float jump_speed = 7.0f; // initial upward velocity
	// Yaw offset (deg) from the engine forward (+Y) to the mesh's visual front.
	// 0 for a model authored facing +Y; 180 for one facing -Y.
	float facing_offset_deg = 0.0f;

	glm::vec3 desired_velocity{0.0f};
	bool jump_requested = false;

	// Post-step state, written by PhysicsSystem
	glm::vec3 velocity{0.0f};
	glm::vec3 ground_normal{0.0f, 0.0f, 1.0f};
	bool grounded = false;

private:
	void markChanged();

	float m_radius = 0.3f;
	float m_half_height = 0.3f;
	float m_max_slope_deg = 50.0f;
	float m_step_height = 0.4f;
	float m_stick_to_floor = 0.5f;
	float m_mass = 70.0f;
	float m_max_strength = 100.0f;
};

// ---------------------------------------------------------------------------
// AnimatorComponent
// ---------------------------------------------------------------------------
struct ClipBinding {
	std::shared_ptr<VeAnimationClip> clip;
	float current_time = 0.0f;
	float speed = 1.0f;
	bool playing = true;
	bool loop = true;
	float weight = 1.0f;
	float target_weight = 1.0f;
	float fade_rate = 0.0f;  // weight units/sec toward target_weight; 0 = snap
};

class VENGINE_API AnimatorComponent : public Component {
public:
	void update(float delta_time) override;

	uint32_t addClip(std::shared_ptr<VeAnimationClip> clip, bool auto_play = true, bool loop = true);
	void play(uint32_t clip_index);
	void pause(uint32_t clip_index);
	void stop(uint32_t clip_index);
	void setSpeed(uint32_t clip_index, float speed);
	void setLoop(uint32_t clip_index, bool loop);
	void setTime(uint32_t clip_index, float time);

	void playAll();
	void pauseAll();
	void stopAll();

	// Returns the first clip whose name matches, or -1.
	int findClip(std::string_view name) const;

	void setClipWeight(uint32_t clip_index, float weight);
	float getClipWeight(uint32_t clip_index) const;

	// Ramps the target clip's weight to 1 and all others to 0 over fade_seconds.
	// Fully faded-out clips auto-stop. Suspends an active blend space until the
	// next setBlendParameter call.
	void crossFadeTo(uint32_t clip_index, float fade_seconds);

	// 1D blend space: a scalar parameter picks the two bracketing samples and
	// weights their clips; phase_sync drives member time from one shared gait
	// phase. Synced members ignore their own loop flag (they always cycle on the
	// shared phase), but per-clip speed biases the shared cadence (duration/speed).
	// A member with speed <= 0 does not drive the phase.
	// Clips left playing that are not blend-space members keep their own weight and
	// blend into the same pose.
	struct BlendSample1D {
		float position;
		uint32_t clip_index;
	};
	void setBlendSpace1D(std::vector<BlendSample1D> samples, bool phase_sync = true);
	void setBlendParameter(float value);
	float getBlendParameter() const { return m_blend_param; }
	bool hasBlendSpace() const { return !m_blend_samples.empty(); }
	bool isBlendSpaceActive() const { return m_blend_space_active; }
	const std::vector<BlendSample1D>& getBlendSamples() const { return m_blend_samples; }

	// Playback-rate multiplier for the phase-synced gait
	void setLocomotionCadence(float rate) { m_locomotion_cadence = std::max(rate, 0.0f); }
	float getLocomotionCadence() const { return m_locomotion_cadence; }

	void setNodeToEntityMap(std::vector<Entity> map) { m_node_to_entity = std::move(map); }
	void remapEntities(const std::unordered_map<uint32_t, Entity>& old_to_new);

	bool hasPlayingClips() const;
	std::vector<Entity> getAnimatedEntities() const;

	const std::vector<ClipBinding>& getClipBindings() const { return m_clip_bindings; }
	const std::vector<Entity>& getNodeToEntityMap() const { return m_node_to_entity; }

private:
	// Weighted accumulation slot for one node
	struct BlendPose {
		glm::vec3 t{0.0f};
		glm::quat r{0.0f, 0.0f, 0.0f, 0.0f};  // unnormalized weighted sum
		glm::vec3 s{0.0f};
		std::vector<float> morph;
		float t_weight = 0.0f;
		float r_weight = 0.0f;
		float s_weight = 0.0f;
		float morph_weight = 0.0f;
		bool touched = false;
	};

	void updateAnimatedFlags();
	void applyBlendParameter();
	bool isPhaseSyncedMember(uint32_t clip_index) const;

	std::vector<ClipBinding> m_clip_bindings;
	std::vector<Entity> m_node_to_entity;

	std::vector<BlendPose> m_pose_scratch;      // sized to node count, reset in place per tick
	std::vector<uint32_t> m_touched_nodes;
	std::vector<float> m_morph_sample_scratch;
	std::vector<BlendSample1D> m_blend_samples; // sorted by position
	float m_blend_param = 0.0f;
	float m_locomotion_cadence = 1.0f;
	float m_phase = 0.0f;                       // in [0,1)
	bool m_blend_space_active = false;
	bool m_phase_sync = true;
};

// ---------------------------------------------------------------------------
// CameraComponent
// ---------------------------------------------------------------------------
class VENGINE_API CameraComponent : public Component {
public:
	enum class ProjectionType : uint8_t { Perspective, Orthographic };

	ProjectionType getProjection() const { return m_projection; }
	void setProjection(ProjectionType t) { m_projection = t; }

	float getFovY() const { return m_fov_y_radians; }
	void setFovY(float radians) { m_fov_y_radians = radians; }

	float getOrthoSize() const { return m_ortho_size; }
	void setOrthoSize(float half_height) { m_ortho_size = half_height; }

	float getNear() const { return m_near_plane; }
	void setNear(float v) { m_near_plane = v; }

	float getFar() const { return m_far_plane; }
	void setFar(float v) { m_far_plane = v; }

	bool isActive() const { return m_active; }
	void setActive(bool v) { m_active = v; }

	int getPriority() const { return m_priority; }
	void setPriority(int v) { m_priority = v; }

private:
	ProjectionType m_projection = ProjectionType::Perspective;
	float m_fov_y_radians = glm::radians(55.0f);
	float m_ortho_size = 10.0f;
	float m_near_plane = 0.1f;
	float m_far_plane = 1000.0f;
	bool  m_active = true;
	int   m_priority = 0;
};

// ---------------------------------------------------------------------------
// SkinComponent
// ---------------------------------------------------------------------------
class VENGINE_API SkinComponent : public Component {
public:
	const std::vector<Entity>& getJointEntities() const { return m_joint_entities; }
	const std::vector<glm::mat4>& getInverseBindMatrices() const { return m_inverse_bind_matrices; }
	const std::vector<VeMesh::AABB>& getJointLocalExtents() const { return m_joint_local_extents; }
	Entity getSkeletonRoot() const { return m_skeleton_root; }
	size_t jointCount() const { return m_joint_entities.size(); }

	void setJointEntities(std::vector<Entity> joints) { m_joint_entities = std::move(joints); }
	void setInverseBindMatrices(std::vector<glm::mat4> ibms) { m_inverse_bind_matrices = std::move(ibms); }
	void setJointLocalExtents(std::vector<VeMesh::AABB> extents) { m_joint_local_extents = std::move(extents); }
	void setSkeletonRoot(Entity e) { m_skeleton_root = e; }

	void remapEntities(const std::unordered_map<uint32_t, Entity>& old_to_new);

private:
	std::vector<Entity> m_joint_entities;
	std::vector<glm::mat4> m_inverse_bind_matrices;
	std::vector<VeMesh::AABB> m_joint_local_extents;  // joint-local space, IBM applied
	Entity m_skeleton_root;
};

// ---------------------------------------------------------------------------
// MorphComponent
// ---------------------------------------------------------------------------
// Per-instance morph-target weights
class VENGINE_API MorphComponent : public Component {
public:
	const std::vector<float>& getWeights() const { return m_weights; }
	std::vector<float>& weights() { return m_weights; }
	void setWeights(std::vector<float> weights) { m_weights = std::move(weights); }
	size_t targetCount() const { return m_weights.size(); }

private:
	std::vector<float> m_weights;  // one weight per mesh morph target
};

// ---------------------------------------------------------------------------
// ParticleEmitterComponent
// ---------------------------------------------------------------------------
class VENGINE_API ParticleEmitterComponent : public Component {
public:
	ParticleEmitterComponent() = default;

	ParticleEmitterComponent(const ParticleEmitterComponent& other)
		: Component(other)
		, params(other.params)
		, texture(other.texture)
		, rate(other.rate)
		, burst_count(other.burst_count)
		, burst_period(other.burst_period)
		, scale(other.scale)
		, rate_accumulator(0.0f)
		, burst_accumulator(0.0f)
		, m_active(other.m_active) {}

	ParticleEmitterComponent& operator=(const ParticleEmitterComponent& other) {
		if (this != &other) {
			Component::operator=(other);
			params = other.params;
			texture = other.texture;
			rate = other.rate;
			burst_count = other.burst_count;
			burst_period = other.burst_period;
			scale = other.scale;
			rate_accumulator = 0.0f;
			burst_accumulator = 0.0f;
			m_active = other.m_active;
		}
		return *this;
	}

	bool isActive() const { return m_active; }
	void setActive(bool v) { m_active = v; }

	EmitterParams params{}; // uploaded to gpu

	// Optional sprite/atlas texture overwrite.
	ResourceHandle<VeTexture> texture;

	// CPU-driven emission params
	float rate = 0.0f;          // particles/sec
	uint32_t burst_count = 0;   // particles per burst trigger
	float burst_period = 0.0f;  // seconds between bursts
	float scale = 1.0f;

	// System internal accumulators
	float rate_accumulator = 0.0f;
	float burst_accumulator = 0.0f;

private:
	bool m_active = true;
};

// suppress implicit instantiation
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<TransformComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<PointLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<DirectionalLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MeshComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SpotLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<AreaLightComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<RigidbodyComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<AnimatorComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<SkinComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<CameraComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<ParticleEmitterComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<MorphComponent>();
extern template VENGINE_API size_t ComponentTypeIDSystem::getTypeID<CharacterControllerComponent>();

} // namespace ve
