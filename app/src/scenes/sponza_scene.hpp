#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"

namespace ve {

class SponzaScene : public VeScene {
public:
	SponzaScene(const SceneContext& ctx, const AssetPaths& paths);

	void update(float dt) override;

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.04f}; }

private:
	Entity m_birds;
	glm::vec3 m_birds_orbit_center{0.0f};
	float m_birds_orbit_radius = 80.0f;
	float m_birds_orbit_speed = 0.1f;
	float m_birds_t = 0.0f;

	std::vector<Entity> m_fire_lights;
	float m_fire_t = 0.0f;
};

}
