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
		Entity sun = m_registry.createDirectionalLight(DEFAULT_SUN_INTENSITY, glm::vec3(0.2f), glm::vec3(0.6f, 0.7f, -1.0f));
		m_registry.setName(sun, "Sun");
		m_registry.getComponent<DirectionalLightComponent>(sun)->casts_shadow = true;
		m_sun = sun;
	}

	// Create some lights with ranging colors
	constexpr uint32_t num_lights = 10;
	constexpr float intensity = 300.0f;
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

	// floor (grid texture, slightly glossy dielectric, tiled ~1m intervals)
	{
		constexpr float half = 800.0f;
		constexpr float tile_uv = half * 2.0f;  // UV range so texture repeats every 1 world unit
		glm::vec3 n{0.0f, 0.0f, 1.0f};
		glm::vec4 t{1.0f, 0.0f, 0.0f, 1.0f};
		std::vector<VeMesh::Vertex> floor_verts = {
			{{-half, -half, 0.0f}, n, {0.0f, tile_uv}, t},
			{{ half,  half, 0.0f}, n, {tile_uv, 0.0f}, t},
			{{-half,  half, 0.0f}, n, {0.0f, 0.0f},    t},
			{{ half, -half, 0.0f}, n, {tile_uv, tile_uv}, t},
		};
		std::vector<uint32_t> floor_indices = {0, 1, 2, 0, 3, 1};
		auto floor_mesh = resource_manager.createMesh("simple_scene::floor_mesh", floor_verts, floor_indices);

		MaterialFactors floor_factors{};
		floor_factors.roughness_factor = 0.3f;
		floor_factors.metallic_factor = 0.0f;
		auto floor_mat = resource_manager.createMaterial(
			"simple_scene::floor",
			paths.grid_texture.lexically_normal(),
			"default_normal.png",
			"default_mr_unit.png",
			"default_occlusion.png",
			"default_emissive.png",
			MaterialAlphaProps{AlphaMode::ALPHA_OPAQUE, 0.5f, false},
			floor_factors, &pool, &material_layout);

		Entity e = m_registry.createEntity("floor");
		auto& tc = m_registry.addComponent<TransformComponent>(e);
		tc.setTranslation({0.0f, 0.0f, -0.05f});
		auto& mc = m_registry.addComponent<MeshComponent>(e, floor_mesh, floor_mat);
		mc.has_texture = 1.0f;
		mc.has_shadow = false;
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
				MaterialAlphaProps{AlphaMode::MASK, 0.5f, true}, MaterialFactors{}, &pool, &material_layout);

			Entity quad_entity = m_registry.createEntity();
			auto& tc = m_registry.addComponent<TransformComponent>(quad_entity);
			tc.setTranslation({0.0f, 60.0f, 40.0f});
			tc.setScale({18.0f, 18.0f, 18.0f});
			auto& mc = m_registry.addComponent<MeshComponent>(quad_entity, quad_data->mesh, mat_handle);
			mc.has_texture = 1.0f;
			mc.has_shadow = false;
		}
	}

	// PBR showcase grid: rows vary roughness (bottom=rough, top=smooth), columns vary metallic (left=dielectric, right=metal)
	auto createPbrGrid = [&](const std::filesystem::path& model_path,
	                         int rows, int cols,
	                         glm::vec3 base_translation, glm::vec3 base_rotation, glm::vec3 base_scale,
	                         float spacing_x, float spacing_y, float z_offset) {
		auto mesh_data = VeModel::loadSingleMesh(resource_manager, model_path.lexically_normal());
		if (!mesh_data)
			return;

		std::string stem = model_path.stem().string();
		for (int j = 0; j < rows; j++) {
			for (int i = 0; i < cols; i++) {
				float roughness = glm::max(1.0f - static_cast<float>(j) / static_cast<float>(rows - 1), 0.05f);
				float metallic = static_cast<float>(i) / static_cast<float>(cols - 1);

				MaterialFactors factors{};
				factors.roughness_factor = roughness;
				factors.metallic_factor = metallic;

				auto mat_handle = resource_manager.createMaterial(
					"pbr_grid::" + stem + "_r" + std::to_string(j) + "_m" + std::to_string(i),
					"default_albedo.png",
					"default_normal.png",
					"default_mr_unit.png",
					"default_occlusion.png",
					"default_emissive.png",
					MaterialAlphaProps{AlphaMode::ALPHA_OPAQUE, 0.5f, true},
					factors, &pool, &material_layout);

				Entity e = m_registry.createEntity();
				auto& tc = m_registry.addComponent<TransformComponent>(e);
				tc.setTranslation(base_translation + glm::vec3{(float)i * spacing_x, (float)j * spacing_y, z_offset});
				tc.setRotationEuler(base_rotation);
				tc.setScale(base_scale);
				auto& mc = m_registry.addComponent<MeshComponent>(e, mesh_data->mesh, mat_handle);
				mc.has_texture = 1.0f;
			}
		}
	};

	// Spheres in a grid (PBR sweep)
	createPbrGrid(paths.sphere_model, 5, 5,
	              {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 4.0f, 4.0f, 1.0f);

	// Cubes in a grid (PBR sweep)
	createPbrGrid(paths.cube_model, 5, 5,
	              {-4.0f, 0, 0}, {0, 0, 0}, {1, 1, 1}, -4.0f, 4.0f, 1.0f);

	// Flat vases (bad normals) in a grid (PBR sweep)
	createPbrGrid(paths.flat_vase_model, 5, 5,
	              {-4.0f, -4.0f, 0}, {glm::radians(-180.0f), 0.0f, 0.0f}, {6.0f, 6.0f, 3.0f}, -4.0f, -4.0f, 0);

	// Smooth vases (interpolated normals) in a grid (PBR sweep)
	createPbrGrid(paths.smooth_vase_model, 5, 5,
	              {0, -4.0f, 0}, {glm::radians(-180.0f), 0.0f, 0.0f}, {6.0f, 6.0f, 3.0f}, 4.0f, -4.0f, 0);
}

} // namespace ve
