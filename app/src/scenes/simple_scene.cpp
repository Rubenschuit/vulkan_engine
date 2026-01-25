#include "simple_scene.hpp"
#include "game/ve_model.hpp"
#include <glm/gtc/constants.hpp>

namespace ve {

SimpleScene::SimpleScene(VeDevice& device, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const std::filesystem::path& project_root)
    : VeScene(device, "Simple Scene") {
    loadTextures(project_root);
    createDescriptorSet(pool, material_layout);
    loadGameObjects(project_root);
}

// Loads textures for particles, TODO: consider moving these from simple scene.
void SimpleScene::loadTextures(const std::filesystem::path& project_root) {
    m_glow_texture = std::make_unique<VeTexture>(m_device, project_root / "textures" / "light.png");
    m_fire_texture = std::make_unique<VeTexture>(m_device, project_root / "textures" / "fire_ball.ktx");
    m_smoke_texture = std::make_unique<VeTexture>(m_device, project_root / "textures" / "smoke_atlas.png");
}

void SimpleScene::createDescriptorSet(VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout) {
    auto glow_texture_info = m_glow_texture->getDescriptorInfo();
    auto fire_texture_info = m_fire_texture->getDescriptorInfo();
    auto smoke_texture_info = m_smoke_texture->getDescriptorInfo();

    m_texture_descriptor_set = vk::raii::DescriptorSet{nullptr};
    VeDescriptorWriter(material_layout, pool)
        .writeImage(0, &glow_texture_info)
        .writeImage(1, &fire_texture_info)
        .writeImage(2, &smoke_texture_info)
        .build(m_texture_descriptor_set);
}

void SimpleScene::loadGameObjects(const std::filesystem::path& project_root) {
    // stationary light
    {
        auto l = VeGameObject::createPointLight(100.0f, 2.0f, glm::vec3(1.0f, 1.0f, 1.0f));
        glm::vec3 pos = {0.0f, 0.0f, 20.0f};
        l.transform.translation = pos;
        l.point_light_component->rotates = false;
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
        point_light.point_light_component->casts_shadow = false;
        point_light.has_shadow = false;
        point_light.transform.translation = pos;
        m_game_objects.emplace(point_light.getId(), std::move(point_light));
    }

    // 'black hole' light
    {
        auto black_hole = VeGameObject::createPointLight(1.0f, 4.0f, glm::vec3(0.0f, 0.0f, 0.0f));
        glm::vec3 pos = {0.0f, -300.0f, 10.0f};
        black_hole.transform.translation = pos;
        black_hole.point_light_component->rotates = false;
        black_hole.point_light_component->casts_shadow = false;
        black_hole.has_shadow = false;
        m_game_objects.emplace(black_hole.getId(), std::move(black_hole));
    }

    // floor
    {
        VeGameObject floor = VeGameObject::createGameObject();
        auto quad = std::make_shared<VeModel>(m_device, project_root / "models" / "quad.gltf");
        floor.ve_model = quad;
        floor.has_texture = 0.0f;
        floor.transform = {
            .translation = {0.0f, 0.0f, 0.0f},
            .rotation = {glm::radians(90.0f), 0.0f, 0.0f},
            .scale = {80.0f, 1.0f, 80.0f}
        };
        floor.has_shadow = false;
        m_game_objects.emplace(floor.getId(), std::move(floor));
    }

    // Spheres in a grid
    {
        std::shared_ptr<VeModel> model = std::make_shared<VeModel>(m_device, project_root / "models" / "sphere" / "scene.gltf");
        for (int j = 0; j < 5; j++) {
            for (int i = 0; i < 5; i++) {
                VeGameObject obj = VeGameObject::createGameObject();
                obj.ve_model = model;
                obj.transform.rotation = {0.0f, 0.0f, 0.0f};
                obj.transform.translation = {(float)i * 4.0f, (float)j * 4.0f, 1.0f};
                obj.transform.scale = {1.0f, 1.0f, 1.0f};
                obj.has_texture = 0.0f;
                m_game_objects.emplace(obj.getId(), std::move(obj));
            }
        }
    }

    // Cubes in a grid
    {
        std::shared_ptr<VeModel> model2 = std::make_shared<VeModel>(m_device, project_root / "models" / "cube.gltf");
        for (int j = 0; j < 5; j++) {
            for (int i = 0; i < 5; i++) {
                VeGameObject obj = VeGameObject::createGameObject();
                obj.ve_model = model2;
                obj.transform.translation = {-1.0 * (float)i * 4.0f - 4.0f, (float)j * 4.0f, 1.0f};
                obj.transform.scale = {1.0f, 1.0f, 1.0f};
                obj.has_texture = 0.0f;
                m_game_objects.emplace(obj.getId(), std::move(obj));
            }
        }
    }

    // Flat vases (bad normals) in a grid
    {
        std::shared_ptr<VeModel> model3 = std::make_shared<VeModel>(m_device, project_root / "models" / "flat_vase.gltf");
        for (int j = 0; j < 5; j++) {
            for (int i = 0; i < 5; i++) {
                VeGameObject obj = VeGameObject::createGameObject();
                obj.ve_model = model3;
                obj.transform = {
                    .translation = {-1.0 * (float)i * 4.0f - 4.0f, (float)j * -4.0f - 4.0f, 0.f},
                    .rotation = {glm::radians(-180.0f), 0.0f, 0.0f},
                    .scale = {6.0f, 6.0f, 3.0f}
                };
                obj.has_texture = 0.0f;
                m_game_objects.emplace(obj.getId(), std::move(obj));
            }
        }
    }

    // Smooth vases (interpolated normals) in a grid
    {
        std::shared_ptr<VeModel> model4 = std::make_shared<VeModel>(m_device, project_root / "models" / "smooth_vase.gltf");
        for (int j = 0; j < 5; j++) {
            for (int i = 0; i < 5; i++) {
                VeGameObject obj = VeGameObject::createGameObject();
                obj.ve_model = model4;
                obj.transform = {
                    .translation = {(float)i * 4.0f , (float)j * -4.0f - 4.0f, 0.f},
                    .rotation = {glm::radians(-180.0f), 0.0f, 0.0f},
                    .scale = {6.0f, 6.0f, 3.0f}
                };
                m_game_objects.emplace(obj.getId(), std::move(obj));
            }
        }
    }
}

} // namespace ve

