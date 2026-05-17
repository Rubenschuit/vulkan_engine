#include "fireworks.hpp"
#include "events/engine_events.hpp"
#include "scene/scene_manager.hpp"
#include "scene/ve_registry.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_random.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <algorithm>

namespace ve::effects {

Fireworks::Fireworks(ParticleBackend& backend,
                     VeResourceManager& resources,
                     SceneManager& scene_manager,
                     EventBus& event_bus,
                     std::filesystem::path fire_atlas_path,
                     std::filesystem::path smoke_atlas_path,
                     FireworksConfig config)
	: m_backend(backend), m_scene_manager(scene_manager), m_event_bus(event_bus),
	  m_config(std::move(config)) {

	m_scene_unloaded_sub = m_event_bus.subscribe<SceneUnloadedEvent>(
		[this](const SceneUnloadedEvent& e) { onSceneUnloaded(e); });

	// Load and register atlas textures via the engine's bindless particle array.
	// If a path is empty, the sub-emitter falls back to the procedural round mask.
	if (!fire_atlas_path.empty()) {
		m_fire_atlas = resources.load<VeTexture>(fire_atlas_path.lexically_normal().generic_string());
		m_fire_atlas_slot = m_backend.registerAtlas(m_fire_atlas);
	}
	if (!smoke_atlas_path.empty()) {
		m_smoke_atlas = resources.load<VeTexture>(smoke_atlas_path.lexically_normal().generic_string());
		m_smoke_atlas_slot = m_backend.registerAtlas(m_smoke_atlas);
	}

	auto base = []() {
		EmitterParams t{};
		t.gravity = 0.0f;
		t.drag = 0.0f;
		t.floor_z = -1.0e30f;
		return t;
	};

	{
		EmitterParams trail = base();
		trail.drag = m_config.trail_drag;
		trail.color_start = glm::vec4(1.0f);
		trail.color_end   = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
		trail.atlas_index = ROUND_MASK_SENTINEL;       // procedural soft glow
		trail.min_life = m_config.trail_life_min;
		trail.max_life = m_config.trail_life_max;
		trail.spawn_scale = 0.0f;
		trail.spawn_velocity_scale = 0.1f;
		// 0 disables the proportional path in the spawn shader and falls back to min/max_life.
		trail.spawn_life_fraction = m_config.trail_life_proportional ? m_config.trail_life_fraction : 0.0f;
		// Trail particles are spawned via spawnChild (sub-emitter chaining), not
		// the spawn pre-pass, so spawn_distribution / mean / stddev are unused here.
		m_trail_emitter = m_backend.registerEmitter(trail);
	}
	{
		EmitterParams smoke = base();
		smoke.gravity = -2.0f;
		smoke.drag = 0.9f;
		smoke.atlas_one_shot = 1u;
		smoke.color_start = glm::vec4(0.7f, 0.7f, 0.7f, 0.9f);
		smoke.color_end   = glm::vec4(0.4f, 0.4f, 0.4f, 0.0f);
		smoke.atlas_index = m_smoke_atlas_slot;        // sampled atlas or sentinel
		smoke.min_life = 5.0f;
		smoke.max_life = 7.5f;
		smoke.spawn_scale = 5.5f;
		smoke.spawn_velocity_scale = 0.02f;
		smoke.spawn_life_fraction = 0.0f;
		smoke.mean = 0.0f;
		smoke.stddev = 0.25f;
		m_smoke_emitter = m_backend.registerEmitter(smoke);
	}
	{
		EmitterParams spark = base();
		spark.drag = 0.8f;
		spark.color_start = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f);
		spark.color_end   = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		spark.atlas_index = m_fire_atlas_slot;
		spark.min_life = 0.5f;
		spark.max_life = 1.0f;

		spark.mean = 0.0f;
		spark.stddev = 2.5f;
		m_spark_emitter = m_backend.registerEmitter(spark);
	}
	{
		EmitterParams streamer = base();
		streamer.drag = m_config.streamer_drag;
		streamer.color_start = glm::vec4(1.0f);
		streamer.color_end   = glm::vec4(1.0f); // identity: preserve rocket color
		streamer.atlas_index = ROUND_MASK_SENTINEL;
		streamer.trail_interval = m_config.trail_interval;
		streamer.on_tick_emitter_id  = static_cast<int32_t>(m_trail_emitter);
		streamer.tick_probability    = 1.0f;
		streamer.on_tick2_emitter_id = static_cast<int32_t>(m_smoke_emitter);
		streamer.tick2_probability   = 0.4f;
		streamer.min_life = m_config.streamer_life_min;
		streamer.max_life = m_config.streamer_life_max;
		streamer.brightness = m_config.streamer_brightness;
		streamer.spawn_distribution = SpawnDistribution::UniformSphere;
		streamer.mean = m_config.streamer_burst_speed;
		streamer.stddev = m_config.streamer_burst_spread;
		m_streamer_emitter = m_backend.registerEmitter(streamer);
	}
	{
		EmitterParams rocket = base();
		rocket.color_start = glm::vec4(1.0f);
		rocket.color_end   = glm::vec4(1.0f);
		rocket.atlas_index = m_fire_atlas_slot;
		rocket.min_life = 2.0f;
		rocket.max_life = 7.0f;
		rocket.mean = 0.0f;
		rocket.stddev = 0.0f;
		m_rocket_emitter = m_backend.registerEmitter(rocket);
	}
	{
		EmitterParams flash = base();
		flash.color_start = glm::vec4(1.0f);
		flash.color_end   = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
		flash.atlas_index = m_fire_atlas_slot;
		flash.brightness  = 4.0f;
		flash.min_life = 0.15f;
		flash.max_life = 0.15f;
		flash.mean = 0.0f;
		flash.stddev = 0.0f;
		flash.scale_end = 0.0f; // lerp scale over life
		m_flash_emitter = m_backend.registerEmitter(flash);
	}
}

Fireworks::~Fireworks() {
	m_event_bus.unsubscribe<SceneUnloadedEvent>(m_scene_unloaded_sub);

	if (Registry* reg = m_scene_manager.getActiveRegistry()) {
		for (const TransientLight& tl : m_lights)
			if (reg->isAlive(tl.entity))
				reg->destroyEntity(tl.entity);
	}
	m_lights.clear();

	for (EmitterId id : {m_flash_emitter, m_rocket_emitter, m_streamer_emitter, m_spark_emitter, m_smoke_emitter, m_trail_emitter}) {
		if (id != INVALID_EMITTER)
			m_backend.releaseEmitter(id);
	}
	if (m_fire_atlas_slot != ROUND_MASK_SENTINEL)
		m_backend.releaseAtlas(m_fire_atlas_slot);
	if (m_smoke_atlas_slot != ROUND_MASK_SENTINEL)
		m_backend.releaseAtlas(m_smoke_atlas_slot);
}

void Fireworks::onSceneUnloaded(const SceneUnloadedEvent&) {
	m_rockets.clear();
	m_lights.clear();
}

void Fireworks::drawConfigUI() {
	FireworksConfig& c = m_config;

	if (ImGui::CollapsingHeader("Launch", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Vel min", &c.launch_vel_min, 0.0f, 200.0f, "%.1f");
		ImGui::SliderFloat("Vel max", &c.launch_vel_max, 0.0f, 200.0f, "%.1f");
		if (c.launch_vel_max < c.launch_vel_min) c.launch_vel_max = c.launch_vel_min;
		ImGui::SliderFloat("Spread XY", &c.launch_spread_xy, 0.0f, 50.0f, "%.1f");
		ImGui::SliderInt("Rockets per launch", &c.launch_count, 1, 20);
	}

	if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Random color", &c.use_random_color);
		ImGui::BeginDisabled(c.use_random_color);
		ImGui::ColorEdit3("Particle color", &c.particle_color.x);
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Explosion", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderInt("Streamers (min)", &c.explosion_particle_count, 10, 500);
		ImGui::SliderFloat("Streamer size", &c.explosion_size, 0.1f, 5.0f, "%.2f");
	}

	if (ImGui::CollapsingHeader("Streamer")) {
		ImGui::SliderFloat("Drag", &c.streamer_drag, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat("Life min (s)", &c.streamer_life_min, 0.1f, 15.0f, "%.2f");
		ImGui::SliderFloat("Life max (s)", &c.streamer_life_max, 0.1f, 15.0f, "%.2f");
		ImGui::SliderFloat("Burst speed", &c.streamer_burst_speed, 0.0f, 200.0f, "%.1f");
		ImGui::SliderFloat("Burst spread", &c.streamer_burst_spread, 0.0f, 50.0f, "%.1f");
		ImGui::SliderFloat("Brightness", &c.streamer_brightness, 0.0f, 150.0f, "%.2f");
	}

	if (ImGui::CollapsingHeader("Trails")) {
		ImGui::SliderFloat("Trail interval (s)", &c.trail_interval, 0.001f, 0.1f, "%.4f");
		ImGui::SliderFloat("Trail drag", &c.trail_drag, 0.0f, 2.0f, "%.2f");
		ImGui::Checkbox("Life proportional to streamer", &c.trail_life_proportional);
		if (c.trail_life_proportional) {
			ImGui::SliderFloat("Trail life fraction", &c.trail_life_fraction, 0.0f, 3.0f, "%.2f");
		} else {
			ImGui::SliderFloat("Trail life min (s)", &c.trail_life_min, 0.1f, 15.0f, "%.2f");
			ImGui::SliderFloat("Trail life max (s)", &c.trail_life_max, 0.1f, 15.0f, "%.2f");
			if (c.trail_life_max < c.trail_life_min) c.trail_life_max = c.trail_life_min;
		}
	}

	if (ImGui::CollapsingHeader("Smoke")) {
		ImGui::ColorEdit4("Smoke color", &c.smoke_color.x);
	}

	if (ImGui::CollapsingHeader("Flash")) {
		ImGui::SliderFloat("Peak scale", &c.flash_peak_scale, 0.0f, 500.0f, "%.1f");
		ImGui::SliderFloat("Peak intensity (light)", &c.flash_peak_intensity, 0.0f, 20000.0f, "%.0f");
		ImGui::SliderFloat("Light life (s)", &c.flash_life, 0.0f, 2.0f, "%.2f");
	}

	if (ImGui::CollapsingHeader("Environment")) {
		ImGui::DragFloat3("Wind direction", &c.wind_direction.x, 0.05f, -1.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Wind strength", &c.wind_strength, 0.0f, 30.0f, "%.2f");
		ImGui::SliderFloat("Gravity", &c.gravity, 0.0f, 40.0f, "%.2f");
	}
}

void Fireworks::launchRocket() {
	for (int i = 0; i < m_config.launch_count; i++) {
		glm::vec3 pos = m_config.launch_pos;
		float speed = Random::floatRange(m_config.launch_vel_min, m_config.launch_vel_max);
		float spread_x = Random::floatRange(-m_config.launch_spread_xy, m_config.launch_spread_xy);
		float spread_y = Random::floatRange(-m_config.launch_spread_xy, m_config.launch_spread_xy);
		glm::vec3 vel = glm::vec3(spread_x, spread_y, speed);
		glm::vec4 color = m_config.use_random_color ? Random::colorHSV() : m_config.particle_color;
		launchRocket(pos, vel, color);
	}
}

void Fireworks::launchRocket(glm::vec3 pos, glm::vec3 vel, glm::vec4 color) {
	Rocket r{
		.pos = pos,
		.vel = vel,
		.color = color,
		.timer = Random::floatRange(2.0f, 7.0f),
		.trail_timer = Random::floatRange(0.0f, m_config.trail_interval),
	};
	m_rockets.push_back(r);

	SpawnEvent event{
		.position_scale = glm::vec4(pos, 0.0f),
		.velocity_life = glm::vec4(vel, r.timer),
		.color = color,
		.count = 1,
		.emitter_id = m_rocket_emitter,
	};
	m_backend.emitParticles(event);
}

void Fireworks::update(float dt, float total_time) {
	if (m_rocket_emitter   == INVALID_EMITTER ||
	    m_streamer_emitter == INVALID_EMITTER ||
	    m_trail_emitter    == INVALID_EMITTER ||
	    m_spark_emitter    == INVALID_EMITTER ||
	    m_smoke_emitter    == INVALID_EMITTER ||
	    m_flash_emitter    == INVALID_EMITTER)
		return;

	Registry* reg = m_scene_manager.getActiveRegistry();

	// lights
	for (size_t i = 0; i < m_lights.size(); ) {
		TransientLight& tl = m_lights[i];
		tl.life -= dt;
		bool alive = reg && reg->isAlive(tl.entity);
		if (!alive || tl.life <= 0.0f) {
			if (alive)
				reg->destroyEntity(tl.entity);
			m_lights[i] = m_lights.back();
			m_lights.pop_back();
			continue;
		}
		float frac = tl.life / tl.max_life;
		if (auto* pl = reg->getComponent<PointLightComponent>(tl.entity))
			pl->setIntensity(tl.peak_intensity * frac * frac);
		++i;
	}

	glm::vec3 dir = glm::length(m_config.wind_direction) > 0.001f
		? glm::normalize(m_config.wind_direction)
		: glm::vec3(1.0f, 0.0f, 0.0f);
	glm::vec4 wind_v4 = glm::vec4(dir, m_config.wind_strength);
	for (EmitterId id : {m_rocket_emitter, m_streamer_emitter, m_trail_emitter, m_spark_emitter}) {
		EmitterParams& p = m_backend.getEmitterParams(id);
		p.wind = wind_v4;
		p.gravity = m_config.gravity;
	}
	m_backend.getEmitterParams(m_smoke_emitter).wind = wind_v4;

	{
		EmitterParams& t = m_backend.getEmitterParams(m_trail_emitter);
		t.drag = m_config.trail_drag;
		t.spawn_life_fraction = m_config.trail_life_proportional ? m_config.trail_life_fraction : 0.0f;
		t.min_life = m_config.trail_life_min;
		t.max_life = m_config.trail_life_max;
	}
	{
		EmitterParams& s = m_backend.getEmitterParams(m_streamer_emitter);
		s.trail_interval = m_config.trail_interval;
		s.drag = m_config.streamer_drag;
		s.min_life = m_config.streamer_life_min;
		s.max_life = m_config.streamer_life_max;
		s.mean = m_config.streamer_burst_speed;
		s.stddev = m_config.streamer_burst_spread;
		s.brightness = m_config.streamer_brightness;
	}

	glm::vec3 wind_velocity = dir * m_config.wind_strength;

	for (size_t i = 0; i < m_rockets.size(); ) {
		Rocket& r = m_rockets[i];

		r.pos += wind_velocity * dt;
		r.vel.z -= m_config.gravity * dt;
		r.pos += r.vel * dt;

		float spiral_factor = glm::clamp(r.vel.z, 0.0f, 10.0f);
		float spiral_speed = glm::clamp(r.vel.z, 0.0f, 5.0f);
		float rocket_seed = r.color.r * 1000.0f;
		r.pos.x += glm::cos(total_time * spiral_speed + rocket_seed) * spiral_factor * dt;
		r.pos.y += glm::sin(total_time * spiral_speed + rocket_seed) * spiral_factor * dt;

		r.timer -= dt;
		r.trail_timer -= dt;

		if (r.trail_timer <= 0.0f) {
			r.trail_timer += m_config.trail_interval;

			// Per-particle position jitter
			glm::vec3 smoke_pos = r.pos + Random::vec3Normal(0.5f);
			SpawnEvent smoke{
				.position_scale = glm::vec4(smoke_pos, 5.0f),
				.velocity_life = glm::vec4(r.vel * 0.02f, 7.0f),
				.color = m_config.smoke_color,
				.count = 2,
				.emitter_id = m_smoke_emitter,
			};
			m_backend.emitParticles(smoke);

			SpawnEvent spark{
				.position_scale = glm::vec4(r.pos, 0.2f),
				.velocity_life = glm::vec4(0.0f, 0.0f, 0.0f, 0.34f),
				.color = glm::vec4(1.0f, 0.8f, 0.1f, 1.0f),
				.count = 5,
				.emitter_id = m_spark_emitter,
			};
			m_backend.emitParticles(spark);
		}

		// Firework explosion
		bool dead = (r.timer <= 0.0f) || (r.vel.z < -5.0f);
		if (dead) {
			uint32_t streamer_count = static_cast<uint32_t>(
				Random::intRange(m_config.explosion_particle_count, m_config.explosion_particle_count * 2));

			SpawnEvent streamers{
				.position_scale = glm::vec4(r.pos, m_config.explosion_size),
				.velocity_life = glm::vec4(0.0f), // life=0 -> spawn shader randomizes from emitter min/max_life
				.color = r.color,
				.count = streamer_count,
				.emitter_id = m_streamer_emitter,
			};
			m_backend.emitParticles(streamers);

			// Rocket apex flash
			SpawnEvent flash{
				.position_scale = glm::vec4(r.pos, m_config.flash_peak_scale),
				.velocity_life  = glm::vec4(0.0f, 0.0f, 0.0f, 0.15f),
				.color          = r.color,
				.count          = 1,
				.emitter_id     = m_flash_emitter,
			};
			m_backend.emitParticles(flash);

			// Transient point light
			if (reg && m_config.flash_peak_intensity > 0.0f && m_config.flash_life > 0.0f) {
				glm::vec3 light_color = glm::vec3(r.color);
				Entity e = reg->createPointLight(m_config.flash_peak_intensity, 1.0f, light_color);
				reg->setName(e, "Firework Flash");
				if (auto* tc = reg->getComponent<TransformComponent>(e))
					tc->setTranslation(r.pos);
				if (auto* pl = reg->getComponent<PointLightComponent>(e))
					pl->setCastsShadow(false);
				m_lights.push_back({e, m_config.flash_life, m_config.flash_life, m_config.flash_peak_intensity});
			}

			m_rockets[i] = m_rockets.back();
			m_rockets.pop_back();
		} else {
			++i;
		}
	}
}

} // namespace ve::effects
