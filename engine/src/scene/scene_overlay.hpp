#pragma once
/* SceneOverlay: a diff of overrides applied on top of instantiated assets.
 *
 * A scene with thousands of asset-derived entities is never serialized whole;
 * instead it is described as its base asset(s) plus this overlay of overrides.
 * Current scope is only light overrides on emissive-derived lights.
 */

#include "ve_export.hpp"

#include <glm/glm.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ve {

class Registry;

class VENGINE_API SceneOverlay {
public:
	// How an override's selector matches an entity
	enum class SelectorKind : uint8_t { Name, Group, Contains };

	struct LightOverride {
		SelectorKind selector = SelectorKind::Group;
		std::string  selector_value;
		std::optional<int> occurrence;

		std::optional<bool>      active;
		std::optional<glm::vec3> color;          // absolute tint
		std::optional<float>     intensity;       // absolute; wins over intensity_mul
		std::optional<float>     intensity_mul;   // multiplier on the extracted value
		std::optional<float>     range;           // 0 = auto-derive

		// Convert a point light to a spot
		bool      to_spot = false;
		float     spot_inner_rad = glm::radians(25.0f);
		float     spot_outer_rad = glm::radians(35.0f);
		glm::vec3 spot_dir_local{0.0f, 0.0f, -1.0f};
	};

	int version = 1;
	std::vector<LightOverride> light_overrides;

	// Parse from a JSON file
	static std::optional<SceneOverlay> loadFromFile(const std::filesystem::path& path);

	// Apply to entities already instantiated in `registry`
	void apply(Registry& registry) const;

	// Serialize the current state of every active emissive-derived light in
	// `registry` to a JSON overlay at `path`
	static bool saveEmissiveState(Registry& registry, const std::filesystem::path& path);
};

}