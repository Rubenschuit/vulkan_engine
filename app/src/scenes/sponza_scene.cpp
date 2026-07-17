#include "sponza_scene.hpp"
#include "events/engine_events.hpp"
#include "utils/ve_path.hpp"
#include <string>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace ve {

namespace {
constexpr float FIRE_INTENSITY = 9.0f;
}

SponzaScene::SponzaScene(const SceneContext& ctx, const AssetPaths& paths)
    : VeScene(ctx, "Sponza Scene") {

	m_event_bus.enqueue(SkyboxRequestEvent{
		.name = "qwantani_moon_noon_puresky_4k", .exposure = 0.15f, .is_day = false});
	m_event_bus.enqueue(RenderSettingsRequestEvent{
		.exposure = 1.05f, .ibl_diffuse_intensity = 0.1f, .ibl_specular_intensity = 0.18f,
		.bloom_strength = 0.015f});

	const float sponza_scale = 2.0f;
	const glm::vec3 root_translation = glm::vec3{0.0f, 0.0f, -50.0f};
	auto sponzaPos = [&](glm::vec3 local) {
		return local * sponza_scale + root_translation;
	};

	placeModel({
		.gltf_path = paths.sponza_model().lexically_normal(),
		.translation = root_translation,
		.scale = glm::vec3(sponza_scale),
	});

	m_birds_orbit_center = root_translation + glm::vec3{0.0f, 0.0f, 20.0f} * sponza_scale;
	placeModel({
		.gltf_path = paths.birds_model.lexically_normal(),
		.translation = m_birds_orbit_center,
		.scale = glm::vec3(sponza_scale),
		.on_loaded = [this](Entity birds) {
			m_registry.setName(birds, "birds");
			m_birds = birds;
		}
	});

	auto makeLight = [&](float intensity, float radius, glm::vec3 color, const std::string& name,
	                     glm::vec3 pos, bool rotates, bool casts_shadow,
	                     glm::vec3 light_scale = glm::vec3(-1.0f)) -> Entity {
		Entity e = m_registry.createPointLight(intensity, radius, color);
		m_registry.setName(e, name);
		auto* tc = m_registry.getComponent<TransformComponent>(e);
		tc->setTranslation(pos);
		if (light_scale.x >= 0.0f)
			tc->setScale(light_scale);
		auto* pl = m_registry.getComponent<PointLightComponent>(e);
		pl->setRotates(rotates);
		pl->setCastsShadow(casts_shadow);
		return e;
	};

    // Moonlight
	{
		Entity dl = m_registry.createDirectionalLight(3.5f, glm::vec3(0.65f, 0.75f, 1.0f),
			glm::normalize(glm::vec3(0.23f, -0.25f, -0.94f)));
		m_registry.setName(dl, "Moonlight");
		m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);
	}

	ResourceHandle<VeTexture> fire_tex;
	ResourceHandle<VeTexture> smoke_tex;
	if (!paths.fire_texture.empty())
		fire_tex = m_resource_manager.load<VeTexture>(ve::pathToUtf8Generic(paths.fire_texture.lexically_normal()));
	if (!paths.smoke_texture.empty())
		smoke_tex = m_resource_manager.load<VeTexture>(ve::pathToUtf8Generic(paths.smoke_texture.lexically_normal()));

	auto attachFireEmitters = [&](Entity light) {
		{
			EmitterParams p{};
			p.color_start = glm::vec4(1.0f, 0.85f, 0.4f, 1.0f);
			p.color_end   = glm::vec4(1.0f, 0.2f, 0.02f, 0.0f);
			p.gravity     = -1.0f;
			p.drag        = 4.5f;
			p.stddev      = 0.4f;
			p.min_life    = 0.5f;
			p.max_life    = 1.2f;
			auto& ec = m_registry.addComponent<ParticleEmitterComponent>(light);
			ec.params = p;
			ec.texture = fire_tex;
			ec.rate = 45.0f;
			ec.scale = 0.2f;
		}
		{
			Entity smoke = m_registry.createEntity(m_registry.getName(light) + " Smoke");
			m_registry.addComponent<TransformComponent>(smoke);
			m_registry.setParent(smoke, light);
			EmitterParams p{};
			p.color_start = glm::vec4(0.7f, 0.65f, 0.6f, 0.9f);
			p.color_end   = glm::vec4(0.25f, 0.25f, 0.25f, 0.4f);
			p.gravity     = -0.3f;
			p.drag        = 4.8f;
			p.stddev      = 0.4f;
			p.min_life    = 3.0f;
			p.max_life    = 5.0f;
			p.atlas_one_shot = 1u;
			auto& ec = m_registry.addComponent<ParticleEmitterComponent>(smoke);
			ec.params = p;
			ec.texture = smoke_tex;
			ec.rate = 8.0f;
		}
	};

	const glm::vec3 fire_positions[4] = {
		sponzaPos({-4.96f, -1.16f,  1.12f}),
		sponzaPos({-4.96f,  1.736f, 1.12f}),
		sponzaPos({ 3.912f, 1.736f, 1.12f}),
		sponzaPos({ 3.912f, -1.16f, 1.12f}),
	};
	for (int i = 0; i < 4; ++i) {
		Entity e = makeLight(FIRE_INTENSITY, 1.0f, glm::vec3(1.0f, .45f, .15f),
			"Fire " + std::to_string(i + 1), fire_positions[i], false, false, glm::vec3(0.1f));
		m_fire_lights.push_back(e);
		attachFireEmitters(e);
	}

    // lion eyes
	makeLight(2.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), "Green eye (left)",
		sponzaPos({10.136f, 0.116f,  1.504f}), false, false,
		glm::vec3(0.03f));
	makeLight(2.0f, 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), "Green eye (right)",
		sponzaPos({10.136f, 0.4864f, 1.504f}), false, false,
		glm::vec3(0.03f));
}

void SponzaScene::update(float dt) {
	VeScene::update(dt);

	// Flickering fire lights
	m_fire_t += dt;
	for (size_t i = 0; i < m_fire_lights.size(); ++i) {
		auto* pl = m_registry.getComponent<PointLightComponent>(m_fire_lights[i]);
		if (!pl)
			continue;
		float phase = 2.1f * static_cast<float>(i);
		float f = 0.14f * std::sin(m_fire_t * 7.3f + phase)
		        + 0.07f * std::sin(m_fire_t * 13.1f + 1.7f * phase)
		        + 0.05f * std::sin(m_fire_t * 23.7f + 2.9f * phase);
		pl->setIntensity(FIRE_INTENSITY * (1.0f + f));
	}

	if (m_birds.isNull())
		return;
	auto* tc = m_registry.getComponent<TransformComponent>(m_birds);
	if (!tc)
		return;

	m_birds_t += dt;
	const float lobe_aspect = 0.4f;
	const float t = m_birds_t * m_birds_orbit_speed;
	const float c = std::cos(t);
	const float s = std::sin(t);
	const float px = c;
	const float py = lobe_aspect * s * c;
	const float vx = -s;
	const float vy = lobe_aspect * (c * c - s * s);
	tc->setTranslation(m_birds_orbit_center + glm::vec3{px * m_birds_orbit_radius, py * m_birds_orbit_radius, 0.0f});
	tc->setRotationEuler({0.0f, 0.0f, std::atan2(vy, vx) - glm::half_pi<float>()});
}

} // namespace ve