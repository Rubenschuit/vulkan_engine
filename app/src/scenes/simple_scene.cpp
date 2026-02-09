#include "simple_scene.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_component.hpp"
#include <glm/gtc/constants.hpp>
#include <unordered_map>

namespace ve {

SimpleScene::SimpleScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& /*pool*/, VeDescriptorSetLayout& /*material_layout*/, const AssetPaths& paths, vk::raii::DescriptorSet* shared_particle_descriptor_set)
	: VeScene(device, "Simple Scene"), m_shared_particle_descriptor_set(shared_particle_descriptor_set) {
	assert(m_shared_particle_descriptor_set && "Shared particle descriptor set must not be null");
	loadGameObjects(resource_manager, paths);
}

void SimpleScene::loadGameObjects(VeResourceManager& resource_manager, const AssetPaths& paths) {
    // stationary light
    {
        auto l = VeGameObject::createPointLight(100.0f, 2.0f, glm::vec3(1.0f, 1.0f, 1.0f));
        glm::vec3 pos = {0.0f, 0.0f, 20.0f};
        l.getComponent<TransformComponent>()->translation = pos;
        l.getComponent<PointLightComponent>()->rotates = false;
        m_game_objects.emplace(l.getId(), std::move(l));
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
        glm::vec3 pos = {
            pos_radius * cos(glm::two_pi<float>() / num_lights * (float)i),
            pos_radius * sin(glm::two_pi<float>() / num_lights * (float)i),
            height
        };
        point_light.getComponent<PointLightComponent>()->casts_shadow = false;
        point_light.getComponent<TransformComponent>()->translation = pos;
        m_game_objects.emplace(point_light.getId(), std::move(point_light));
    }

    // floor
    {
        auto floor = VeModel::loadAsSingleObject(resource_manager,
            paths.quad_model,
            {0.0f, 0.0f, 0.0f}, {glm::radians(90.0f), 0.0f, 0.0f}, {80.0f, 1.0f, 80.0f});
        if (auto* mesh = floor.getComponent<MeshComponent>()) {
            mesh->has_texture = 0.0f;
            mesh->has_shadow = false;
            m_game_objects.emplace(floor.getId(), std::move(floor));
        }
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
                transform->translation = base_translation + glm::vec3{(float)i * spacing_x, (float)j * spacing_y, z_offset};
                transform->rotation = base_rotation;
                transform->scale = base_scale;
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

