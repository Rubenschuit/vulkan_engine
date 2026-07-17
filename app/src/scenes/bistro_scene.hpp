#pragma once
#include "VEngine/VEngine.hpp"
#include "../asset_paths.hpp"

namespace ve {

class BistroScene : public VeScene {
public:
	BistroScene(const SceneContext& ctx, const AssetPaths& paths);

	void update(float dt) override;

	glm::vec4 getDefaultAmbient() const override { return {1.0f, 1.0f, 1.0f, 0.05f}; }
	std::filesystem::path sceneOverlayPath() const override { return m_overlay_path; }

private:
	std::filesystem::path m_overlay_path;

	Entity m_birds;
	glm::vec3 m_birds_orbit_center{0.0f};
	float m_birds_orbit_radius = 80.0f;
	float m_birds_orbit_speed = 0.22f;
	float m_birds_t = 0.0f;
};

} // namespace ve
