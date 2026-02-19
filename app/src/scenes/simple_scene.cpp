#include "simple_scene.hpp"
#include "resources/ve_model.hpp"
#include "resources/ve_material_properties.hpp"
#include "scene/ve_component.hpp"
#include <string>
#include <glm/gtc/constants.hpp>

namespace ve {

SimpleScene::SimpleScene(VeDevice& device, VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths, vk::raii::DescriptorSet* default_material_descriptor_set)
	: VeScene(device, "Simple Scene"), m_default_material_descriptor_set(default_material_descriptor_set) {
	assert(m_default_material_descriptor_set && "Default material descriptor set must not be null");
	loadGameObjects(resource_manager, pool, material_layout, paths);
}

void SimpleScene::setSunIntensity(float intensity) {
	auto* dl = m_registry.getComponent<DirectionalLightComponent>(m_sun);
	if (dl) dl->intensity = intensity;
}

float SimpleScene::getSunIntensity() const {
	const auto* dl = m_registry.getComponent<DirectionalLightComponent>(m_sun);
	return dl ? dl->intensity : DEFAULT_SUN_INTENSITY;
}

void SimpleScene::loadGameObjects(VeResourceManager& resource_manager, VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout, const AssetPaths& paths) {
	// sun light (directional)
	{
		Entity sun = m_registry.createDirectionalLight(DEFAULT_SUN_INTENSITY, glm::vec3(0.2f), glm::vec3(0.3f, 0.4f, -1.0f));
		m_registry.setName(sun, "Sun");
		m_registry.getComponent<DirectionalLightComponent>(sun)->casts_shadow = true;
		m_sun = sun;
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
		Entity light = m_registry.createPointLight(intensity, radius, colors[i % 10]);
		m_registry.setName(light, "Light " + std::to_string(i));
		glm::vec3 pos = {
			pos_radius * cos(glm::two_pi<float>() / num_lights * (float)i),
			pos_radius * sin(glm::two_pi<float>() / num_lights * (float)i),
			height
		};
		m_registry.getComponent<TransformComponent>(light)->setTranslation(pos);
		auto* pl = m_registry.getComponent<PointLightComponent>(light);
		pl->setCastsShadow(false);
		pl->setRotates(true);
	}

	// floor
	{
		auto floor_data = VeModel::loadSingleMesh(resource_manager, paths.quad_model);
		if (floor_data) {
			Entity e = m_registry.createEntity("floor");
			auto& tc = m_registry.addComponent<TransformComponent>(e);
			tc.setTranslation({0.0f, 0.0f, 0.0f});
			tc.setRotationEuler({glm::radians(-90.0f), 0.0f, 0.0f});
			tc.setScale({800.0f, 1.0f, 800.0f});
			auto& mc = m_registry.addComponent<MeshComponent>(e, floor_data->mesh, floor_data->material);
			mc.has_texture = 0.0f;
			mc.has_shadow = false;
		}
	}

	// Textured quad
	{
		auto quad_data = VeModel::loadSingleMesh(resource_manager, paths.quad_model.lexically_normal());
		if (quad_data) {
			auto default_dir = paths.quad_model.parent_path();
			auto mat_handle = resource_manager.createMaterial(
				"simple_scene::mots_quad",
				paths.mots_texture.lexically_normal(),
				default_dir / "default_normal.png",
				default_dir / "default_metallic_roughness.png",
				default_dir / "default_occlusion.png",
				default_dir / "default_emissive.png",
				MaterialAlphaProps{}, MaterialFactors{}, &pool, &material_layout);

			Entity quad_entity = m_registry.createEntity();
			auto& tc = m_registry.addComponent<TransformComponent>(quad_entity);
			tc.setTranslation({0.0f, 60.0f, 10.0f});
			tc.setScale({18.0f, 18.0f, 18.0f});
			auto& mc = m_registry.addComponent<MeshComponent>(quad_entity, quad_data->mesh, mat_handle);
			mc.has_texture = 1.0f;
			mc.has_shadow = false;
		}
	}

	// Helper to create grid instances from a single-mesh model
	auto addGridInstances = [&](const std::filesystem::path& model_path,
	                            int rows, int cols,
	                            glm::vec3 base_translation, glm::vec3 base_rotation, glm::vec3 base_scale,
	                            float spacing_x, float spacing_y, float z_offset) {
		auto mesh_data = VeModel::loadSingleMesh(resource_manager, model_path.lexically_normal());
		if (!mesh_data) return;

		for (int j = 0; j < rows; j++) {
			for (int i = 0; i < cols; i++) {
				Entity e = m_registry.createEntity();
				auto& tc = m_registry.addComponent<TransformComponent>(e);
				tc.setTranslation(base_translation + glm::vec3{(float)i * spacing_x, (float)j * spacing_y, z_offset});
				tc.setRotationEuler(base_rotation);
				tc.setScale(base_scale);
				auto& mc = m_registry.addComponent<MeshComponent>(e, mesh_data->mesh, mesh_data->material);
				mc.has_texture = 0.0f;
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
