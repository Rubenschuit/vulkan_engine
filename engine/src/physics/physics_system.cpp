#include "pch.hpp"
#include "physics/physics_system.hpp"
#include <cstdarg>
#include <set>
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "scene/ecs_event_dispatcher.hpp"
#include "events/event_bus.hpp"
#include "events/engine_events.hpp"
#include "resources/ve_mesh.hpp"

// Jolt headers (only included in this translation unit)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Core/IssueReporting.h>

JPH_SUPPRESS_WARNINGS

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailed(const char* inExpression, const char* inMessage, const char* inFile, JPH::uint inLine) {
	VE_LOGE("Jolt assert failed: " << inFile << ":" << inLine << ": (" << inExpression << ") "
		<< (inMessage ? inMessage : ""));
	return true;
}
#endif

static void JoltTrace(const char* inFmt, ...) {
	va_list args;
	va_start(args, inFmt);
	char buf[1024];
	vsnprintf(buf, sizeof(buf), inFmt, args);
	va_end(args);
	VE_LOGI("Jolt: " << buf);
}

namespace ve {

// ── Jolt layer configuration ────────────────────────────────────────────────

namespace Layers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

namespace BPLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr uint32_t NUM_LAYERS = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
	BPLayerInterfaceImpl() {
		m_object_to_bp[Layers::NON_MOVING] = BPLayers::NON_MOVING;
		m_object_to_bp[Layers::MOVING] = BPLayers::MOVING;
	}

	JPH::uint GetNumBroadPhaseLayers() const override {
		return BPLayers::NUM_LAYERS;
	}

	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
		return m_object_to_bp[layer];
	}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
		switch (static_cast<JPH::BroadPhaseLayer::Type>(layer)) {
			case static_cast<JPH::BroadPhaseLayer::Type>(BPLayers::NON_MOVING): return "NON_MOVING";
			case static_cast<JPH::BroadPhaseLayer::Type>(BPLayers::MOVING): return "MOVING";
			default: return "INVALID";
		}
	}
#endif

private:
	JPH::BroadPhaseLayer m_object_to_bp[Layers::NUM_LAYERS];
};

class ObjectVsBPLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
		if (layer1 == Layers::NON_MOVING)
			return layer2 == BPLayers::MOVING;
		return true;
	}
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer obj1, JPH::ObjectLayer obj2) const override {
		if (obj1 == Layers::NON_MOVING)
			return obj2 == Layers::MOVING;
		return true;
	}
};

class ContactListenerImpl : public JPH::ContactListener {
public:
	EventBus* event_bus = nullptr;

	JPH::ValidateResult OnContactValidate(
		[[maybe_unused]] const JPH::Body& body1,
		[[maybe_unused]] const JPH::Body& body2,
		[[maybe_unused]] JPH::RVec3Arg base_offset,
		[[maybe_unused]] const JPH::CollideShapeResult& result) override {
		return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	void OnContactAdded(
		const JPH::Body& body1,
		const JPH::Body& body2,
		[[maybe_unused]] const JPH::ContactManifold& manifold,
		[[maybe_unused]] JPH::ContactSettings& settings) override {
		if (!event_bus)
			return;
		Entity a = Entity::fromRaw(static_cast<uint32_t>(body1.GetUserData()));
		Entity b = Entity::fromRaw(static_cast<uint32_t>(body2.GetUserData()));
		auto cp = manifold.GetWorldSpaceContactPointOn1(0);
		auto cn = manifold.mWorldSpaceNormal;
		event_bus->enqueue(CollisionEvent{
			a, b,
			{cp.GetX(), cp.GetY(), cp.GetZ()},
			{cn.GetX(), cn.GetY(), cn.GetZ()},
			manifold.mPenetrationDepth});
	}

	void OnContactRemoved(
		[[maybe_unused]] const JPH::SubShapeIDPair& sub_shape_pair) override {
		// CollisionEndEvent requires entity mapping from BodyID, which is not
		// available in this callback (only SubShapeIDs). Deferred to a future
		// iteration when a BodyID->Entity reverse map is maintained.
	}
};

struct BodyData {
	JPH::BodyID body_id;
	JPH::RefConst<JPH::Shape> shape_ref;
	glm::vec3 last_scale{1.0f};
	glm::vec3 last_synced_world_pos{0.0f};
	glm::quat last_synced_world_rot{1.0f, 0.0f, 0.0f, 0.0f};
	std::vector<uint32_t> compound_children;
	std::vector<glm::vec3> child_last_scales;
	bool pushed_this_frame = false;
	bool frozen = false;
	bool preserve_velocity = false;
	JPH::EMotionType pre_freeze_motion_type = JPH::EMotionType::Dynamic;
};

// ── implementation ────────────────────────────────────────────────────
//
// All Jolt types live here so that Jolt headers stay out of the public header.
// PhysicsSystem public methods delegate to this struct.

static int s_jolt_ref_count = 0;

struct PhysicsSystem::Impl {
	PhysicsConfig config;

	std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
	std::unique_ptr<JPH::JobSystemThreadPool> job_system;
	std::unique_ptr<JPH::PhysicsSystem> physics_system;

	BPLayerInterfaceImpl bp_layer_interface;
	ObjectVsBPLayerFilterImpl obj_vs_bp_filter;
	ObjectLayerPairFilterImpl obj_pair_filter;
	ContactListenerImpl contact_listener;

	// Entity index -> Jolt body mapping
	std::vector<std::optional<BodyData>> body_map;
	std::vector<uint32_t> active_body_indices; // dense list for iteration

	std::vector<Entity> scratch_entities; // reusable buffer for collectDescendantMeshes

	SubscriptionId sub_rb_added = 0;
	SubscriptionId sub_rb_removed = 0;
	SubscriptionId sub_rb_changed = 0;
	Registry* active_registry = nullptr;

	EventBus* event_bus = nullptr;

	// Event-driven dirty tracking (replaces per-frame scan in rebuildDirtyBodies)
	std::vector<uint32_t> m_dirty_rb_indices;

	float accumulator = 0.0f;
	uint32_t dynamic_body_count = 0; // non-static bodies (dynamic + kinematic)

	// ── Body map helpers ───────────────────────────────────────────────────

	void ensureBodyMapSize(uint32_t idx) {
		if (idx >= body_map.size())
			body_map.resize(idx + 1);
	}

	bool hasBody(uint32_t idx) const {
		return idx < body_map.size() && body_map[idx].has_value();
	}

	BodyData& getBody(uint32_t idx) {
		return *body_map[idx];
	}

	void insertBody(uint32_t idx, BodyData data) {
		ensureBodyMapSize(idx);
		body_map[idx] = std::move(data);
		active_body_indices.push_back(idx);
	}

	void eraseBody(uint32_t idx) {
		body_map[idx].reset();
		auto it = std::find(active_body_indices.begin(), active_body_indices.end(), idx);
		if (it != active_body_indices.end()) {
			*it = active_body_indices.back();
			active_body_indices.pop_back();
		}
	}

	void clearAllBodies() {
		body_map.clear();
		active_body_indices.clear();
		dynamic_body_count = 0;
	}

	// ── Lifecycle ───────────────────────────────────────────────────────────

	explicit Impl(const PhysicsConfig& cfg) : config(cfg) {
		if (s_jolt_ref_count++ == 0) {
			JPH::RegisterDefaultAllocator();
			JPH::Trace = JoltTrace;
			JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
			JPH::Factory::sInstance = new JPH::Factory();
			JPH::RegisterTypes();
		}

		temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

		auto thread_count = std::max(1u, std::thread::hardware_concurrency() - 1);
		job_system = std::make_unique<JPH::JobSystemThreadPool>(
			JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(thread_count));

		physics_system = std::make_unique<JPH::PhysicsSystem>();
		physics_system->Init(
			cfg.max_bodies, 0, cfg.max_body_pairs, cfg.max_contact_constraints,
			bp_layer_interface, obj_vs_bp_filter, obj_pair_filter);
		physics_system->SetGravity(JPH::Vec3(cfg.gravity.x, cfg.gravity.y, cfg.gravity.z));
		physics_system->SetContactListener(&contact_listener);
	}

	~Impl() {
		removeAllBodies();

		physics_system.reset();
		job_system.reset();
		temp_allocator.reset();

		if (--s_jolt_ref_count == 0) {
			JPH::UnregisterTypes();
			delete JPH::Factory::sInstance;
			JPH::Factory::sInstance = nullptr;
		}
	}

	// ── Scene binding ───────────────────────────────────────────────────────

	void onSceneLoaded(Registry& registry) {
		active_registry = &registry;
		accumulator = 0.0f;

		sub_rb_added = registry.events().subscribe<ComponentAddedEvent<RigidbodyComponent>>(
			[this](const ComponentAddedEvent<RigidbodyComponent>& event) {
				onRigidbodyAdded(event.entity, event.component);
			});

		sub_rb_removed = registry.events().subscribe<ComponentRemovedEvent<RigidbodyComponent>>(
			[this](const ComponentRemovedEvent<RigidbodyComponent>& event) {
				onRigidbodyRemoved(event.entity);
			});

		sub_rb_changed = registry.events().subscribe<RigidbodyChangedEvent>(
			[this](const RigidbodyChangedEvent& event) {
				uint32_t idx = event.entity.index();
				if (hasBody(idx))
					m_dirty_rb_indices.push_back(idx);
			});

		// Copy entity indices before iterating: onRigidbodyAdded calls
		// removeDescendantRigidbodies which does swap-and-pop on the pool
		auto& rb_pool = registry.rigidbodies();
		std::vector<uint32_t> existing_rb_indices;
		existing_rb_indices.reserve(rb_pool.size());
		for (uint32_t i = 0; i < rb_pool.size(); ++i)
			existing_rb_indices.push_back(rb_pool.entityAt(i));

		for (uint32_t idx : existing_rb_indices) {
			Entity e = registry.entityFromIndex(idx);
			if (!registry.isAlive(e))
				continue;
			auto* rb = registry.getComponent<RigidbodyComponent>(e);
			if (rb && !hasBody(idx))
				onRigidbodyAdded(e, *rb);
		}
	}

	void onSceneUnloaded() {
		if (active_registry) {
			active_registry->events().unsubscribe<ComponentAddedEvent<RigidbodyComponent>>(sub_rb_added);
			active_registry->events().unsubscribe<ComponentRemovedEvent<RigidbodyComponent>>(sub_rb_removed);
			active_registry->events().unsubscribe<RigidbodyChangedEvent>(sub_rb_changed);
		}
		removeAllBodies();
		m_dirty_rb_indices.clear();
		active_registry = nullptr;
		accumulator = 0.0f;
	}

	// ── Body creation / removal ─────────────────────────────────────────────

	void onRigidbodyAdded(Entity entity, RigidbodyComponent& rb) {
		if (!active_registry)
			return;

		auto* tc = active_registry->getComponent<TransformComponent>(entity);
		if (!tc)
			return;

		// Only remove descendant rigidbodies when this entity will create a compound
		// shape (no mesh of its own). Entities with their own mesh only cover
		// themselves, so descendants keep their own colliders.
		auto* mc = active_registry->getComponent<MeshComponent>(entity);
		if (!mc || !mc->hasMesh())
			removeDescendantRigidbodies(entity);

		JPH::ShapeRefC shape = createShape(entity, rb, *tc);
		if (!shape)
			return;

		glm::vec3 pos;
		glm::quat rot;
		getWorldPosRot(entity, pos, rot);

		auto body_settings = createBodySettings(rb, shape, pos, rot);
		auto& body_interface = physics_system->GetBodyInterface();
		JPH::Body* body = body_interface.CreateBody(body_settings);
		if (!body) {
			VE_LOGW("PhysicsSystem: failed to create body for entity " << entity.index());
			return;
		}
		body->SetUserData(static_cast<uint64_t>(entity.id()));

		JPH::EActivation activation = (rb.getMotionType() == PhysicsMotionType::Static)
			? JPH::EActivation::DontActivate
			: JPH::EActivation::Activate;
		body_interface.AddBody(body->GetID(), activation);

		rb.setBodyId(body->GetID().GetIndexAndSequenceNumber());
		rb.clearDirty();
		insertBody(entity.index(), {body->GetID(), shape, tc->getScale(), pos, rot, {}, {}});
		snapshotCompoundChildren(entity, getBody(entity.index()));

		if (rb.getMotionType() != PhysicsMotionType::Static)
			dynamic_body_count++;
	}

	void onRigidbodyRemoved(Entity entity) {
		uint32_t idx = entity.index();
		if (!hasBody(idx))
			return;

		auto& body_interface = physics_system->GetBodyInterface();
		auto& data = getBody(idx);

		// Wake bodies that were resting on this one before removing it
		JPH::TransformedShape ts = body_interface.GetTransformedShape(data.body_id);
		JPH::AABox bounds = ts.GetWorldSpaceBounds();
		bounds.ExpandBy(JPH::Vec3::sReplicate(0.5f));

		if (body_interface.GetMotionType(data.body_id) != JPH::EMotionType::Static)
			dynamic_body_count--;

		body_interface.RemoveBody(data.body_id);
		body_interface.DestroyBody(data.body_id);
		eraseBody(idx);

		body_interface.ActivateBodiesInAABox(bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
	}

	void removeAllBodies() {
		if (!physics_system)
			return;
		auto& body_interface = physics_system->GetBodyInterface();
		for (uint32_t idx : active_body_indices) {
			auto& data = getBody(idx);
			body_interface.RemoveBody(data.body_id);
			body_interface.DestroyBody(data.body_id);
		}
		clearAllBodies();
	}

	// ── World transform helpers ─────────────────────────────────────────────

	static glm::quat quatFromMatrix(const glm::mat4& m) {
		glm::mat3 rot_mat(
			glm::normalize(glm::vec3(m[0])),
			glm::normalize(glm::vec3(m[1])),
			glm::normalize(glm::vec3(m[2])));
		return glm::normalize(glm::quat_cast(rot_mat));
	}

	void getWorldPosRot(Entity entity, glm::vec3& pos, glm::quat& rot) {
		const glm::mat4& world = active_registry->getWorldTransform(entity);
		pos = glm::vec3(world[3]);
		rot = quatFromMatrix(world);
	}

	glm::vec3 getWorldScale(Entity entity) {
		const glm::mat4& world = active_registry->getWorldTransform(entity);
		return {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))};
	}

	void worldToLocal(Entity entity, const glm::vec3& world_pos, const glm::quat& world_rot,
	                   glm::vec3& local_pos, glm::quat& local_rot) {
		Entity parent = active_registry->getParent(entity);
		if (parent.isNull()) {
			local_pos = world_pos;
			local_rot = world_rot;
			return;
		}
		const glm::mat4& parent_world = active_registry->getWorldTransform(parent);
		glm::mat4 inv_parent = glm::inverse(parent_world);
		local_pos = glm::vec3(inv_parent * glm::vec4(world_pos, 1.0f));
		local_rot = glm::normalize(glm::inverse(quatFromMatrix(parent_world)) * world_rot);
	}

	// ── Shape creation ──────────────────────────────────────────────────────

	// Wrap a shape with a translation if the mesh AABB center is not at the origin
	JPH::ShapeRefC applyMeshCenterOffset(Entity entity, glm::vec3 scale, JPH::ShapeRefC shape) {
		if (!shape || !active_registry)
			return shape;
		auto* mc = active_registry->getComponent<MeshComponent>(entity);
		if (!mc || !mc->hasMesh())
			return shape;

		VeMesh::AABB aabb = mc->getMesh()->getLocalAABB();
		glm::vec3 center = (aabb.min + aabb.max) * 0.5f * scale;
		if (glm::length(center) < 0.001f)
			return shape;

		auto result = JPH::RotatedTranslatedShapeSettings(
			JPH::Vec3(center.x, center.y, center.z), JPH::Quat::sIdentity(), shape.GetPtr()).Create();
		return result.HasError() ? shape : result.Get();
	}

	// Result from capsule shape creation: the shape itself and an axis rotation
	// quaternion (identity when height axis is Y, otherwise a rotation to align).
	// The caller decides whether to bake the rotation into an RT wrapper (single mesh)
	// or fold it into the compound sub-shape transform.
	struct CapsuleResult {
		JPH::ShapeRefC shape;
		JPH::Quat axis_rotation = JPH::Quat::sIdentity();
	};

	std::optional<CapsuleResult> createCapsuleShape(const glm::vec3& he) {
		int height_axis = 1;
		if (he.x >= he.y && he.x >= he.z)
			height_axis = 0;
		else if (he.z >= he.x && he.z >= he.y)
			height_axis = 2;

		float radius, half_extent;
		if (height_axis == 0) {
			radius = std::min(he.y, he.z);
			half_extent = he.x;
		} else if (height_axis == 1) {
			radius = std::min(he.x, he.z);
			half_extent = he.y;
		} else {
			radius = std::min(he.x, he.y);
			half_extent = he.z;
		}
		float half_height = std::max(half_extent - radius, 0.0f);

		if (half_height < radius * 0.1f) {
			auto result = JPH::SphereShapeSettings(half_extent).Create();
			if (result.HasError())
				return std::nullopt;
			return CapsuleResult{result.Get(), JPH::Quat::sIdentity()};
		}

		auto result = JPH::CapsuleShapeSettings(half_height, radius).Create();
		if (result.HasError())
			return std::nullopt;

		JPH::Quat axis_rot = JPH::Quat::sIdentity();
		if (height_axis == 2)
			axis_rot = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI * 0.5f);
		else if (height_axis == 0)
			axis_rot = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -JPH::JPH_PI * 0.5f);
		return CapsuleResult{result.Get(), axis_rot};
	}

	// Create a shape from a mesh without applying the AABB center offset
	JPH::ShapeRefC createShapeForMeshRaw(Entity mesh_entity, const PhysicsShapeDesc& desc,
		PhysicsMotionType motion_type, float hull_tolerance = 0.05f) {

		if (!active_registry)
			return nullptr;

		auto* mc = active_registry->getComponent<MeshComponent>(mesh_entity);
		if (!mc || !mc->hasMesh())
			return nullptr;

		glm::vec3 scale = getWorldScale(mesh_entity);

		VeMesh::AABB aabb = mc->getMesh()->getLocalAABB();
		glm::vec3 mesh_he = glm::max((aabb.max - aabb.min) * 0.5f, glm::vec3(0.01f));

		// MeshStatic is only valid for static bodies, derive a box from the AABB
		if (desc.type == PhysicsShapeType::MeshStatic && motion_type != PhysicsMotionType::Static) {
			glm::vec3 he = glm::max(mesh_he * scale, glm::vec3(0.01f));
			auto result = JPH::BoxShapeSettings(JPH::Vec3(he.x, he.y, he.z)).Create();
			if (!result.HasError())
				return result.Get();

			VE_LOGW("PhysicsSystem: MeshStatic not supported for dynamic bodies, falling back to unit box");
			result = JPH::BoxShapeSettings(JPH::Vec3(0.5f, 0.5f, 0.5f)).Create();
			return result.HasError() ? nullptr : result.Get();
		}

		// For all primitive shapes, derive dimensions from the mesh AABB
		glm::vec3 he = glm::max(mesh_he * scale, glm::vec3(0.01f));
		float max_he = std::max({he.x, he.y, he.z});

		JPH::ShapeRefC shape;
		switch (desc.type) {
			case PhysicsShapeType::Box: {
				auto result = JPH::BoxShapeSettings(JPH::Vec3(he.x, he.y, he.z)).Create();
				if (result.HasError()) {
					VE_LOGW("PhysicsSystem: box shape error: " << result.GetError().c_str());
					return nullptr;
				}
				shape = result.Get();
				break;
			}
			case PhysicsShapeType::Sphere: {
				auto result = JPH::SphereShapeSettings(max_he).Create();
				if (result.HasError())
					return nullptr;
				shape = result.Get();
				break;
			}
			case PhysicsShapeType::Capsule: {
				auto capsule = createCapsuleShape(he);
				if (!capsule)
					return nullptr;
				// Bake axis rotation into an RT wrapper for standalone shapes
				if (capsule->axis_rotation != JPH::Quat::sIdentity()) {
					auto rt_result = JPH::RotatedTranslatedShapeSettings(
						JPH::Vec3::sZero(), capsule->axis_rotation, capsule->shape.GetPtr()).Create();
					if (rt_result.HasError())
						return nullptr;
					shape = rt_result.Get();
				} else {
					shape = capsule->shape;
				}
				break;
			}
			case PhysicsShapeType::ConvexHull:
				return createConvexHullShape(mesh_entity, scale, hull_tolerance);
			case PhysicsShapeType::MeshStatic:
				return createMeshShape(mesh_entity, scale);
		}
		return shape;
	}

	JPH::ShapeRefC createShapeForMesh(Entity mesh_entity, const PhysicsShapeDesc& desc,
		PhysicsMotionType motion_type, float hull_tolerance = 0.05f) {
		JPH::ShapeRefC shape = createShapeForMeshRaw(mesh_entity, desc, motion_type, hull_tolerance);
		if (!shape)
			return nullptr;
		// Hulls and triangle meshes already encode position in their vertices
		if (desc.type == PhysicsShapeType::ConvexHull || desc.type == PhysicsShapeType::MeshStatic)
			return shape;
		glm::vec3 scale = getWorldScale(mesh_entity);
		return applyMeshCenterOffset(mesh_entity, scale, shape);
	}

	// Recursively collect descendant entities that have a MeshComponent
	void collectDescendantMeshes(Entity entity, std::vector<Entity>& out) {
		Entity child = active_registry->firstChild(entity);
		while (!child.isNull()) {
			auto* mc = active_registry->getComponent<MeshComponent>(child);
			if (mc && mc->hasMesh())
				out.push_back(child);
			collectDescendantMeshes(child, out);
			child = active_registry->nextSibling(child);
		}
	}

	void removeDescendantRigidbodies(Entity rb_entity) {
		if (!active_registry)
			return;
		scratch_entities.clear();
		collectDescendantMeshes(rb_entity, scratch_entities);
		for (Entity child : scratch_entities) {
			if (active_registry->hasComponent<RigidbodyComponent>(child))
				active_registry->removeComponent<RigidbodyComponent>(child);
		}
	}

	// Record descendant mesh entities and their scales for compound change detection
	void snapshotCompoundChildren(Entity rb_entity, BodyData& data) {
		if (!active_registry)
			return;
		auto* mc = active_registry->getComponent<MeshComponent>(rb_entity);
		if (mc && mc->hasMesh())
			return; // Not a compound body

		data.compound_children.clear();
		data.child_last_scales.clear();
		scratch_entities.clear();
		collectDescendantMeshes(rb_entity, scratch_entities);
		data.compound_children.reserve(scratch_entities.size());
		data.child_last_scales.reserve(scratch_entities.size());
		for (Entity child : scratch_entities) {
			data.compound_children.push_back(child.index());
			data.child_last_scales.push_back(getWorldScale(child));
		}
	}

	// Compute a child's position and rotation relative to the rb entity in world space
	void getRelativePosRot(Entity rb_entity, Entity child,
		glm::vec3& rel_pos, glm::quat& rel_rot) {

		glm::vec3 rb_pos, child_pos;
		glm::quat rb_rot, child_rot;
		getWorldPosRot(rb_entity, rb_pos, rb_rot);
		getWorldPosRot(child, child_pos, child_rot);

		glm::quat inv_rb_rot = glm::inverse(rb_rot);
		rel_pos = inv_rb_rot * (child_pos - rb_pos);
		rel_rot = glm::normalize(inv_rb_rot * child_rot);
	}

	// Create a compound shape from descendant meshes of an entity without its own mesh.
	JPH::ShapeRefC createCompoundShape(Entity rb_entity, const RigidbodyComponent& rb) {
		scratch_entities.clear();
		collectDescendantMeshes(rb_entity, scratch_entities);
		// Copy to local since scratch_entities may be reused by nested calls
		std::vector<Entity> mesh_entities(scratch_entities.begin(), scratch_entities.end());
		if (mesh_entities.empty())
			return nullptr;

		const auto& desc = rb.getShapeDesc();
		bool is_dynamic = rb.getMotionType() != PhysicsMotionType::Static;

		// Single child
		if (mesh_entities.size() == 1) {
			Entity child = mesh_entities[0];
			JPH::ShapeRefC sub = createShapeForMeshRaw(child, desc, rb.getMotionType(), rb.getHullTolerance());
			if (!sub)
				return nullptr;

			auto* mc = active_registry->getComponent<MeshComponent>(child);
			glm::vec3 scale = getWorldScale(child);
			VeMesh::AABB aabb = mc->getMesh()->getLocalAABB();
			glm::vec3 mesh_center = (aabb.min + aabb.max) * 0.5f * scale;

			glm::vec3 rel_pos;
			glm::quat rel_rot;
			getRelativePosRot(rb_entity, child, rel_pos, rel_rot);
			glm::vec3 final_pos = rel_pos + rel_rot * mesh_center;

			if (glm::length(final_pos) < 0.001f && std::abs(glm::dot(rel_rot, glm::quat(1, 0, 0, 0))) > 1.0f - 1e-6f)
				return sub;

			auto result = JPH::RotatedTranslatedShapeSettings(
				JPH::Vec3(final_pos.x, final_pos.y, final_pos.z),
				JPH::Quat(rel_rot.x, rel_rot.y, rel_rot.z, rel_rot.w),
				sub.GetPtr()).Create();
			return result.HasError() ? nullptr : result.Get();
		}

		// Multiple children: build a StaticCompoundShape with sub-shapes
		JPH::StaticCompoundShapeSettings compound_settings;
		for (Entity child : mesh_entities) {
			auto* mc = active_registry->getComponent<MeshComponent>(child);
			if (!mc || !mc->hasMesh())
				continue;

			glm::vec3 scale = getWorldScale(child);
			VeMesh::AABB aabb = mc->getMesh()->getLocalAABB();
			glm::vec3 mesh_he = glm::max((aabb.max - aabb.min) * 0.5f * scale, glm::vec3(0.01f));
			glm::vec3 mesh_center = (aabb.min + aabb.max) * 0.5f * scale;

			JPH::ShapeRefC sub;
			JPH::Quat shape_rot = JPH::Quat::sIdentity();

			switch (desc.type) {
				case PhysicsShapeType::Box: {
					auto r = JPH::BoxShapeSettings(JPH::Vec3(mesh_he.x, mesh_he.y, mesh_he.z)).Create();
					if (!r.HasError())
						sub = r.Get();
					break;
				}
				case PhysicsShapeType::Sphere: {
					float max_he = std::max({mesh_he.x, mesh_he.y, mesh_he.z});
					auto r = JPH::SphereShapeSettings(max_he).Create();
					if (!r.HasError())
						sub = r.Get();
					break;
				}
				case PhysicsShapeType::Capsule: {
					auto capsule = createCapsuleShape(mesh_he);
					if (capsule) {
						sub = capsule->shape;
						shape_rot = capsule->axis_rotation;
					}
					break;
				}
				case PhysicsShapeType::ConvexHull:
					sub = createConvexHullShape(child, scale, rb.getHullTolerance());
					mesh_center = glm::vec3(0.0f); // Hull vertices already include position
					break;
				case PhysicsShapeType::MeshStatic:
					if (is_dynamic) {
						auto r = JPH::BoxShapeSettings(JPH::Vec3(mesh_he.x, mesh_he.y, mesh_he.z)).Create();
						if (!r.HasError())
							sub = r.Get();
					} else {
						sub = createMeshShape(child, scale);
						mesh_center = glm::vec3(0.0f); // Mesh shape includes position
					}
					break;
			}
			if (!sub)
				continue;

			// Compute the compound sub-shape position: entity-relative position
			// plus the mesh AABB center offset in the child's rotated frame
			glm::vec3 rel_pos;
			glm::quat rel_rot;
			getRelativePosRot(rb_entity, child, rel_pos, rel_rot);
			glm::vec3 final_pos = rel_pos + rel_rot * mesh_center;

			// Combine child entity rotation with shape-intrinsic rotation (capsule axis)
			JPH::Quat final_rot = JPH::Quat(rel_rot.x, rel_rot.y, rel_rot.z, rel_rot.w) * shape_rot;

			compound_settings.AddShape(
				JPH::Vec3(final_pos.x, final_pos.y, final_pos.z),
				final_rot,
				sub.GetPtr());
		}

		if (compound_settings.mSubShapes.empty())
			return nullptr;

		auto result = compound_settings.Create();
		if (result.HasError()) {
			VE_LOGW("PhysicsSystem: compound shape error: " << result.GetError().c_str());
			return nullptr;
		}
		return result.Get();
	}

	JPH::ShapeRefC createShape(Entity entity, const RigidbodyComponent& rb, const TransformComponent& /*tc*/) {
		auto* mc = active_registry->getComponent<MeshComponent>(entity);
		if (mc && mc->hasMesh())
			return createShapeForMesh(entity, rb.getShapeDesc(), rb.getMotionType(), rb.getHullTolerance());

		// No mesh on this entity so we build compound from descendant meshes
		return createCompoundShape(entity, rb);
	}

	JPH::ShapeRefC createConvexHullShape(Entity entity, glm::vec3 scale, float hull_tolerance = 0.05f) {
		if (!active_registry)
			return nullptr;

		auto* mc = active_registry->getComponent<MeshComponent>(entity);
		if (!mc || !mc->hasMesh())
			return nullptr;

		const VeMesh* mesh = mc->getMesh();
		if (!mesh->hasCpuGeometry())
			return nullptr;

		const auto& positions = mesh->getCpuPositions();
		if (positions.empty())
			return nullptr;

		JPH::Array<JPH::Vec3> hull_points;
		hull_points.reserve(positions.size());
		for (const auto& p : positions)
			hull_points.push_back(JPH::Vec3(p.x * scale.x, p.y * scale.y, p.z * scale.z));

		JPH::ConvexHullShapeSettings settings(std::move(hull_points));
		settings.mHullTolerance = hull_tolerance;
		auto result = settings.Create();
		if (result.HasError()) {
			VE_LOGW("PhysicsSystem: convex hull error for entity " << entity.index() << ": " << result.GetError().c_str());
			return nullptr;
		}

		// Degenerate hulls have zero inner radius and cannot be used
		// with LinearCast motion quality. Fall back to a box from the AABB.
		JPH::ShapeRefC hull = result.Get();
		if (hull->GetInnerRadius() < 1e-4f) {
			VeMesh::AABB aabb = mesh->getLocalAABB();
			glm::vec3 he = glm::max((aabb.max - aabb.min) * 0.5f * scale, glm::vec3(0.01f));
			auto box_result = JPH::BoxShapeSettings(JPH::Vec3(he.x, he.y, he.z)).Create();
			if (!box_result.HasError())
				return box_result.Get();
		}
		return hull;
	}

	JPH::ShapeRefC createMeshShape(Entity entity, glm::vec3 scale) {
		if (!active_registry)
			return nullptr;

		auto* mc = active_registry->getComponent<MeshComponent>(entity);
		if (!mc || !mc->hasMesh())
			return nullptr;

		const VeMesh* mesh = mc->getMesh();
		if (!mesh->hasCpuGeometry())
			return nullptr;

		const auto& positions = mesh->getCpuPositions();
		const auto& indices = mesh->getCpuIndices();
		if (positions.empty() || indices.empty())
			return nullptr;

		// Bake entity scale into vertex positions
		JPH::VertexList jolt_vertices;
		jolt_vertices.reserve(positions.size());
		for (const auto& p : positions)
			jolt_vertices.push_back(JPH::Float3(p.x * scale.x, p.y * scale.y, p.z * scale.z));

		JPH::IndexedTriangleList jolt_triangles;
		jolt_triangles.reserve(indices.size() / 3);
		for (size_t i = 0; i + 2 < indices.size(); i += 3)
			jolt_triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));

		JPH::MeshShapeSettings settings(std::move(jolt_vertices), std::move(jolt_triangles));
		auto result = settings.Create();
		if (result.HasError()) {
			VE_LOGW("PhysicsSystem: mesh shape error for entity " << entity.index() << ": " << result.GetError().c_str());
			return nullptr;
		}
		return result.Get();
	}

	// ── Body settings helper ────────────────────────────────────────────────

	JPH::BodyCreationSettings createBodySettings(
		const RigidbodyComponent& rb, JPH::ShapeRefC shape, glm::vec3 pos, glm::quat rot) {

		JPH::EMotionType motion;
		JPH::ObjectLayer layer;
		switch (rb.getMotionType()) {
			case PhysicsMotionType::Static:
				motion = JPH::EMotionType::Static;
				layer = Layers::NON_MOVING;
				break;
			case PhysicsMotionType::Kinematic:
				motion = JPH::EMotionType::Kinematic;
				layer = Layers::MOVING;
				break;
			case PhysicsMotionType::Dynamic:
				motion = JPH::EMotionType::Dynamic;
				layer = Layers::MOVING;
				break;
			default:
				motion = JPH::EMotionType::Static;
				layer = Layers::NON_MOVING;
				break;
		}

		JPH::BodyCreationSettings settings(
			shape,
			JPH::RVec3(pos.x, pos.y, pos.z),
			JPH::Quat(rot.x, rot.y, rot.z, rot.w),
			motion, layer);
		settings.mFriction = rb.getFriction();
		settings.mRestitution = rb.getRestitution();
		if (rb.getMotionType() == PhysicsMotionType::Dynamic) {
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = rb.getMass();
			settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
		}
		return settings;
	}

	// ── Body rebuild helpers ────────────────────────────────────────────────

	void destroyBody(uint32_t entity_idx, BodyData& data, RigidbodyComponent& rb) {
		auto& body_interface = physics_system->GetBodyInterface();
		body_interface.RemoveBody(data.body_id);
		body_interface.DestroyBody(data.body_id);
		rb.setBodyId(UINT32_MAX);
	}

	bool recreateBody(Entity entity, BodyData& data, RigidbodyComponent& rb, TransformComponent& tc) {
		auto& body_interface = physics_system->GetBodyInterface();

		// Capture velocity before destroying old body
		JPH::Vec3 linear_vel = JPH::Vec3::sZero();
		JPH::Vec3 angular_vel = JPH::Vec3::sZero();
		bool was_dynamic = body_interface.GetMotionType(data.body_id) == JPH::EMotionType::Dynamic;
		if (was_dynamic) {
			linear_vel = body_interface.GetLinearVelocity(data.body_id);
			angular_vel = body_interface.GetAngularVelocity(data.body_id);
		}

		// Destroy old body
		body_interface.RemoveBody(data.body_id);
		body_interface.DestroyBody(data.body_id);
		rb.setBodyId(UINT32_MAX);

		removeDescendantRigidbodies(entity);

		// Create new shape
		JPH::ShapeRefC shape = createShape(entity, rb, tc);
		if (!shape)
			return false;

		glm::vec3 pos;
		glm::quat rot;
		getWorldPosRot(entity, pos, rot);

		auto body_settings = createBodySettings(rb, shape, pos, rot);
		JPH::Body* body = body_interface.CreateBody(body_settings);
		if (!body)
			return false;
		body->SetUserData(static_cast<uint64_t>(entity.id()));

		JPH::EActivation activation = (rb.getMotionType() == PhysicsMotionType::Static)
			? JPH::EActivation::DontActivate
			: JPH::EActivation::Activate;
		body_interface.AddBody(body->GetID(), activation);

		// Restore velocity if still dynamic
		if (was_dynamic && rb.getMotionType() == PhysicsMotionType::Dynamic) {
			body_interface.SetLinearVelocity(body->GetID(), linear_vel);
			body_interface.SetAngularVelocity(body->GetID(), angular_vel);
		}

		auto* tc_ptr = active_registry->getComponent<TransformComponent>(entity);
		data.body_id = body->GetID();
		data.shape_ref = shape;
		data.last_scale = tc_ptr ? tc_ptr->getScale() : glm::vec3(1.0f);
		data.last_synced_world_pos = pos;
		data.last_synced_world_rot = rot;
		rb.setBodyId(body->GetID().GetIndexAndSequenceNumber());
		snapshotCompoundChildren(entity, data);

		// Re-freeze the new body if the old one was frozen
		if (data.frozen) {
			data.frozen = false;
			freezeBody(entity);
		}

		return true;
	}

	// ── Update ────────────────────────────────────────────────────────────

	void update(float dt, Registry& registry) {
		if (active_body_indices.empty())
			return;

		rebuildDirtyBodies(registry);

		// Early return when no dynamic/kinematic bodies exist
		if (dynamic_body_count == 0) {
			accumulator = 0.0f;
			return;
		}

		checkScaleChanges(registry);
		pushEntityChanges(registry);

		// Fixed timestep accumulator
		accumulator += dt;
		if (accumulator > static_cast<float>(config.max_substeps) * config.fixed_timestep)
			accumulator = static_cast<float>(config.max_substeps) * config.fixed_timestep;
		int steps = static_cast<int>(accumulator / config.fixed_timestep);
		if (steps <= 0)
			return;
		accumulator -= static_cast<float>(steps) * config.fixed_timestep;

		// Skip Jolt simulation if no bodies are awake
		if (physics_system->GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0)
			return;

		float total_time = static_cast<float>(steps) * config.fixed_timestep;
		physics_system->Update(total_time, steps, temp_allocator.get(), job_system.get());

		pullJoltResults(registry);
	}

	void rebuildDirtyBodies(Registry& registry) {
		if (m_dirty_rb_indices.empty())
			return;

		// Swap-and-process: prevents re-entry when recreateBody emits RigidbodyChangedEvent
		std::vector<uint32_t> to_process;
		to_process.swap(m_dirty_rb_indices);

		// Deduplicate
		std::sort(to_process.begin(), to_process.end());
		to_process.erase(std::unique(to_process.begin(), to_process.end()), to_process.end());

		for (uint32_t idx : to_process) {
			if (!hasBody(idx))
				continue;
			Entity entity = registry.entityFromIndex(idx);
			if (!registry.isAlive(entity))
				continue;
			auto* rb = registry.getComponent<RigidbodyComponent>(entity);
			auto* tc = registry.getComponent<TransformComponent>(entity);
			if (!rb || !tc)
				continue;

			// Track motion type changes for the dynamic body counter
			auto& bi = physics_system->GetBodyInterface();
			bool was_dynamic = bi.GetMotionType(getBody(idx).body_id) != JPH::EMotionType::Static;
			bool is_dynamic = rb->getMotionType() != PhysicsMotionType::Static;

			rb->clearDirty();
			if (!recreateBody(entity, getBody(idx), *rb, *tc)) {
				VE_LOGW("PhysicsSystem: failed to recreate body for entity " << idx);
				if (was_dynamic)
					dynamic_body_count--;
				eraseBody(idx);
			} else if (was_dynamic != is_dynamic) {
				dynamic_body_count += is_dynamic ? 1 : -1;
			}
		}
	}

	static bool scaleChanged(const glm::vec3& a, const glm::vec3& b) {
		constexpr float EPSILON = 1e-4f;
		glm::vec3 d = a - b;
		return std::abs(d.x) > EPSILON || std::abs(d.y) > EPSILON || std::abs(d.z) > EPSILON;
	}

	void checkScaleChanges(Registry& registry) {
		std::vector<uint32_t> changed;
		for (uint32_t idx : active_body_indices) {
			auto& data = getBody(idx);
			Entity entity = registry.entityFromIndex(idx);
			if (!registry.isAlive(entity))
				continue;
			auto* rb = registry.getComponent<RigidbodyComponent>(entity);
			if (!rb)
				continue;

			auto* tc = registry.getComponent<TransformComponent>(entity);
			if (tc && scaleChanged(tc->getScale(), data.last_scale)) {
				changed.push_back(idx);
				continue;
			}

			// Check compound children scales
			for (size_t i = 0; i < data.compound_children.size(); ++i) {
				Entity child = registry.entityFromIndex(data.compound_children[i]);
				if (!registry.isAlive(child))
					continue;
				if (scaleChanged(getWorldScale(child), data.child_last_scales[i])) {
					changed.push_back(idx);
					break;
				}
			}
		}
		for (uint32_t idx : changed) {
			if (!hasBody(idx))
				continue;
			Entity entity = registry.entityFromIndex(idx);
			auto* rb = registry.getComponent<RigidbodyComponent>(entity);
			auto* tc = registry.getComponent<TransformComponent>(entity);
			if (!rb || !tc)
				continue;
			if (!recreateBody(entity, getBody(idx), *rb, *tc))
				eraseBody(idx);
		}
	}

	// Push entity transform changes to Jolt (runs before physics step)
	void pushEntityChanges(Registry& registry) {
		auto& body_interface = physics_system->GetBodyInterface();

		constexpr float POS_EPSILON_SQ = 1e-2f * 1e-2f;
		constexpr float ROT_DOT_THRESHOLD = 1.0f - 1e-4f;

		for (uint32_t idx : active_body_indices) {
			auto& data = getBody(idx);
			data.pushed_this_frame = false;

			Entity entity = registry.entityFromIndex(idx);
			if (!registry.isAlive(entity))
				continue;

			glm::vec3 world_pos;
			glm::quat world_rot;
			getWorldPosRot(entity, world_pos, world_rot);

			bool pos_changed = glm::dot(world_pos - data.last_synced_world_pos, world_pos - data.last_synced_world_pos) > POS_EPSILON_SQ;
			bool rot_changed = std::abs(glm::dot(world_rot, data.last_synced_world_rot)) < ROT_DOT_THRESHOLD;
			if (!pos_changed && !rot_changed)
				continue;

			bool is_static = body_interface.GetMotionType(data.body_id) == JPH::EMotionType::Static;

			JPH::AABox old_bounds;
			if (is_static) {
				JPH::TransformedShape ts = body_interface.GetTransformedShape(data.body_id);
				old_bounds = ts.GetWorldSpaceBounds();
				old_bounds.ExpandBy(JPH::Vec3::sReplicate(0.5f));
			}

			JPH::EActivation activation = is_static
				? JPH::EActivation::DontActivate
				: JPH::EActivation::Activate;
			JPH::Quat set_rot(world_rot.x, world_rot.y, world_rot.z, world_rot.w);
			body_interface.SetPositionAndRotation(
				data.body_id,
				JPH::RVec3(world_pos.x, world_pos.y, world_pos.z),
				set_rot, activation);
			if (!is_static && !data.preserve_velocity) {
				body_interface.SetLinearVelocity(data.body_id, JPH::Vec3::sZero());
				body_interface.SetAngularVelocity(data.body_id, JPH::Vec3::sZero());
			} else if (is_static) {
				body_interface.ActivateBodiesInAABox(old_bounds, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter());
			}
			data.last_synced_world_pos = world_pos;
			data.last_synced_world_rot = world_rot;
			data.pushed_this_frame = true;
		}
	}

	// Pull Jolt simulation results back to entity transforms (runs after physics step)
	void pullJoltResults(Registry& registry) {
		auto& body_interface = physics_system->GetBodyInterface();

		for (uint32_t idx : active_body_indices) {
			auto& data = getBody(idx);
			if (data.pushed_this_frame)
				continue;

			Entity entity = registry.entityFromIndex(idx);
			if (!registry.isAlive(entity))
				continue;

			if (body_interface.GetMotionType(data.body_id) == JPH::EMotionType::Static)
				continue;

			auto* tc = registry.getComponent<TransformComponent>(entity);
			if (!tc)
				continue;

			JPH::RMat44 jtf = body_interface.GetWorldTransform(data.body_id);
			JPH::RVec3 jpos = jtf.GetTranslation();
			JPH::Quat jrot = jtf.GetRotation().GetQuaternion();

			glm::vec3 jolt_world_pos{
				static_cast<float>(jpos.GetX()),
				static_cast<float>(jpos.GetY()),
				static_cast<float>(jpos.GetZ())};
			glm::quat jolt_world_rot{jrot.GetW(), jrot.GetX(), jrot.GetY(), jrot.GetZ()};

			glm::vec3 new_local_pos;
			glm::quat new_local_rot;
			worldToLocal(entity, jolt_world_pos, jolt_world_rot, new_local_pos, new_local_rot);
			tc->setTranslation(new_local_pos);
			tc->setRotation(new_local_rot);
			data.last_synced_world_pos = jolt_world_pos;
			data.last_synced_world_rot = jolt_world_rot;
		}
	}

	void freezeBody(Entity entity) {
		uint32_t idx = entity.index();
		if (!hasBody(idx) || getBody(idx).frozen)
			return;

		auto& body_interface = physics_system->GetBodyInterface();
		auto& data = getBody(idx);
		data.pre_freeze_motion_type = body_interface.GetMotionType(data.body_id);
		data.frozen = true;

		if (data.pre_freeze_motion_type != JPH::EMotionType::Static) {
			body_interface.SetMotionType(data.body_id, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
			body_interface.SetLinearVelocity(data.body_id, JPH::Vec3::sZero());
			body_interface.SetAngularVelocity(data.body_id, JPH::Vec3::sZero());
		}
	}

	void unfreezeBody(Entity entity) {
		uint32_t idx = entity.index();
		if (!hasBody(idx) || !getBody(idx).frozen)
			return;

		auto& body_interface = physics_system->GetBodyInterface();
		auto& data = getBody(idx);
		data.frozen = false;

		if (data.pre_freeze_motion_type != JPH::EMotionType::Static) {
			body_interface.SetMotionType(data.body_id, data.pre_freeze_motion_type, JPH::EActivation::Activate);
			body_interface.SetLinearVelocity(data.body_id, JPH::Vec3::sZero());
			body_interface.SetAngularVelocity(data.body_id, JPH::Vec3::sZero());
		}
	}

	void setPreserveVelocity(Entity entity, bool preserve) {
		uint32_t idx = entity.index();
		if (!hasBody(idx))
			return;
		getBody(idx).preserve_velocity = preserve;
	}

	// ── Bulk operations ─────────────────────────────────────────────────────

	// Check if any ancestor has a compound rigidbody (RB without its own mesh).
	// Such children are already represented as sub-shapes of the ancestor's compound.
	// Ancestors with BOTH an RB and a mesh only cover their own mesh,
	// so children still need their own colliders.
	static bool hasAncestorCompoundRigidbody(Entity e, Registry& registry) {
		Entity parent = registry.getParent(e);
		while (!parent.isNull()) {
			if (registry.hasComponent<RigidbodyComponent>(parent)
				&& !registry.hasComponent<MeshComponent>(parent))
				return true;
			parent = registry.getParent(parent);
		}
		return false;
	}

	void addStaticCollidersForAllMeshes(Registry& registry) {
		registry.events().beginBatch();
		std::vector<Entity> entities_to_init;

		for (auto& mc : registry.meshes()) {
			Entity e = mc.getEntity();
			if (registry.hasComponent<RigidbodyComponent>(e))
				continue;
			if (hasAncestorCompoundRigidbody(e, registry))
				continue;
			if (!mc.hasMesh() || !mc.getMesh()->hasCpuGeometry())
				continue;
			if (mc.getMesh()->getCpuPositions().empty() || mc.getMesh()->getCpuIndices().empty())
				continue;

			auto& rb = registry.addComponent<RigidbodyComponent>(e);
			rb.setMotionType(PhysicsMotionType::Static);
			rb.setShapeDesc({.type = PhysicsShapeType::MeshStatic});
			entities_to_init.push_back(e);
		}
		registry.events().endBatch();

		// Events were suppressed during batch, so create bodies manually
		for (Entity e : entities_to_init) {
			auto* rb = registry.getComponent<RigidbodyComponent>(e);
			if (rb && !rb->hasBody())
				onRigidbodyAdded(e, *rb);
		}

		physics_system->OptimizeBroadPhase();
		VE_LOGI("PhysicsSystem: created " << entities_to_init.size() << " static colliders");
	}
};

// ── Public interface delegates to Impl struct ────────────────────────────────

PhysicsSystem::PhysicsSystem(const PhysicsConfig& config)
	: m_impl(std::make_unique<Impl>(config)) {}

PhysicsSystem::~PhysicsSystem() = default;

void PhysicsSystem::setEventBus(EventBus* bus) {
	m_impl->event_bus = bus;
	m_impl->contact_listener.event_bus = bus;

	if (bus) {
		bus->subscribe<SceneLoadedEvent>([this](const SceneLoadedEvent& e) {
			m_impl->onSceneLoaded(*e.registry);
			m_impl->addStaticCollidersForAllMeshes(*e.registry);
		});
		bus->subscribe<SceneUnloadedEvent>([this](const SceneUnloadedEvent&) {
			m_impl->onSceneUnloaded();
		});
		bus->subscribe<AssetLoadCompleteEvent>([this](const AssetLoadCompleteEvent&) {
			if (m_impl->active_registry)
				m_impl->addStaticCollidersForAllMeshes(*m_impl->active_registry);
		});
	}
}

void PhysicsSystem::onSceneLoaded(Registry& registry) {
	m_impl->onSceneLoaded(registry);
}

void PhysicsSystem::onSceneUnloaded() {
	m_impl->onSceneUnloaded();
}

void PhysicsSystem::update(float dt, Registry& registry) {
	m_impl->update(dt, registry);
}

void PhysicsSystem::freezeBody(Entity entity) {
	m_impl->freezeBody(entity);
}

void PhysicsSystem::unfreezeBody(Entity entity) {
	m_impl->unfreezeBody(entity);
}

void PhysicsSystem::setPreserveVelocity(Entity entity, bool preserve) {
	m_impl->setPreserveVelocity(entity, preserve);
}

void PhysicsSystem::addStaticCollidersForAllMeshes(Registry& registry) {
	m_impl->addStaticCollidersForAllMeshes(registry);
}

uint32_t PhysicsSystem::getActiveBodyCount() const {
	return m_impl->physics_system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

// Convert a Jolt shape into a DebugShape at a given world position/rotation.
// Unwraps decorated shapes and handles primitives + compounds recursively.
static bool fillDebugShape(const JPH::Shape* raw, DebugShape& ds) {
	// Unwrap decorated shapes (RotatedTranslated and Scaled wrappers)
	JPH::Quat shape_rot = JPH::Quat::sIdentity();
	JPH::Vec3 shape_offset = JPH::Vec3::sZero();
	JPH::Vec3 shape_scale = JPH::Vec3::sReplicate(1.0f);

	while (raw->GetType() == JPH::EShapeType::Decorated) {
		if (raw->GetSubType() == JPH::EShapeSubType::RotatedTranslated) {
			auto* rt = static_cast<const JPH::RotatedTranslatedShape*>(raw);
			shape_offset += shape_rot * rt->GetPosition();
			shape_rot = shape_rot * rt->GetRotation();
		} else if (raw->GetSubType() == JPH::EShapeSubType::Scaled) {
			auto* sc = static_cast<const JPH::ScaledShape*>(raw);
			shape_scale = shape_scale * sc->GetScale();
		}
		raw = static_cast<const JPH::DecoratedShape*>(raw)->GetInnerShape();
	}

	// Apply accumulated offset in the parent's frame
	JPH::Quat parent_rot(ds.rotation.x, ds.rotation.y, ds.rotation.z, ds.rotation.w);
	JPH::Vec3 world_offset = parent_rot * shape_offset;
	ds.position += glm::vec3(world_offset.GetX(), world_offset.GetY(), world_offset.GetZ());
	ds.offset = glm::vec3(0.0f);
	glm::quat shape_rotation(shape_rot.GetW(), shape_rot.GetX(), shape_rot.GetY(), shape_rot.GetZ());
	ds.rotation = ds.rotation * shape_rotation;

	glm::vec3 scale(shape_scale.GetX(), shape_scale.GetY(), shape_scale.GetZ());

	// Handle compound shapes
	if (raw->GetType() == JPH::EShapeType::Compound) {
		auto* compound = static_cast<const JPH::CompoundShape*>(raw);
		ds.type = DebugShapeType::Compound;

		// Jolt stores compound sub-shape positions relative to the compound's COM,
		// but ds.position is the shape origin (from GetWorldTransform). Add the
		// compound COM to convert sub-shape positions back to shape-origin space.
		JPH::Vec3 compound_com = compound->GetCenterOfMass();

		for (JPH::uint i = 0; i < compound->GetNumSubShapes(); i++) {
			const auto& sub = compound->GetSubShape(i);
			const JPH::Shape* sub_shape = sub.mShape;
			JPH::Quat sub_rot = sub.GetRotation();
			JPH::Vec3 sub_pos = sub.GetPositionCOM() - sub_rot * sub_shape->GetCenterOfMass()
				+ compound_com;

			DebugShape child_ds{};
			glm::vec3 local_offset(sub_pos.GetX(), sub_pos.GetY(), sub_pos.GetZ());
			child_ds.position = ds.position + ds.rotation * (local_offset * scale);
			child_ds.rotation = ds.rotation * glm::quat(sub_rot.GetW(), sub_rot.GetX(), sub_rot.GetY(), sub_rot.GetZ());
			child_ds.is_dynamic = ds.is_dynamic;

			if (fillDebugShape(sub_shape, child_ds))
				ds.sub_shapes.push_back(std::move(child_ds));
		}
		return !ds.sub_shapes.empty();
	}

	switch (raw->GetSubType()) {
		case JPH::EShapeSubType::Box: {
			auto* box = static_cast<const JPH::BoxShape*>(raw);
			JPH::Vec3 he = box->GetHalfExtent();
			ds.type = DebugShapeType::Box;
			ds.extents = glm::vec3(he.GetX(), he.GetY(), he.GetZ()) * scale;
			return true;
		}
		case JPH::EShapeSubType::Sphere: {
			auto* sphere = static_cast<const JPH::SphereShape*>(raw);
			ds.type = DebugShapeType::Sphere;
			ds.extents.x = sphere->GetRadius() * std::max({scale.x, scale.y, scale.z});
			return true;
		}
		case JPH::EShapeSubType::Capsule: {
			auto* capsule = static_cast<const JPH::CapsuleShape*>(raw);
			ds.type = DebugShapeType::Capsule;
			float radial_scale = std::max(scale.x, scale.z);
			ds.extents.x = capsule->GetRadius() * radial_scale;
			ds.extents.y = capsule->GetHalfHeightOfCylinder() * scale.y;
			return true;
		}
		case JPH::EShapeSubType::ConvexHull: {
			auto* hull = static_cast<const JPH::ConvexHullShape*>(raw);
			ds.type = DebugShapeType::ConvexHull;

			JPH::Vec3 hull_com = hull->GetCenterOfMass();
			uint32_t num_points = hull->GetNumPoints();
			ds.hull_vertices.resize(num_points);
			for (uint32_t i = 0; i < num_points; i++) {
				JPH::Vec3 p = hull->GetPoint(i) + hull_com;
				ds.hull_vertices[i] = glm::vec3(p.GetX(), p.GetY(), p.GetZ()) * scale;
			}

			std::set<std::pair<uint32_t, uint32_t>> edge_set;
			uint32_t num_faces = hull->GetNumFaces();
			for (uint32_t f = 0; f < num_faces; f++) {
				uint32_t num_verts = hull->GetNumVerticesInFace(f);
				std::vector<uint32_t> face_verts(num_verts);
				hull->GetFaceVertices(f, num_verts, face_verts.data());
				for (uint32_t v = 0; v < num_verts; v++) {
					uint32_t a = face_verts[v];
					uint32_t b = face_verts[(v + 1) % num_verts];
					edge_set.insert({std::min(a, b), std::max(a, b)});
				}
			}
			ds.hull_edges.assign(edge_set.begin(), edge_set.end());
			return true;
		}
		default:
			return false;
	}
}

std::optional<DebugShape> PhysicsSystem::getDebugShape(Entity entity, Registry& registry) const {
	uint32_t idx = entity.index();
	if (!m_impl->hasBody(idx))
		return std::nullopt;

	auto& data = m_impl->getBody(idx);
	auto& body_interface = m_impl->physics_system->GetBodyInterface();
	JPH::RefConst<JPH::Shape> shape_ref = body_interface.GetShape(data.body_id);
	if (!shape_ref)
		return std::nullopt;

	DebugShape ds{};
	JPH::RMat44 world_tf = body_interface.GetWorldTransform(data.body_id);
	JPH::RVec3 bp = world_tf.GetTranslation();
	JPH::Quat br = world_tf.GetRotation().GetQuaternion();
	ds.position = glm::vec3(static_cast<float>(bp.GetX()), static_cast<float>(bp.GetY()), static_cast<float>(bp.GetZ()));
	ds.rotation = glm::quat(br.GetW(), br.GetX(), br.GetY(), br.GetZ());
	ds.is_dynamic = body_interface.GetMotionType(data.body_id) != JPH::EMotionType::Static;

	if (fillDebugShape(shape_ref.GetPtr(), ds))
		return ds;
	return std::nullopt;
}

} // namespace ve