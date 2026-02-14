#include "simple_scene.hpp"
#include "resources/ve_model.hpp"
#include <string>
#include "resources/ve_material_properties.hpp"
#include "scene/ve_component.hpp"
#include <glm/gtc/constants.hpp>
#include <unordered_map>

namespace ve {

SimpleScene::SimpleScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, vk::raii::DescriptorSet* default_material_descriptor_set)
	: VeScene(device, "Simple Scene"), m_default_material_descriptor_set(default_material_descriptor_set) {
	assert(m_default_material_descriptor_set && "Default material descriptor set must not be null");
	loadGameObjects(resource_manager, pool, material_layout, paths);
}

void SimpleScene::setSunIntensity(float intensity) {
	if (m_game_objects.contains(m_sun_id)) {
		auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
		if (pl) pl->intensity = intensity;
	}
}

float SimpleScene::getSunIntensity() const {
	if (m_game_objects.contains(m_sun_id)) {
		const auto* pl = m_game_objects.at(m_sun_id).getComponent<PointLightComponent>();
		return pl ? pl->intensity : DEFAULT_SUN_INTENSITY;
	}
	return DEFAULT_SUN_INTENSITY;
}

void SimpleScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths) {
    // sun light
    {
        auto l = VeGameObject::createPointLight(DEFAULT_SUN_INTENSITY, 2.0f, glm::vec3(1.0f, 1.0f, 1.0f));
        l.setName("Sun");
        glm::vec3 pos = {0.0f, 0.0f, 20.0f};
        l.getComponent<TransformComponent>()->setTranslation(pos);
        l.getComponent<PointLightComponent>()->rotates = false;
		l.getComponent<PointLightComponent>()->casts_shadow = true;
        m_sun_id = l.getId();
        m_game_objects.emplace(m_sun_id, std::move(l));
    }

    // Create some lights with ranging colors
    constexpr uint32_t num_lights = 7;
    constexpr float intensity = 100.0f;
    constexpr float radius = 1.0f;
    const glm::vec3 colors[10] = {
        {0.0f, 1.0f, 1.0f}, //cyan
        {1.0f, 0.0f, 0.0f}, //red
        {1.0f, 0.5f, 0.0f}, //orange
        {1.0f, 1.0f, 0.0f}, //yellow
        {0.0f, 1.0f, 0.0f}, //green
        {0.0f, 1.0f, 0.5f}, //turquoise
        {1.0f, 1.0f, 1.0f}, //white
        {0.0f, 0.5f, 1.0f}, //light-blue
        {0.0f, 0.0f, 1.0f}, //blue
        {0.5f, 0.0f, 1.0f}  //purple
    };
    constexpr float pos_radius = 28.0f;
    constexpr float height = 20.0f;

    for (uint32_t i = 0; i < num_lights; i += 1) {
        auto point_light = VeGameObject::createPointLight(intensity, radius, colors[i % 10]);
        point_light.setName("Light " + std::to_string(i));
        glm::vec3 pos = {
            pos_radius * cos(glm::two_pi<float>() / num_lights * (float)i),
            pos_radius * sin(glm::two_pi<float>() / num_lights * (float)i),
            height
        };
        point_light.getComponent<PointLightComponent>()->casts_shadow = false;
		point_light.getComponent<PointLightComponent>()->rotates = true;
        point_light.getComponent<TransformComponent>()->setTranslation(pos);
        m_game_objects.emplace(point_light.getId(), std::move(point_light));
    }

    // floor
    {
        auto floor = VeModel::loadAsSingleObject(resource_manager,
            paths.quad_model,
            {0.0f, 0.0f, 0.0f}, {glm::radians(-90.0f), 0.0f, 0.0f}, {80.0f, 1.0f, 80.0f});
        if (auto* mesh = floor.getComponent<MeshComponent>()) {
            mesh->has_texture = 0.0f;
            mesh->has_shadow = false;
            m_game_objects.emplace(floor.getId(), std::move(floor));
        }
    }

	// Textured quad
    {
		auto quad_template = VeModel::loadAsSingleObject(resource_manager, paths.quad_model.lexically_normal(),
		{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
		auto* quad_mesh_comp = quad_template.getComponent<MeshComponent>();
		auto mesh_handle = quad_mesh_comp->getMeshHandle();
		auto default_dir = paths.quad_model.parent_path();
		auto mat_handle = resource_manager.createMaterial(
			"simple_scene::mots_quad",
			paths.mots_texture.lexically_normal(),
			default_dir / "default_normal.png",
			default_dir / "default_metallic_roughness.png",
			default_dir / "default_occlusion.png",
			default_dir / "default_emissive.png",
			MaterialAlphaProps{}, MaterialFactors{}, &pool, &material_layout);
		VeGameObject quad_obj = VeGameObject::createGameObject();
		quad_obj.addComponent<MeshComponent>(mesh_handle, mat_handle);
		auto* quad_mesh = quad_obj.getComponent<MeshComponent>();
		quad_mesh->has_texture = 1.0f;
		quad_mesh->has_shadow = false;
		auto* quad_tr = quad_obj.getComponent<TransformComponent>();
		quad_tr->setTranslation({0.0f, -60.0f, 10.0f});
		quad_tr->setScale({8.0f, 8.0f, 8.0f});
		m_game_objects.emplace(quad_obj.getId(), std::move(quad_obj));
    }

    // Helper to create grid instances from a single-mesh model
    auto addGridInstances = [&](const std::filesystem::path& model_path,
                               int rows, int cols,
                               glm::vec3 base_translation, glm::vec3 base_rotation, glm::vec3 base_scale,
                               float spacing_x, float spacing_y, float z_offset) {
        auto template_obj = VeModel::loadAsSingleObject(resource_manager, model_path.lexically_normal(),
            {0, 0, 0}, {0, 0, 0}, {1, 1, 1});
        auto* mesh_comp = template_obj.getComponent<MeshComponent>();
        if (!mesh_comp) return;

        auto mesh_handle = mesh_comp->getMeshHandle();
        auto material_handle = mesh_comp->getMaterialHandle();
        mesh_comp->has_texture = 0.0f;

        for (int j = 0; j < rows; j++) {
            for (int i = 0; i < cols; i++) {
                VeGameObject obj = (i == 0 && j == 0) ? std::move(template_obj) : VeGameObject::createGameObject();
                if (i != 0 || j != 0) {
                    auto* m = obj.addComponent<MeshComponent>(mesh_handle, material_handle);
                    m->has_texture = 0.0f;
                }
                auto* transform = obj.getComponent<TransformComponent>();
                transform->setTranslation(base_translation + glm::vec3{(float)i * spacing_x, (float)j * spacing_y, z_offset});
                transform->setRotationEuler(base_rotation);
                transform->setScale(base_scale);
                m_game_objects.emplace(obj.getId(), std::move(obj));
            }
        }
    };

    // Spheres in a grid
    addGridInstances(paths.sphere_model, 5, 5,
                    {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 4.0f, 4.0f, 1.0f);

    // Cubes in a grid
    addGridInstances(paths.cube_model, 5, 5,
                    {-4.0f, 0, 0}, {0, 0, 0}, {1, 1, 1}, -4.0f, 4.0f, 1.0f);

    // Flat vases (bad normals) in a grid
    addGridInstances(paths.flat_vase_model, 5, 5,
                    {-4.0f, -4.0f, 0}, {glm::radians(-180.0f), 0.0f, 0.0f}, {6.0f, 6.0f, 3.0f}, -4.0f, -4.0f, 0);

    // Smooth vases (interpolated normals) in a grid
    addGridInstances(paths.smooth_vase_model, 5, 5, {0, -4.0f, 0}, {glm::radians(-180.0f), 0.0f, 0.0f}, {6.0f, 6.0f, 3.0f}, 4.0f, -4.0f, 0);
}

} // namespace ve

