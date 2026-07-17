#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <scene/ve_registry.hpp>
#include <scene/ve_component.hpp>
#include <scene/ecs_event_dispatcher.hpp>
#include <resources/ve_animation_clip.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>

using Catch::Approx;

namespace {

ve::AnimationSampler vec3Sampler(std::vector<float> timestamps, std::vector<float> values) {
	return {.timestamps = std::move(timestamps), .values = std::move(values), .component_count = 3};
}

ve::AnimationSampler quatSampler(std::vector<float> timestamps, std::vector<float> values) {
	return {.timestamps = std::move(timestamps), .values = std::move(values), .component_count = 4};
}

// Single-node clip with a translation channel and optionally a rotation channel.
std::shared_ptr<ve::VeAnimationClip> makeClip(std::string name, float duration,
                                              ve::AnimationSampler translation,
                                              ve::AnimationSampler* rotation = nullptr,
                                              uint32_t target_node = 0) {
	auto clip = std::make_shared<ve::VeAnimationClip>();
	clip->name = std::move(name);
	clip->duration = duration;
	clip->target_node_indices = {target_node};
	clip->samplers.push_back(std::move(translation));
	clip->channels.push_back({.target_slot = 0,
	                          .path = ve::AnimationPath::Translation,
	                          .interpolation = ve::AnimationInterpolation::Linear,
	                          .sampler_index = 0});
	if (rotation) {
		clip->samplers.push_back(std::move(*rotation));
		clip->channels.push_back({.target_slot = 0,
		                          .path = ve::AnimationPath::Rotation,
		                          .interpolation = ve::AnimationInterpolation::Linear,
		                          .sampler_index = 1});
	}
	return clip;
}

struct BlendFixture {
	ve::Registry registry;
	ve::Entity wrapper;
	std::vector<ve::Entity> nodes;
	ve::AnimatorComponent* animator = nullptr;

	explicit BlendFixture(size_t node_count = 1) {
		wrapper = registry.createEntity("wrapper");
		for (size_t i = 0; i < node_count; i++) {
			ve::Entity node = registry.createEntity("node" + std::to_string(i));
			registry.addComponent<ve::TransformComponent>(node);
			nodes.push_back(node);
		}
		animator = &registry.addComponent<ve::AnimatorComponent>(wrapper);
		animator->setNodeToEntityMap(nodes);
	}

	glm::vec3 translation(size_t node = 0) {
		return registry.getComponent<ve::TransformComponent>(nodes[node])->getTranslation();
	}
	glm::quat rotation(size_t node = 0) {
		return registry.getComponent<ve::TransformComponent>(nodes[node])->getRotation();
	}
};

} // namespace

TEST_CASE("Single clip at weight 1 matches the direct sample", "[animator][blend]") {
	BlendFixture f;
	// x moves 0 -> 10 over 1s
	f.animator->addClip(makeClip("move", 1.0f,
		vec3Sampler({0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f})));

	f.animator->update(0.25f);

	glm::vec3 t = f.translation();
	REQUIRE(t.x == Approx(2.5f));
	REQUIRE(t.y == Approx(0.0f));
	REQUIRE(t.z == Approx(0.0f));
}

TEST_CASE("Two clips at 0.5/0.5 blend to the midpoint", "[animator][blend]") {
	BlendFixture f;
	auto rot_identity = quatSampler({0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
	auto rot_90z = quatSampler({0.0f}, {0.0f, 0.0f, 0.70710678f, 0.70710678f});
	uint32_t a = f.animator->addClip(
		makeClip("a", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f}), &rot_identity));
	uint32_t b = f.animator->addClip(
		makeClip("b", 1.0f, vec3Sampler({0.0f}, {10.0f, 0.0f, 0.0f}), &rot_90z));
	f.animator->setClipWeight(a, 0.5f);
	f.animator->setClipWeight(b, 0.5f);

	f.animator->update(0.01f);

	glm::vec3 t = f.translation();
	REQUIRE(t.x == Approx(5.0f));

	// nlerp midpoint of unit quats equals the slerp midpoint
	glm::quat r = f.rotation();
	REQUIRE(glm::length(r) == Approx(1.0f));
	REQUIRE(std::abs(r.w) == Approx(std::cos(glm::radians(22.5f))).margin(1e-4));
	REQUIRE(std::abs(r.z) == Approx(std::sin(glm::radians(22.5f))).margin(1e-4));
}

TEST_CASE("crossFadeTo ramps weights and stops the faded-out clip", "[animator][blend]") {
	BlendFixture f;
	uint32_t a = f.animator->addClip(
		makeClip("a", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})));
	uint32_t b = f.animator->addClip(
		makeClip("b", 1.0f, vec3Sampler({0.0f}, {10.0f, 0.0f, 0.0f})), false);

	size_t state_events = 0;
	f.registry.events().subscribe<ve::AnimationStateChangedEvent>(
		[&state_events](const ve::AnimationStateChangedEvent&) { state_events++; });

	f.animator->crossFadeTo(b, 0.5f);
	REQUIRE(f.animator->getClipBindings()[b].playing);

	f.animator->update(0.25f);
	REQUIRE(f.animator->getClipWeight(a) == Approx(0.5f));
	REQUIRE(f.animator->getClipWeight(b) == Approx(0.5f));
	REQUIRE(f.translation().x == Approx(5.0f));

	f.animator->update(0.3f);
	REQUIRE(f.animator->getClipWeight(a) == Approx(0.0f));
	REQUIRE(f.animator->getClipWeight(b) == Approx(1.0f));
	REQUIRE_FALSE(f.animator->getClipBindings()[a].playing);
	REQUIRE(f.translation().x == Approx(10.0f));
	// The fade-out auto-stop must notify subscribers (GPU scene, shadow cache).
	REQUIRE(state_events >= 1);
}

TEST_CASE("Node animated by only one of two blended clips gets that clip's full pose", "[animator][blend]") {
	BlendFixture f(2);

	// Clip P animates node0 and node1; clip Q animates only node0.
	auto p = std::make_shared<ve::VeAnimationClip>();
	p->name = "p";
	p->duration = 1.0f;
	p->target_node_indices = {0, 1};
	p->samplers.push_back(vec3Sampler({0.0f}, {2.0f, 0.0f, 0.0f}));
	p->samplers.push_back(vec3Sampler({0.0f}, {4.0f, 0.0f, 0.0f}));
	p->channels.push_back({.target_slot = 0, .path = ve::AnimationPath::Translation,
	                       .interpolation = ve::AnimationInterpolation::Linear, .sampler_index = 0});
	p->channels.push_back({.target_slot = 1, .path = ve::AnimationPath::Translation,
	                       .interpolation = ve::AnimationInterpolation::Linear, .sampler_index = 1});

	uint32_t pi = f.animator->addClip(p);
	uint32_t qi = f.animator->addClip(
		makeClip("q", 1.0f, vec3Sampler({0.0f}, {6.0f, 0.0f, 0.0f})));
	f.animator->setClipWeight(pi, 0.5f);
	f.animator->setClipWeight(qi, 0.5f);

	f.animator->update(0.01f);

	REQUIRE(f.translation(0).x == Approx(4.0f));  // (2*0.5 + 6*0.5) / 1.0
	REQUIRE(f.translation(1).x == Approx(4.0f));  // (4*0.5) / 0.5 -> full pose of P
}

TEST_CASE("Locomotion cadence multiplier scales phase rate", "[animator][blend]") {
	BlendFixture f;
	// Clip b has duration 0.5 -> authored rate 2 cycles/s at cadence 1.
	uint32_t a = f.animator->addClip(makeClip("a", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})));
	uint32_t b = f.animator->addClip(makeClip("b", 0.5f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})), false);
	f.animator->setBlendSpace1D({{.position = 0.0f, .clip_index = a},
	                             {.position = 1.0f, .clip_index = b}}, true);
	f.animator->setBlendParameter(1.0f); // full weight on b

	// Cadence 1: phase_rate = weight/duration = 1/0.5 = 2 cycles/s -> phase 0.2 over 0.1s
	f.animator->setLocomotionCadence(1.0f);
	f.animator->update(0.1f);
	REQUIRE(f.animator->getClipBindings()[b].current_time == Approx(0.2f * 0.5f).margin(1e-3));

	// Doubling the multiplier doubles the cadence -> twice the phase advance
	f.animator->setLocomotionCadence(2.0f);
	float p0 = f.animator->getClipBindings()[b].current_time;
	f.animator->update(0.05f);
	float advanced = f.animator->getClipBindings()[b].current_time - p0;
	REQUIRE(advanced == Approx(0.05f * 2.0f * 2.0f * 0.5f).margin(1e-3)); // dt*cadence*rate*duration
}

TEST_CASE("Per-clip speed biases a synced member's shared cadence", "[animator][blend]") {
	BlendFixture f;
	uint32_t a = f.animator->addClip(makeClip("a", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})));
	uint32_t b = f.animator->addClip(makeClip("b", 0.5f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})), false);
	f.animator->setBlendSpace1D({{.position = 0.0f, .clip_index = a},
	                             {.position = 1.0f, .clip_index = b}}, true);
	f.animator->setBlendParameter(1.0f); // full weight on b

	// speed 2 halves b's effective duration -> doubles the phase rate vs speed 1.
	f.animator->setSpeed(b, 2.0f);
	f.animator->update(0.1f);
	// phase_rate = 1 / (0.5/2) = 4 cycles/s -> phase 0.4 -> t = 0.4 * 0.5
	REQUIRE(f.animator->getClipBindings()[b].current_time == Approx(0.4f * 0.5f).margin(1e-3));
}

// The phase is shared, so a member with no rate to contribute must drop out of the
// cadence rather than dragging every other member toward a standstill with it.
TEST_CASE("A non-positive speed member does not stall the shared phase", "[animator][blend]") {
	BlendFixture f;
	uint32_t a = f.animator->addClip(makeClip("a", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})));
	uint32_t b = f.animator->addClip(makeClip("b", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})), false);
	f.animator->setBlendSpace1D({{.position = 0.0f, .clip_index = a},
	                             {.position = 1.0f, .clip_index = b}}, true);
	f.animator->setBlendParameter(0.5f); // both members at weight 0.5

	// a alone drives the cadence: phase_rate = 0.5 / (0.5 * 1.0) = 1 cycle/s
	f.animator->setSpeed(b, 0.0f);
	f.animator->update(0.1f);
	REQUIRE(f.animator->getClipBindings()[a].current_time == Approx(0.1f).margin(1e-3));

	float t0 = f.animator->getClipBindings()[a].current_time;
	f.animator->setSpeed(b, -2.0f);
	f.animator->update(0.1f);
	REQUIRE(f.animator->getClipBindings()[a].current_time - t0 == Approx(0.1f).margin(1e-3));
}

TEST_CASE("fadeClipWeight ramps one binding and auto-stops at zero", "[animator][blend]") {
	BlendFixture f;
	uint32_t a = f.animator->addClip(makeClip("a", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})));
	uint32_t b = f.animator->addClip(makeClip("b", 1.0f, vec3Sampler({0.0f}, {1.0f, 0.0f, 0.0f})));

	f.animator->fadeClipWeight(b, 0.0f, 0.5f);
	f.animator->update(0.25f);
	REQUIRE(f.animator->getClipWeight(b) == Approx(0.5f));
	REQUIRE(f.animator->getClipWeight(a) == Approx(1.0f)); // other bindings untouched

	f.animator->update(0.3f);
	REQUIRE(f.animator->getClipWeight(b) == Approx(0.0f));
	REQUIRE_FALSE(f.animator->getClipBindings()[b].playing); // auto-stopped

	// fade_seconds <= 0 snaps
	f.animator->fadeClipWeight(a, 0.0f, 0.0f);
	REQUIRE(f.animator->getClipWeight(a) == Approx(0.0f));
	REQUIRE_FALSE(f.animator->getClipBindings()[a].playing);
}

TEST_CASE("1D blend space brackets the parameter and drives member weights", "[animator][blend]") {
	BlendFixture f;
	uint32_t survey = f.animator->addClip(
		makeClip("survey", 1.0f, vec3Sampler({0.0f}, {0.0f, 0.0f, 0.0f})));
	uint32_t walk = f.animator->addClip(
		makeClip("walk", 1.0f, vec3Sampler({0.0f}, {1.0f, 0.0f, 0.0f})), false);
	uint32_t run = f.animator->addClip(
		makeClip("run", 2.0f, vec3Sampler({0.0f}, {2.0f, 0.0f, 0.0f})), false);

	REQUIRE(f.animator->findClip("walk") == static_cast<int>(walk));
	REQUIRE(f.animator->findClip("missing") == -1);

	f.animator->setBlendSpace1D({
		{.position = 0.0f, .clip_index = survey},
		{.position = 0.5f, .clip_index = walk},
		{.position = 1.0f, .clip_index = run},
	});

	f.animator->setBlendParameter(0.25f);
	REQUIRE(f.animator->getClipWeight(survey) == Approx(0.5f));
	REQUIRE(f.animator->getClipWeight(walk) == Approx(0.5f));
	REQUIRE(f.animator->getClipWeight(run) == Approx(0.0f));

	f.animator->setBlendParameter(1.0f);
	REQUIRE(f.animator->getClipWeight(run) == Approx(1.0f));

	// Out-of-range parameters clamp to the sample range.
	f.animator->setBlendParameter(5.0f);
	REQUIRE(f.animator->getClipWeight(run) == Approx(1.0f));

	// crossFadeTo suspends the space; setBlendParameter resumes it.
	f.animator->crossFadeTo(survey, 0.0f);
	REQUIRE_FALSE(f.animator->isBlendSpaceActive());
	REQUIRE(f.animator->getClipWeight(survey) == Approx(1.0f));
	f.animator->setBlendParameter(0.0f);
	REQUIRE(f.animator->isBlendSpaceActive());
}

TEST_CASE("crossFadeTo restarts a finished non-looping clip from the beginning", "[animator][blend]") {
	BlendFixture f;
	// x moves 0 -> 10 over 1s, non-looping.
	uint32_t clip = f.animator->addClip(
		makeClip("oneshot", 1.0f, vec3Sampler({0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f})),
		/*auto_play=*/true, /*loop=*/false);

	// Run it to completion; it stops parked at the end.
	f.animator->update(1.5f);
	REQUIRE_FALSE(f.animator->getClipBindings()[clip].playing);
	REQUIRE(f.translation().x == Approx(10.0f));

	// Cross-fading back must rewind and actually play, not re-stop on the next tick.
	f.animator->crossFadeTo(clip, 0.0f);
	REQUIRE(f.animator->getClipBindings()[clip].playing);

	f.animator->update(0.25f);
	REQUIRE(f.animator->getClipBindings()[clip].playing);
	REQUIRE(f.translation().x == Approx(2.5f));
}
