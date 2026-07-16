#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <scene/ve_registry.hpp>
#include <scene/ve_component.hpp>
#include <physics/physics_system.hpp>
#include <input/character_input_driver.hpp>
#include <input/input_action.hpp>
#include <events/event_bus.hpp>
#include <resources/ve_animation_clip.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <memory>

using Catch::Approx;

namespace {

constexpr float FIXED_DT = 1.0f / 60.0f;

struct PhysicsFixture {
	ve::EventBus bus;
	ve::PhysicsSystem physics{bus};
	ve::Registry registry;

	PhysicsFixture() { physics.onSceneLoaded(registry); }
	~PhysicsFixture() { physics.onSceneUnloaded(); }

	// Static 20x20x1 box whose top face is at z = 0
	ve::Entity addFloor() {
		ve::Entity floor = registry.createEntity("floor");
		auto& tc = registry.addComponent<ve::TransformComponent>(floor);
		tc.setTranslation({0.0f, 0.0f, -0.5f});
		auto& rb = registry.addComponent<ve::RigidbodyComponent>(floor);
		rb.setMotionType(ve::PhysicsMotionType::Static);
		rb.setShapeDesc({.type = ve::PhysicsShapeType::Box, .half_extents = {10.0f, 10.0f, 0.5f}});
		return floor;
	}

	// Dynamic box resting on the floor; top face at z = center + half_z.
	ve::Entity addDynamicBox(glm::vec3 center, glm::vec3 half, float mass) {
		ve::Entity box = registry.createEntity("box");
		registry.addComponent<ve::TransformComponent>(box).setTranslation(center);
		auto& rb = registry.addComponent<ve::RigidbodyComponent>(box);
		rb.setMotionType(ve::PhysicsMotionType::Dynamic);
		rb.setMass(mass);
		rb.setShapeDesc({.type = ve::PhysicsShapeType::Box, .half_extents = half});
		return box;
	}

	ve::Entity addCharacter(glm::vec3 pos) {
		ve::Entity e = registry.createEntity("character");
		registry.addComponent<ve::TransformComponent>(e).setTranslation(pos);
		auto& cc = registry.addComponent<ve::CharacterControllerComponent>(e);
		cc.setRadius(0.3f);
		cc.setHalfHeight(0.2f);
		return e;
	}

	void step(int n) {
		for (int i = 0; i < n; i++)
			physics.update(FIXED_DT, registry);
	}

	glm::vec3 translation(ve::Entity e) {
		return registry.getComponent<ve::TransformComponent>(e)->getTranslation();
	}
};

} // namespace

TEST_CASE("Character controller registry integration", "[character][registry]") {
	ve::Registry registry;
	ve::Entity e = registry.createEntity("c");
	registry.addComponent<ve::TransformComponent>(e);
	auto& cc = registry.addComponent<ve::CharacterControllerComponent>(e);
	cc.setRadius(0.5f);
	cc.walk_speed = 3.0f;

	REQUIRE(registry.hasComponent<ve::CharacterControllerComponent>(e));

	ve::Entity clone = registry.cloneEntityRecursive(e);
	auto* cc_clone = registry.getComponent<ve::CharacterControllerComponent>(clone);
	REQUIRE(cc_clone != nullptr);
	REQUIRE(cc_clone->getRadius() == Approx(0.5f));
	REQUIRE(cc_clone->walk_speed == Approx(3.0f));

	registry.removeComponent<ve::CharacterControllerComponent>(e);
	REQUIRE_FALSE(registry.hasComponent<ve::CharacterControllerComponent>(e));
}

TEST_CASE("Character falls, lands on the floor, and reports grounded", "[character][physics]") {
	PhysicsFixture f;
	f.addFloor();
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 1.0f});

	f.step(120);

	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	REQUIRE(cc->grounded);
	REQUIRE(cc->ground_normal.z == Approx(1.0f).margin(0.01));
	// Feet at the floor top (small character padding tolerated)
	REQUIRE(f.translation(c).z == Approx(0.0f).margin(0.1));
}

TEST_CASE("Character walks with the desired velocity while grounded", "[character][physics]") {
	PhysicsFixture f;
	f.addFloor();
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 0.05f});

	f.step(30); // settle onto the floor
	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	REQUIRE(cc->grounded);

	cc->desired_velocity = {2.0f, 0.0f, 0.0f};
	f.step(60);

	glm::vec3 t = f.translation(c);
	REQUIRE(t.x == Approx(2.0f).margin(0.2));
	REQUIRE(cc->grounded);
	REQUIRE(cc->velocity.x == Approx(2.0f).margin(0.1));
}

TEST_CASE("Character without ground falls under gravity", "[character][physics]") {
	PhysicsFixture f;
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 5.0f});

	f.step(30); // 0.5s of free fall

	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	REQUIRE_FALSE(cc->grounded);
	REQUIRE(f.translation(c).z < 4.5f);
	REQUIRE(cc->velocity.z < -1.0f);
}

TEST_CASE("Character-only scene steps without any rigidbodies", "[character][physics]") {
	PhysicsFixture f;
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 2.0f});

	f.step(10);

	REQUIRE(f.translation(c).z < 2.0f); // gravity applied => characters were stepped
}

TEST_CASE("Character controller rebuild preserves position on param change", "[character][physics]") {
	PhysicsFixture f;
	f.addFloor();
	ve::Entity c = f.addCharacter({1.0f, 2.0f, 0.05f});
	f.step(30);
	glm::vec3 before = f.translation(c);

	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	cc->setRadius(0.4f); // dirties -> rebuild next update
	f.step(2);

	glm::vec3 after = f.translation(c);
	REQUIRE(after.x == Approx(before.x).margin(0.01));
	REQUIRE(after.y == Approx(before.y).margin(0.01));
}

TEST_CASE("Character jumps when requested while grounded", "[character][physics]") {
	PhysicsFixture f;
	f.addFloor();
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 0.05f});

	f.step(30); // settle onto the floor
	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	REQUIRE(cc->grounded);
	float z_rest = f.translation(c).z;

	cc->jump_speed = 5.0f;
	cc->jump_requested = true;
	f.step(1); // the request is consumed and the launch applied
	REQUIRE_FALSE(cc->jump_requested);
	REQUIRE_FALSE(cc->grounded);
	REQUIRE(cc->velocity.z > 0.0f);

	f.step(10);
	REQUIRE(f.translation(c).z > z_rest + 0.1f);

	// A jump requested mid-air is cleared without launching
	float vz_before = cc->velocity.z;
	cc->jump_requested = true;
	f.step(1);
	REQUIRE_FALSE(cc->jump_requested);
	REQUIRE(cc->velocity.z < vz_before); // decelerating, no second launch

	f.step(180); // fall back and settle
	REQUIRE(cc->grounded);
	REQUIRE(f.translation(c).z == Approx(z_rest).margin(0.1));
}

TEST_CASE("Freezing a character suspends its simulation", "[character][physics]") {
	PhysicsFixture f;
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 5.0f}); // no floor: free-falls unless frozen

	f.step(10);
	REQUIRE(f.translation(c).z < 5.0f); // falling before the freeze

	f.physics.freezeEntity(c);
	glm::vec3 held = f.translation(c);
	f.step(60); // 1s frozen
	REQUIRE(f.translation(c).z == Approx(held.z));

	f.physics.unfreezeEntity(c);
	f.step(10);
	REQUIRE(f.translation(c).z < held.z); // resumes falling
}

// Gizmo drags freeze the entity, then write the transform each frame. Without the
// freeze the character kept accumulating gravity and sagged below the handle.
TEST_CASE("Frozen character tracks external transform writes without sagging", "[character][physics]") {
	PhysicsFixture f;
	f.addFloor();
	ve::Entity c = f.addCharacter({0.0f, 0.0f, 3.0f});

	f.physics.freezeEntity(c);
	for (int i = 1; i <= 30; i++) {
		f.registry.getComponent<ve::TransformComponent>(c)->setTranslation({0.1f * i, 0.0f, 3.0f});
		f.physics.update(FIXED_DT, f.registry);
	}
	REQUIRE(f.translation(c).x == Approx(3.0f));
	REQUIRE(f.translation(c).z == Approx(3.0f)); // no gravity sag while frozen

	// Unfreezing re-syncs the character to where the drag left it, then it falls
	f.physics.unfreezeEntity(c);
	f.step(120);
	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	REQUIRE(cc->grounded);
	REQUIRE(f.translation(c).x == Approx(3.0f).margin(0.05)); // landed under the drag
	REQUIRE(f.translation(c).z == Approx(0.0f).margin(0.1));
}

TEST_CASE("Explicit dims still collide when the shape type needs absent geometry", "[character][physics]") {
	PhysicsFixture f;
	ve::Entity floor = f.registry.createEntity("hull_floor");
	f.registry.addComponent<ve::TransformComponent>(floor).setTranslation({0.0f, 0.0f, -0.5f});
	auto& rb = f.registry.addComponent<ve::RigidbodyComponent>(floor);
	rb.setMotionType(ve::PhysicsMotionType::Static);
	// ConvexHull needs CPU geometry this mesh-less entity does not have
	rb.setShapeDesc({.type = ve::PhysicsShapeType::ConvexHull, .half_extents = {10.0f, 10.0f, 0.5f}});

	ve::Entity c = f.addCharacter({0.0f, 0.0f, 1.0f});
	f.step(120);

	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(c);
	REQUIRE(cc->grounded); // the floor kept a collider derived from the dims
	REQUIRE(f.translation(c).z == Approx(0.0f).margin(0.1));
}

TEST_CASE("A rigidbody is refused on an entity that already has a character", "[character][physics]") {
	PhysicsFixture f;
	f.addFloor();

	ve::Entity e = f.registry.createEntity("both");
	f.registry.addComponent<ve::TransformComponent>(e).setTranslation({0.0f, 0.0f, 1.0f});
	f.registry.addComponent<ve::CharacterControllerComponent>(e);

	auto& rb = f.registry.addComponent<ve::RigidbodyComponent>(e); // second one loses
	rb.setMotionType(ve::PhysicsMotionType::Dynamic);
	rb.setShapeDesc({.type = ve::PhysicsShapeType::Box, .half_extents = {0.5f, 0.5f, 0.5f}});

	f.step(120);

	// Previously the body and the character each drove the transform, and the
	// character climbed its own box a half-extent per frame.
	auto* cc = f.registry.getComponent<ve::CharacterControllerComponent>(e);
	REQUIRE(cc->grounded);
	REQUIRE(f.translation(e).z == Approx(0.0f).margin(0.1));
}


// ── CharacterInputDriver ────────────────────────────────────────────────────

namespace {

std::shared_ptr<ve::VeAnimationClip> constantClip(const char* name) {
	auto clip = std::make_shared<ve::VeAnimationClip>();
	clip->name = name;
	clip->duration = 1.0f;
	clip->target_node_indices = {0};
	clip->samplers.push_back({.timestamps = {0.0f}, .values = {0.0f, 0.0f, 0.0f}, .component_count = 3});
	clip->channels.push_back({.target_slot = 0,
	                          .path = ve::AnimationPath::Translation,
	                          .interpolation = ve::AnimationInterpolation::Linear,
	                          .sampler_index = 0});
	return clip;
}

} // namespace

TEST_CASE("Driver clears possession when the entity is invalid", "[character][driver]") {
	ve::Registry registry;
	ve::CharacterInputDriver driver;
	ve::InputActions actions{};

	ve::Entity dead = registry.createEntity("gone");
	registry.destroyEntity(dead);
	ve::Entity possessed = dead;
	REQUIRE_FALSE(driver.tick(registry, actions, glm::vec3(1.0f, 0.0f, 0.0f), possessed, 0.016f));
	REQUIRE(possessed.isNull());
}

namespace {

ve::Entity makeDrivable(ve::Registry& registry, const char* name) {
	ve::Entity e = registry.createEntity(name);
	registry.addComponent<ve::TransformComponent>(e);
	auto& cc = registry.addComponent<ve::CharacterControllerComponent>(e);
	cc.walk_speed = 2.0f;
	cc.acceleration = 100.0f; // reaches walk_speed in one tick
	return e;
}

} // namespace

// PhysicsSystem applies desired_velocity every step whether or not anything is
// possessing the character, so releasing it has to zero the movement state.
TEST_CASE("Unpossessing zeroes the character's movement state", "[character][driver]") {
	ve::Registry registry;
	ve::CharacterInputDriver driver;
	ve::Entity e = makeDrivable(registry, "c");
	auto* cc = registry.getComponent<ve::CharacterControllerComponent>(e);

	ve::InputActions actions{};
	actions.move_forward = 1.0f;
	const glm::vec3 cam_fwd{-1.0f, 0.0f, 0.0f};

	ve::Entity possessed = e;
	REQUIRE(driver.tick(registry, actions, cam_fwd, possessed, 0.05f));
	REQUIRE(glm::length(glm::vec2(cc->desired_velocity)) == Approx(2.0f));

	// Unpossess with the key still held, as the Unpossess button leaves it
	possessed = ve::Entity::null();
	REQUIRE_FALSE(driver.tick(registry, actions, cam_fwd, possessed, 0.05f));
	REQUIRE(glm::length(glm::vec2(cc->desired_velocity)) == Approx(0.0f).margin(1e-4));
	REQUIRE_FALSE(cc->jump_requested);
}

TEST_CASE("Possessing a second character releases the first", "[character][driver]") {
	ve::Registry registry;
	ve::CharacterInputDriver driver;
	ve::Entity a = makeDrivable(registry, "a");
	ve::Entity b = makeDrivable(registry, "b");
	auto* cc_a = registry.getComponent<ve::CharacterControllerComponent>(a);
	auto* cc_b = registry.getComponent<ve::CharacterControllerComponent>(b);

	ve::InputActions actions{};
	actions.move_forward = 1.0f;
	const glm::vec3 cam_fwd{-1.0f, 0.0f, 0.0f};

	ve::Entity possessed = a;
	driver.tick(registry, actions, cam_fwd, possessed, 0.05f);
	REQUIRE(glm::length(glm::vec2(cc_a->desired_velocity)) == Approx(2.0f));

	possessed = b;
	driver.tick(registry, actions, cam_fwd, possessed, 0.05f);
	REQUIRE(glm::length(glm::vec2(cc_a->desired_velocity)) == Approx(0.0f).margin(1e-4));
	REQUIRE(glm::length(glm::vec2(cc_b->desired_velocity)) == Approx(2.0f));
}

// Space also raises the fly camera, so it is routinely held while unpossessed.
TEST_CASE("A jump key held across a possession change is not a fresh press", "[character][driver]") {
	ve::Registry registry;
	ve::CharacterInputDriver driver;
	ve::Entity e = makeDrivable(registry, "c");
	auto* cc = registry.getComponent<ve::CharacterControllerComponent>(e);

	ve::InputActions actions{};
	actions.jump = true;
	const glm::vec3 cam_fwd{1.0f, 0.0f, 0.0f};

	ve::Entity possessed = ve::Entity::null();
	for (int i = 0; i < 5; i++)
		driver.tick(registry, actions, cam_fwd, possessed, 0.016f);

	possessed = e;
	driver.tick(registry, actions, cam_fwd, possessed, 0.016f);
	REQUIRE_FALSE(cc->jump_requested); // no jump on the possess frame

	// Releasing and pressing again still jumps
	actions.jump = false;
	driver.tick(registry, actions, cam_fwd, possessed, 0.016f);
	actions.jump = true;
	driver.tick(registry, actions, cam_fwd, possessed, 0.016f);
	REQUIRE(cc->jump_requested);
}

TEST_CASE("Driver eases desired velocity toward camera-relative input", "[character][driver]") {
	ve::Registry registry;
	ve::CharacterInputDriver driver;

	ve::Entity e = registry.createEntity("c");
	registry.addComponent<ve::TransformComponent>(e);
	auto& cc = registry.addComponent<ve::CharacterControllerComponent>(e);
	cc.walk_speed = 2.0f;
	cc.acceleration = 20.0f;

	ve::InputActions actions{};
	actions.move_forward = 1.0f;

	ve::Entity possessed = e;
	const glm::vec3 cam_fwd{-1.0f, 0.0f, 0.0f};
	REQUIRE(driver.tick(registry, actions, cam_fwd, possessed, 0.05f)); // max_delta = 1.0
	REQUIRE(cc.desired_velocity.x == Approx(-1.0f).margin(1e-4));
	REQUIRE(cc.desired_velocity.y == Approx(0.0f).margin(1e-4));

	driver.tick(registry, actions, cam_fwd, possessed, 0.05f);
	REQUIRE(cc.desired_velocity.x == Approx(-2.0f).margin(1e-4)); // reached walk speed

	driver.tick(registry, actions, cam_fwd, possessed, 0.05f);
	REQUIRE(cc.desired_velocity.x == Approx(-2.0f).margin(1e-4)); // clamped at target
}

TEST_CASE("Driver turns the mesh front toward the movement heading", "[character][driver]") {
	ve::CharacterInputDriver driver;
	ve::InputActions actions{};
	actions.move_forward = 1.0f;

	const glm::vec3 cam_fwd{-1.0f, 0.0f, 0.0f};

	// Default offset: engine forward (+Y) points along the heading.
	{
		ve::Registry registry;
		ve::Entity e = registry.createEntity("c");
		auto& tc = registry.addComponent<ve::TransformComponent>(e);
		registry.addComponent<ve::CharacterControllerComponent>(e).turn_rate_deg = 720.0f;
		ve::Entity possessed = e;
		for (int i = 0; i < 30; i++)
			driver.tick(registry, actions, cam_fwd, possessed, 0.016f);
		glm::vec3 mesh_front = tc.getRotation() * glm::vec3(0.0f, 1.0f, 0.0f);
		REQUIRE(mesh_front.x == Approx(-1.0f).margin(1e-3));
		REQUIRE(mesh_front.y == Approx(0.0f).margin(1e-3));
	}

	// 180 offset: a -Y-facing mesh (its -Y axis) points along the heading.
	{
		ve::Registry registry;
		ve::Entity e = registry.createEntity("c");
		auto& tc = registry.addComponent<ve::TransformComponent>(e);
		auto& cc = registry.addComponent<ve::CharacterControllerComponent>(e);
		cc.turn_rate_deg = 720.0f;
		cc.facing_offset_deg = 180.0f;
		ve::Entity possessed = e;
		for (int i = 0; i < 30; i++)
			driver.tick(registry, actions, cam_fwd, possessed, 0.016f);
		glm::vec3 mesh_front = tc.getRotation() * glm::vec3(0.0f, -1.0f, 0.0f);
		REQUIRE(mesh_front.x == Approx(-1.0f).margin(1e-3));
		REQUIRE(mesh_front.y == Approx(0.0f).margin(1e-3));
	}

	// Parented under a yawed node: the heading is world-space, so the world-space
	// front must track it rather than being offset by the parent's rotation.
	{
		ve::Registry registry;
		ve::Entity parent = registry.createEntity("parent");
		registry.addComponent<ve::TransformComponent>(parent)
			.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)));

		ve::Entity e = registry.createEntity("c");
		registry.addComponent<ve::TransformComponent>(e);
		registry.addComponent<ve::CharacterControllerComponent>(e).turn_rate_deg = 720.0f;
		registry.setParent(e, parent);

		ve::Entity possessed = e;
		for (int i = 0; i < 30; i++)
			driver.tick(registry, actions, cam_fwd, possessed, 0.016f);

		glm::vec3 mesh_front = registry.getWorldRotation(e) * glm::vec3(0.0f, 1.0f, 0.0f);
		REQUIRE(mesh_front.x == Approx(-1.0f).margin(1e-3));
		REQUIRE(mesh_front.y == Approx(0.0f).margin(1e-3));
	}
}

TEST_CASE("Driver maps ground speed into a normalized gait", "[character][driver]") {
	ve::Registry registry;
	ve::CharacterInputDriver driver;

	ve::Entity node = registry.createEntity("node");
	registry.addComponent<ve::TransformComponent>(node);
	ve::Entity e = registry.createEntity("c");
	registry.addComponent<ve::TransformComponent>(e);
	auto& cc = registry.addComponent<ve::CharacterControllerComponent>(e);
	cc.walk_speed = 2.0f;
	cc.run_speed = 6.0f;

	auto& anim = registry.addComponent<ve::AnimatorComponent>(e);
	anim.setNodeToEntityMap({node});
	uint32_t idle = anim.addClip(constantClip("idle"));
	uint32_t walk = anim.addClip(constantClip("walk"), false);
	uint32_t run = anim.addClip(constantClip("run"), false);
	// Normalized gait space: idle/walk/run at 0/0.5/1, independent of the speeds.
	anim.setBlendSpace1D({
		{.position = 0.0f, .clip_index = idle},
		{.position = 0.5f, .clip_index = walk},
		{.position = 1.0f, .clip_index = run},
	});

	ve::InputActions actions{};
	ve::Entity possessed = e;
	const glm::vec3 cam_fwd{1.0f, 0.0f, 0.0f};

	auto gaitAt = [&](float speed) {
		cc.velocity = {speed, 0.0f, 0.0f}; // as if written by the physics step
		driver.tick(registry, actions, cam_fwd, possessed, 0.016f);
		return anim.getBlendParameter();
	};

	REQUIRE(gaitAt(1.0f) == Approx(0.25f)); // half walk_speed -> quarter gait
	REQUIRE(gaitAt(2.0f) == Approx(0.5f));  // walk_speed -> exactly walk
	REQUIRE(gaitAt(4.0f) == Approx(0.75f)); // midway walk..run
	REQUIRE(gaitAt(6.0f) == Approx(1.0f));  // run_speed -> full run
	REQUIRE(gaitAt(9.0f) == Approx(1.0f));  // beyond run_speed clamps

	// Lowering run_speed still reaches full run at the new top speed.
	cc.run_speed = 3.0f;
	REQUIRE(gaitAt(3.0f) == Approx(1.0f));
}
