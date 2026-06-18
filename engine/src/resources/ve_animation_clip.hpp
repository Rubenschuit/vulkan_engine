/* VeAnimationClip - static keyframe data parsed from glTF animations.
 * Stores samplers (timestamps + values) and channels (node target + property path).
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ve {

enum class AnimationPath : uint8_t {
	Translation,
	Rotation,
	Scale,
	Weights
};

enum class AnimationInterpolation : uint8_t {
	Step,
	Linear,
	CubicSpline
};

struct AnimationSampler {
	std::vector<float> timestamps;
	std::vector<float> values;
	uint8_t component_count = 3; 
	uint16_t weights_target_count = 0;
};

struct AnimationChannel {
	uint32_t target_slot;
	AnimationPath path;
	AnimationInterpolation interpolation;
	uint32_t sampler_index;
};

struct VeAnimationClip {
	std::string name;
	float duration = 0.0f;
	std::vector<AnimationSampler> samplers;
	std::vector<AnimationChannel> channels;
	std::vector<uint32_t> target_node_indices;
};

} // namespace ve