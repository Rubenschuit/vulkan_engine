#include "fox.hpp"

namespace ve {

namespace {

constexpr float FOX_SCALE = 0.03f;
constexpr float FOX_FACING_OFFSET_DEG = 180.0f;

void wireFox(Registry& registry, Entity wrapper) {
	registry.setName(wrapper, "Fox");
	auto* anim = registry.getComponent<AnimatorComponent>(wrapper);
	if (!anim)
		return;
	int survey = anim->findClip("Survey");
	int walk = anim->findClip("Walk");
	int run = anim->findClip("Run");
	if (survey < 0 || walk < 0 || run < 0)
		return;

	anim->setSpeed(static_cast<uint32_t>(run), 2.0f);

	// Playable via the editor's Possess button
	auto& cc = registry.addComponent<CharacterControllerComponent>(wrapper);
	cc.setRadius(0.3f);
	cc.setHalfHeight(0.2f);
	cc.walk_speed = 4.0f;
	cc.run_speed = 12.0f;
	cc.facing_offset_deg = FOX_FACING_OFFSET_DEG;

	anim->setBlendSpace1D({
		{.position = 0.0f, .clip_index = static_cast<uint32_t>(survey)},
		{.position = 0.5f, .clip_index = static_cast<uint32_t>(walk)},
		{.position = 1.0f, .clip_index = static_cast<uint32_t>(run)},
	});
	anim->setLocomotionCadence(1.0f);
	anim->setBlendParameter(0.0f);

	// Air pose
	auto run_clip = anim->getClipBindings()[static_cast<size_t>(run)].clip;
	uint32_t air = anim->addClip(run_clip, false);
	anim->setSpeed(air, 0.5f);
	float air_time = 0.3f * run_clip->duration;
	anim->setTime(air, air_time);
	anim->setAirPose(static_cast<int>(air), air_time);

	// Follow cam: possess activates it
	Entity cam = registry.createFollowCamera(wrapper, "Fox Follow Camera");
	auto* fc = registry.getComponent<FollowCameraComponent>(cam);
	fc->distance = 14.0f;
	fc->pivot_height = 1.0f;
	fc->alignBehind();
}

} // namespace

AddModelRequestedEvent foxModelRequest(Registry& registry, const std::filesystem::path& gltf_path,
                                       glm::vec3 translation) {
	return {
		.gltf_path = gltf_path.lexically_normal(),
		.translation = translation,
		.rotation = {0.0f, 0.0f, glm::radians(-135.0f)},
		.scale = glm::vec3(FOX_SCALE),
		.on_loaded = [&registry](Entity wrapper) { wireFox(registry, wrapper); },
	};
}

}