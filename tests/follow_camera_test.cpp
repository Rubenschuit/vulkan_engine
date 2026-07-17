#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <scene/ve_registry.hpp>
#include <scene/ve_component.hpp>
#include <scene/camera_view.hpp>
#include <scene/camera_manager.hpp>
#include <scene/camera_math.hpp>
#include <scene/fly_camera_controller.hpp>
#include <input/input_action.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

using Catch::Approx;

namespace {

struct FollowFixture {
	ve::Registry registry;
	ve::Entity target;
	ve::Entity cam;
	ve::FollowCameraComponent* fc = nullptr;

	explicit FollowFixture(glm::vec3 target_pos = {0.0f, 0.0f, 0.0f}) {
		target = registry.createEntity("target");
		registry.addComponent<ve::TransformComponent>(target).setTranslation(target_pos);
		cam = registry.createFollowCamera(target);
		fc = registry.getComponent<ve::FollowCameraComponent>(cam);
	}

	ve::CameraView view(float aspect = 1.0f) { return ve::buildCameraView(registry, cam, aspect); }
};

} // namespace

TEST_CASE("createFollowCamera composes transform, lens, and rig", "[camera][follow]") {
	FollowFixture f;
	REQUIRE(f.registry.hasComponent<ve::TransformComponent>(f.cam));
	REQUIRE(f.registry.hasComponent<ve::CameraComponent>(f.cam));
	REQUIRE(f.fc != nullptr);
	REQUIRE(f.fc->target == f.target);
}

TEST_CASE("cloneEntityRecursive remaps the follow target inside the subtree", "[camera][follow]") {
	FollowFixture f;
	ve::Entity root = f.registry.createEntity("root");
	f.registry.addComponent<ve::TransformComponent>(root);
	f.registry.setParent(f.target, root);
	f.registry.setParent(f.cam, root);

	ve::Entity clone = f.registry.cloneEntityRecursive(root);
	auto* cloned_fc = f.registry.getComponentInChildren<ve::FollowCameraComponent>(clone);
	REQUIRE(cloned_fc != nullptr);
	REQUIRE(cloned_fc->target != f.target); // remapped to the cloned target
	REQUIRE(f.registry.isAlive(cloned_fc->target));
}

TEST_CASE("Follow camera orbits behind the pivot through the entity-camera path", "[camera][follow]") {
	FollowFixture f;
	f.fc->pivot_height = 1.6f;
	f.fc->distance = 5.0f;
	f.fc->current_distance = 5.0f;
	f.fc->yaw = 0.0f; // looks down -X
	f.fc->pitch = 0.0f;

	REQUIRE(f.fc->tick({}, ve::InputActions{}, 0.0f));

	ve::CameraView v = f.view(16.0f / 9.0f);
	REQUIRE(v.position.x == Approx(5.0f).margin(1e-4));
	REQUIRE(v.position.y == Approx(0.0f).margin(1e-4));
	REQUIRE(v.position.z == Approx(1.6f).margin(1e-4));
	REQUIRE(v.forward.x == Approx(-1.0f).margin(1e-4));
	REQUIRE(v.up.z == Approx(1.0f).margin(1e-4));
}

TEST_CASE("Follow camera tracks a moved pivot", "[camera][follow]") {
	FollowFixture f({10.0f, -4.0f, 2.0f});
	f.fc->pivot_height = 1.0f;
	f.fc->distance = 3.0f;
	f.fc->current_distance = 3.0f;
	f.fc->yaw = 0.0f;
	f.fc->pitch = 0.0f;

	f.fc->tick({}, ve::InputActions{}, 0.0f);

	ve::CameraView v = f.view();
	REQUIRE(v.position.x == Approx(13.0f).margin(1e-4));
	REQUIRE(v.position.y == Approx(-4.0f).margin(1e-4));
	REQUIRE(v.position.z == Approx(3.0f).margin(1e-4)); // 2 + pivot_height
}

TEST_CASE("Follow camera projection contract via CameraComponent", "[camera][follow]") {
	FollowFixture f;
	f.registry.getComponent<ve::CameraComponent>(f.cam)->setNear(0.25f);
	f.fc->tick({}, ve::InputActions{}, 0.0f);

	ve::CameraView v = f.view(16.0f / 9.0f);
	// render_pipeline sniffs proj[3][3] == 0 to mean perspective -- do not "clean up".
	REQUIRE(v.proj[3][3] == 0.0f);
	REQUIRE(v.proj[2][3] == Approx(-1.0f));
	REQUIRE(v.proj[3][2] == Approx(0.25f)); // near
	REQUIRE(v.proj[1][1] < 0.0f);           // Vulkan Y-flip
	REQUIRE(v.proj[2][2] == 0.0f);          // infinite far
}

// The fly <-> follow handoff passes raw yaw/pitch, so the two cameras must agree
// on the look convention or every possess/unpossess snaps.
TEST_CASE("Follow and fly cameras share the look convention", "[camera][follow]") {
	FollowFixture f;
	f.fc->yaw = 0.0f;
	f.fc->pitch = 0.0f;
	ve::FlyCameraController fly;
	fly.setYawPitch(0.0f, 0.0f);

	ve::InputActions a{};
	a.mouse_look_enabled = true;
	a.mouse_dx = 13.0f;
	a.mouse_dy = -7.0f;

	for (int i = 0; i < 5; i++) {
		f.fc->tick({}, a, 0.016f);
		fly.tick(a, 0.016f);
	}

	REQUIRE(f.fc->yaw == Approx(fly.yaw()).margin(1e-5));
	REQUIRE(f.fc->pitch == Approx(fly.pitch()).margin(1e-5));
	ve::CameraView v = f.view();
	REQUIRE(glm::dot(v.forward, fly.forward()) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("Follow camera look input semantics", "[camera][follow]") {
	SECTION("mouse_look_enabled gates the mouse") {
		FollowFixture f;
		f.fc->yaw = 0.5f;
		ve::InputActions a{};
		a.mouse_look_enabled = false;
		a.mouse_dx = 100.0f;
		f.fc->tick({}, a, 0.016f);
		REQUIRE(f.fc->yaw == Approx(0.5f));
	}

	SECTION("mouse deltas are not dt-scaled") {
		ve::InputActions a{};
		a.mouse_look_enabled = true;
		a.mouse_dx = 10.0f;

		FollowFixture slow, fast;
		slow.fc->yaw = 0.0f;
		fast.fc->yaw = 0.0f;
		slow.fc->tick({}, a, 0.001f);
		fast.fc->tick({}, a, 0.1f);
		REQUIRE(slow.fc->yaw == Approx(fast.fc->yaw).margin(1e-6));
	}
}

TEST_CASE("Follow camera pitch stays inside the orbit range and the basis stays finite", "[camera][follow]") {
	FollowFixture f;
	f.fc->min_pitch_deg = -70.0f;
	f.fc->max_pitch_deg = 60.0f;

	ve::InputActions a{};
	a.mouse_look_enabled = true;

	// Slam the pitch: at +-90 the basis would degenerate to NaN.
	a.mouse_dy = 10000.0f;
	f.fc->tick({}, a, 0.016f);
	REQUIRE(f.fc->pitch == Approx(glm::radians(60.0f)));
	ve::CameraView v = f.view();
	REQUIRE(std::isfinite(v.right.x));
	REQUIRE(glm::length(v.right) == Approx(1.0f).margin(1e-4));

	a.mouse_dy = -10000.0f;
	f.fc->tick({}, a, 0.016f);
	REQUIRE(f.fc->pitch == Approx(glm::radians(-70.0f)));
	v = f.view();
	REQUIRE(std::isfinite(v.right.x));
	REQUIRE(glm::length(v.right) == Approx(1.0f).margin(1e-4));
}

TEST_CASE("Follow camera survives pitch limits authored past the degenerate angle", "[camera][follow]") {
	FollowFixture f;
	f.fc->min_pitch_deg = -90.0f;
	f.fc->max_pitch_deg = 90.0f;

	const float lim = glm::radians(ve::ORBIT_PITCH_LIMIT_DEG);
	REQUIRE(f.fc->pitchLimitsRad().x == Approx(-lim));
	REQUIRE(f.fc->pitchLimitsRad().y == Approx(lim));

	ve::InputActions a{};
	a.mouse_look_enabled = true;

	for (float dy : {10000.0f, -10000.0f}) {
		a.mouse_dy = dy;
		f.fc->tick({}, a, 0.016f);
		REQUIRE(std::abs(f.fc->pitch) <= lim + 1e-5f);
		ve::CameraView v = f.view();
		REQUIRE(std::isfinite(v.right.x));
		REQUIRE(std::isfinite(v.up.z));
		REQUIRE(std::isfinite(v.position.z));
		REQUIRE(glm::length(v.right) == Approx(1.0f).margin(1e-4));
	}

	SECTION("inverted limits collapse to a point rather than a NaN range") {
		f.fc->min_pitch_deg = 40.0f;
		f.fc->max_pitch_deg = -40.0f;
		glm::vec2 limits = f.fc->pitchLimitsRad();
		REQUIRE(limits.x <= limits.y);

		f.fc->tick({}, ve::InputActions{}, 0.016f);
		REQUIRE(std::isfinite(f.fc->pitch));
		REQUIRE(std::isfinite(f.view().right.x));
	}
}

// writePose derives a world pose, but TransformComponent stores a local one. A rig
// parented under anything non-identity must still land where the math says.
TEST_CASE("Follow camera poses correctly under a transformed parent", "[camera][follow]") {
	FollowFixture f;
	f.fc->pivot_height = 0.0f;
	f.fc->distance = 5.0f;
	f.fc->current_distance = 5.0f;
	f.fc->yaw = 0.0f; // looks down -X, so the eye sits at +X
	f.fc->pitch = 0.0f;

	// Unparented reference pose
	f.fc->tick({}, ve::InputActions{}, 0.0f);
	const ve::CameraView flat = f.view();
	REQUIRE(flat.position.x == Approx(5.0f).margin(1e-4));

	ve::Entity rig = f.registry.createEntity("rig");
	auto& rig_tc = f.registry.addComponent<ve::TransformComponent>(rig);
	rig_tc.setTranslation({100.0f, -30.0f, 7.0f});
	rig_tc.setRotationEuler({0.0f, 0.0f, glm::radians(37.0f)});
	f.registry.setParent(f.cam, rig);

	f.fc->tick({}, ve::InputActions{}, 0.0f);
	const ve::CameraView parented = f.view();

	// Same world pose as unparented: the parent must be undone, not inherited.
	REQUIRE(glm::length(parented.position - flat.position) == Approx(0.0f).margin(1e-3));
	REQUIRE(glm::dot(parented.forward, flat.forward) == Approx(1.0f).margin(1e-4));
	REQUIRE(glm::dot(parented.up, flat.up) == Approx(1.0f).margin(1e-4));
}

TEST_CASE("Spring arm behavior", "[camera][follow][arm]") {
	FollowFixture f;
	f.fc->distance = 5.0f;
	f.fc->min_distance = 0.6f;
	f.fc->pull_out_speed = 4.0f;
	f.fc->yaw = 0.0f;
	f.fc->pitch = 0.0f;
	f.fc->current_distance = 5.0f;

	SECTION("no sweep fn means the full distance") {
		f.fc->tick({}, ve::InputActions{}, 0.016f);
		REQUIRE(f.fc->current_distance == Approx(5.0f));
	}

	SECTION("a hit shortens the arm on the very first frame, at any dt") {
		ve::CameraSweepFn sweep = [](const glm::vec3&, const glm::vec3&, float) {
			return std::optional<float>{2.0f};
		};
		f.fc->tick(sweep, ve::InputActions{}, 0.0f);
		REQUIRE(f.fc->current_distance == Approx(2.0f));
		REQUIRE(f.view().position.x == Approx(2.0f).margin(1e-4)); // pivot at origin+z, eye on +X
	}

	SECTION("min_distance is a hard floor") {
		ve::CameraSweepFn sweep = [](const glm::vec3&, const glm::vec3&, float) {
			return std::optional<float>{0.1f};
		};
		f.fc->tick(sweep, ve::InputActions{}, 0.0f);
		REQUIRE(f.fc->current_distance == Approx(0.6f));
	}

	SECTION("cleared path eases back out at pull_out_speed without overshoot") {
		f.fc->current_distance = 2.0f;
		f.fc->tick({}, ve::InputActions{}, 0.25f);
		REQUIRE(f.fc->current_distance == Approx(3.0f));
		f.fc->tick({}, ve::InputActions{}, 0.25f);
		REQUIRE(f.fc->current_distance == Approx(4.0f));
		f.fc->tick({}, ve::InputActions{}, 0.25f);
		REQUIRE(f.fc->current_distance == Approx(5.0f));
		f.fc->tick({}, ve::InputActions{}, 0.25f);
		REQUIRE(f.fc->current_distance == Approx(5.0f));
	}

	SECTION("the sweep is asked the right question") {
		f.fc->pivot_height = 1.5f;
		f.fc->distance = 4.0f;
		f.fc->collision_radius = 0.3f;
		glm::vec3 got_from{0.0f}, got_to{0.0f};
		float got_radius = 0.0f;
		ve::CameraSweepFn sweep = [&](const glm::vec3& from, const glm::vec3& to, float r)
			-> std::optional<float> {
			got_from = from;
			got_to = to;
			got_radius = r;
			return std::nullopt;
		};
		f.fc->tick(sweep, ve::InputActions{}, 0.0f);
		REQUIRE(got_from.z == Approx(1.5f).margin(1e-4)); // pivot = target + pivot_height*Z
		REQUIRE(got_to.x == Approx(4.0f).margin(1e-4));   // pivot - forward*distance
		REQUIRE(got_to.z == Approx(1.5f).margin(1e-4));
		REQUIRE(got_radius == Approx(0.3f));
	}
}

TEST_CASE("alignBehind faces the logical heading and honors facing_offset_deg", "[camera][follow]") {
	FollowFixture f;
	f.fc->pitch = glm::radians(-15.0f);

	SECTION("plain target: camera looks along entity +Y") {
		f.fc->alignBehind();
		ve::CameraView v = f.view();
		glm::vec2 fwd_xy = glm::normalize(glm::vec2(v.forward));
		REQUIRE(fwd_xy.y == Approx(1.0f).margin(1e-4));
		REQUIRE(v.position.y < 0.0f); // behind the +Y heading
		REQUIRE(f.fc->pitch == Approx(glm::radians(-15.0f)).margin(1e-5)); // authored pitch kept
	}

	SECTION("facing_offset_deg=180 (Fox): camera sits behind the visual front") {
		auto& cc = f.registry.addComponent<ve::CharacterControllerComponent>(f.target);
		cc.facing_offset_deg = 180.0f;
		f.fc->alignBehind();
		ve::CameraView v = f.view();
		glm::vec2 fwd_xy = glm::normalize(glm::vec2(v.forward));
		REQUIRE(fwd_xy.y == Approx(-1.0f).margin(1e-4)); // looks along the visual front (-Y)
		REQUIRE(v.position.y > 0.0f);
	}
}

TEST_CASE("Follow tick fails cleanly on an invalid target", "[camera][follow]") {
	FollowFixture f;
	f.registry.destroyEntity(f.target);
	REQUIRE_FALSE(f.fc->tick({}, ve::InputActions{}, 0.016f));

	FollowFixture untargeted;
	untargeted.fc->target = ve::Entity::null();
	REQUIRE_FALSE(untargeted.fc->tick({}, ve::InputActions{}, 0.016f));
}

// ── CameraManager ───────────────────────────────────────────────────────────

TEST_CASE("CameraManager ticks the follow rig or the fly camera", "[camera][manager]") {
	FollowFixture f;
	ve::CameraManager cameras;

	// Active follow camera: orbit consumes the mouse, fly camera untouched.
	ve::InputActions a{};
	a.mouse_look_enabled = true;
	a.mouse_dx = 20.0f;
	float fly_yaw = cameras.flyCamera().yaw();
	float follow_yaw = f.fc->yaw;
	REQUIRE(cameras.tick(&f.registry, f.cam, a, 0.016f));
	REQUIRE(f.fc->yaw != Approx(follow_yaw));
	REQUIRE(cameras.flyCamera().yaw() == Approx(fly_yaw));

	// No active entity: the fly camera ticks.
	cameras.tick(&f.registry, ve::Entity::null(), a, 0.016f);
	REQUIRE(cameras.flyCamera().yaw() != Approx(fly_yaw));

	// Dead target reports false so the caller can clear the selection.
	f.registry.destroyEntity(f.target);
	REQUIRE_FALSE(cameras.tick(&f.registry, f.cam, a, 0.016f));
}

TEST_CASE("CameraManager leaves the fly camera alone under a plain entity camera", "[camera][manager]") {
	ve::Registry registry;
	ve::Entity cam = registry.createEntity("static cam");
	registry.addComponent<ve::TransformComponent>(cam);
	registry.addComponent<ve::CameraComponent>(cam);

	ve::CameraManager cameras;
	ve::InputActions a{};
	a.mouse_look_enabled = true;
	a.mouse_dx = 25.0f;
	a.move_forward = 1.0f;

	const float yaw = cameras.flyCamera().yaw();
	const glm::vec3 pos = cameras.flyCamera().position();

	REQUIRE(cameras.tick(&registry, cam, a, 0.016f));
	REQUIRE(cameras.flyCamera().yaw() == Approx(yaw));
	REQUIRE(glm::length(cameras.flyCamera().position() - pos) == Approx(0.0f).margin(1e-6));
}

TEST_CASE("activeForward agrees with the rendered forward under a scaled parent", "[camera][manager]") {
	ve::Registry registry;
	ve::Entity parent = registry.createEntity("scaled parent");
	auto& ptc = registry.addComponent<ve::TransformComponent>(parent);
	ptc.setScale({4.0f, 0.25f, 2.0f}); // non-uniform: skews the child's world columns
	ptc.setRotationEuler({0.0f, 0.0f, glm::radians(50.0f)});

	ve::Entity cam = registry.createEntity("cam");
	auto& ctc = registry.addComponent<ve::TransformComponent>(cam);
	ctc.setRotationEuler({glm::radians(20.0f), 0.0f, glm::radians(-25.0f)});
	registry.addComponent<ve::CameraComponent>(cam);
	registry.setParent(cam, parent);

	ve::CameraManager cameras;
	glm::vec3 input_fwd = cameras.activeForward(&registry, cam);
	glm::vec3 render_fwd = ve::buildCameraView(registry, cam, 1.0f).forward;

	REQUIRE(glm::dot(input_fwd, render_fwd) == Approx(1.0f).margin(1e-5));
	REQUIRE(glm::length(input_fwd) == Approx(1.0f).margin(1e-5));

	// No camera -> the fly camera's own forward, not a zero vector.
	REQUIRE(glm::dot(cameras.activeForward(&registry, ve::Entity::null()),
	                 cameras.flyCamera().forward()) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("CameraManager seeds the fly camera when leaving an entity camera", "[camera][manager]") {
	FollowFixture f({4.0f, 2.0f, 0.0f});
	f.fc->tick({}, ve::InputActions{}, 0.0f);
	ve::CameraManager cameras;

	const ve::CameraView& through_entity = cameras.resolveView(&f.registry, f.cam, 1.0f, glm::radians(60.0f));
	glm::vec3 entity_pos = through_entity.position;
	glm::vec3 entity_fwd = through_entity.forward;
	REQUIRE(through_entity.source == f.cam);

	// Drop to fly: the fly camera resumes exactly where the entity camera was.
	const ve::CameraView& through_fly = cameras.resolveView(&f.registry, ve::Entity::null(), 1.0f, glm::radians(60.0f));
	REQUIRE(through_fly.source == ve::Entity::null());
	REQUIRE(glm::length(through_fly.position - entity_pos) == Approx(0.0f).margin(1e-4));
	REQUIRE(glm::dot(through_fly.forward, entity_fwd) == Approx(1.0f).margin(1e-4));
}

// acquireFollowCamera (inspector) matches a rig to its target by scanning this view.
// Active filtering is on by default, so a deactivated rig would go unseen and Possess
// would mint a duplicate every click.
TEST_CASE("A deactivated follow rig is still findable by target", "[camera][follow]") {
	FollowFixture f;
	f.registry.setActive(f.cam, false);

	auto count_matching = [&](bool include_inactive) {
		int n = 0;
		if (include_inactive) {
			for (auto [e, fc] : f.registry.view<ve::FollowCameraComponent>().includeInactive())
				if (fc.target == f.target)
					n++;
		} else {
			for (auto [e, fc] : f.registry.view<ve::FollowCameraComponent>())
				if (fc.target == f.target)
					n++;
		}
		return n;
	};

	REQUIRE(count_matching(false) == 0); // the trap
	REQUIRE(count_matching(true) == 1);  // what the inspector must do
}

// tick() must advance whatever resolveView() renders. The Inspector's X on the Camera
// header removes CameraComponent without touching FollowCameraComponent, so a rig can
// legitimately exist without a lens -- and then the FLY camera is what renders.
TEST_CASE("A follow rig without a CameraComponent still ticks the fly camera", "[camera][manager]") {
	FollowFixture f;
	f.registry.removeComponent<ve::CameraComponent>(f.cam);
	REQUIRE(f.registry.hasComponent<ve::FollowCameraComponent>(f.cam));

	ve::CameraManager cameras;
	ve::InputActions a{};
	a.mouse_look_enabled = true;
	a.mouse_dx = 20.0f;

	const float fly_yaw = cameras.flyCamera().yaw();
	cameras.tick(&f.registry, f.cam, a, 0.016f);

	// resolveView falls back to the fly camera here, so the fly camera must have moved.
	const ve::CameraView& v = cameras.resolveView(&f.registry, f.cam, 1.0f, glm::radians(60.0f));
	REQUIRE(v.source == ve::Entity::null()); // fly camera rendered
	REQUIRE(cameras.flyCamera().yaw() != Approx(fly_yaw)); // and it was advanced
}
