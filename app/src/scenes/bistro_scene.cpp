#include "bistro_scene.hpp"
#include "fox.hpp"

#include <string>

namespace ve {

BistroScene::BistroScene(const SceneContext& ctx, const AssetPaths& paths)
	: VeScene(ctx, "Bistro Scene"), m_overlay_path(paths.bistro_scene_overlay()) {

	// Directional light
	Entity dl = m_registry.createDirectionalLight(5.0f, glm::vec3(1.0f),
		glm::normalize(glm::vec3(-1.0f, -0.5f, -2.5f)));
	m_registry.setName(dl, "Directional Light");
	m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);

	placeModel({
		.gltf_path = paths.bistro_model().lexically_normal(),
		.scale = {2.0f, 2.0f, 2.0f},
		.flip_tex_coord_v = true,
		.on_loaded = [this](Entity wrapper) {
			m_registry.setName(wrapper, "Bistro Model");
			// Curated lighting setup
			if (auto ov = SceneOverlay::loadFromFile(m_overlay_path))
				ov->apply(m_registry);
		}
	});

	// Pigeons scattered across the rooftops
	struct PigeonSpawn {
		glm::vec3 pos;
		float yaw_deg;
		float scale;
		float anim_speed;
		float anim_phase;
	};
	const PigeonSpawn pigeons[] = {
		{{  87.5f, -49.25f,  9.4f},   25.0f, 1.2f, 1.00f, 0.0f},
		{{  12.5f, -27.5f, 38.7f},  160.0f, 1.0f, 0.90f, 1.7f},
		{{ -52.5f, -32.5f, 34.2f},  -70.0f, 1.4f, 1.10f, 0.6f},
		{{ -42.5f,  12.5f, 37.7f},  210.0f, 1.8f, 0.95f, 2.9f},
		{{ -72.5f,  -7.5f, 31.2f},   95.0f, 1.1f, 1.05f, 1.1f},
		{{  92.5f, -97.5f, 34.2f},  -30.0f, 1.3f, 0.85f, 3.6f},
		{{ -8.42f,  -32.1f, 28.54f},  -14.0f, 0.9f, 1.00f, 0.3f},
		{{ -7.96f,  -32.29f, 28.54f},  -14.0f, 0.95f, 1.00f, 2.1f},
		{{ -8.23f,  -32.22f, 28.54f},  -14.0f, 1.1f, 1.00f, 3.5f},
		{{  17.5f, -77.5f, 38.0f},   60.0f, 1.0f, 1.08f, 2.2f},
	};
	int pigeon_num = 0;
	for (const PigeonSpawn& p : pigeons) {
		int num = ++pigeon_num;
		placeModel({
			.gltf_path = paths.pigeon_model.lexically_normal(),
			.translation = p.pos,
			.rotation = {0.0f, 0.0f, glm::radians(p.yaw_deg)},
			.scale = glm::vec3(p.scale),
			.on_loaded = [this, num, speed = p.anim_speed, phase = p.anim_phase](Entity wrapper) {
				m_registry.setName(wrapper, "Pigeon " + std::to_string(num));
				if (auto* anim = m_registry.getComponent<AnimatorComponent>(wrapper)) {
					anim->setSpeed(0, speed);
					anim->setTime(0, phase);
				}
				if (auto* mesh = m_registry.getComponentInChildren<MeshComponent>(wrapper))
					mesh->editMaterialFactors([](MaterialFactors& f) { f.emissive_strength = 0.0f; });
			},
		});
	}

	placeModel(foxModelRequest(m_registry, paths.fox_model, {9.0f, -46.0f, 39.0f}));
}

}
