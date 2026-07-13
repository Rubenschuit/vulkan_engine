#include "simple_scene.hpp"
#include "scene/ve_component.hpp"
#include "utils/ve_path.hpp"
#include <string>
#include <glm/gtc/constants.hpp>

namespace ve {

SimpleScene::SimpleScene(const SceneContext& ctx, const AssetPaths& paths)
	: VeScene(ctx, "Simple Scene") {
	loadGameObjects(paths);
}

void SimpleScene::loadGameObjects(const AssetPaths& paths) {
	{
		Entity dl = m_registry.createDirectionalLight(3.0f, glm::vec3(0.2f), glm::vec3(0.6f, 0.7f, -1.0f));
		m_registry.setName(dl, "Directional Light");
		m_registry.getComponent<DirectionalLightComponent>(dl)->setCastsShadow(true);
	}

	{
		const glm::vec3 color{1.0f, 0.85f, 0.7f};
		const float width = 14.0f, height = 7.0f;

		// Area light
		Entity al = m_registry.createAreaLight(10.0f, color, width, height);
		m_registry.setName(al, "Area Light");
		m_registry.getComponent<TransformComponent>(al)->setTranslation({0.0f, -70.0f, 9.0f});
		m_registry.getComponent<AreaLightComponent>(al)->setTwoSided(false);

		// Emissive panel mesh
		const glm::vec3 n{0.0f, 1.0f, 0.0f};
		const glm::vec4 t{1.0f, 0.0f, 0.0f, 1.0f};
		auto quad_mesh = m_resource_manager.createMesh("simple_scene::area_light_quad",
			{
				{{-0.5f, 0.0f, -0.5f}, n, {0.0f, 1.0f}, t},
				{{ 0.5f, 0.0f,  0.5f}, n, {1.0f, 0.0f}, t},
				{{-0.5f, 0.0f,  0.5f}, n, {0.0f, 0.0f}, t},
				{{ 0.5f, 0.0f, -0.5f}, n, {1.0f, 1.0f}, t},
			},
			{0, 1, 2, 0, 3, 1});
		auto emissive_mat = m_resource_manager.createMaterial("simple_scene::area_light_emissive",
			MaterialTextures{}, MaterialAlphaProps{.double_sided = true},
			MaterialFactors{
				.base_color_factor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
				.emissive_factor = color,
				.metallic_factor = 0.0f,
				.emissive_strength = 44.0f});

		Entity panel = m_registry.createGameObject("Area Light Panel");
		m_registry.addComponent<MeshComponent>(panel, quad_mesh, emissive_mat).has_shadow = false;
		m_registry.setParent(panel, al);
	}

	// Create some point lights with ranging colors
	constexpr uint32_t num_lights = 10;
	constexpr float intensity = 70.0f;
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

	// floor (grid texture, tiled at ~1m intervals)
	{
		constexpr float half = 100.0f;
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
		auto floor_mesh = m_resource_manager.createMesh("simple_scene::floor_mesh", floor_verts, floor_indices);

		MaterialFactors floor_factors{};
		floor_factors.roughness_factor = 0.3f;
		floor_factors.metallic_factor = 0.4f;
		auto floor_mat = m_resource_manager.createMaterial(
			"simple_scene::floor",
			MaterialTextures{
				.albedo = VeTexture::loadFromPath(m_resource_manager,
				                                  paths.grid_texture.lexically_normal(),
				                                  TextureType::ALBEDO),
				.metallic_roughness = m_resource_manager.load<VeTexture>("default_mr_unit"),
			},
			MaterialAlphaProps{
				.alpha_mode = AlphaMode::ALPHA_OPAQUE,
				.alpha_cutoff = 0.5f,
				.double_sided = true,
				.use_spec_gloss = false},
			floor_factors);

		Entity e = m_registry.createEntity("floor");
		auto& tc = m_registry.addComponent<TransformComponent>(e);
		tc.setTranslation({0.0f, 0.0f, -0.05f});
		auto& mc = m_registry.addComponent<MeshComponent>(e, floor_mesh, floor_mat);
		mc.has_texture = 1.0f;
		mc.has_shadow = false;
	}

	// Textured quad
	{
		auto quad_mesh = m_resource_manager.loadMesh(paths.quad_model.lexically_normal());
		if (quad_mesh.isValid()) {
			auto mat_handle = m_resource_manager.createMaterial(
				"simple_scene::mots_quad",
				MaterialTextures{
					.albedo = VeTexture::loadFromPath(m_resource_manager,
					                                  paths.mots_texture.lexically_normal(),
					                                  TextureType::ALBEDO),
				},
				MaterialAlphaProps{AlphaMode::MASK, 0.5f, true}, MaterialFactors{});

			Entity quad_entity = m_registry.createEntity("tex_quad");
			auto& tc = m_registry.addComponent<TransformComponent>(quad_entity);
			tc.setTranslation({0.0f, 70.0f, 20.0f});
			tc.setScale({18.0f, 18.0f, 18.0f});
			auto& mc = m_registry.addComponent<MeshComponent>(quad_entity, quad_mesh, mat_handle);
			mc.has_texture = 1.0f;
			mc.has_shadow = false;

			// Add spot light shining on the quad
			glm::vec3 light_dir = glm::normalize(glm::vec3{0.0f, 70.0f, 60.0f});
			Entity sl = m_registry.createSpotLight(2000.0f, 15.0f, glm::vec3(1.0f), light_dir, glm::radians(20.0f), glm::radians(30.0f));
			m_registry.setName(sl, "Quad Spot Light");
			auto* sl_tc = m_registry.getComponent<TransformComponent>(sl);
			sl_tc->setTranslation(glm::vec3(0.0f, -0.76f, -0.93f));
			sl_tc->setScale(glm::vec3(1.0f));
			m_registry.setParent(sl, quad_entity);
		}

	}

	// PBR showcase grid: rows vary roughness (bottom=rough, top=smooth), columns vary metallic (left=dielectric, right=metal)
	auto createPbrGrid = [&](const std::filesystem::path& model_path,
	                         int rows, int cols,
	                         glm::vec3 base_translation, glm::vec3 base_rotation, glm::vec3 base_scale,
	                         float spacing_x, float spacing_y, float z_offset,
	                         PhysicsShapeType shape_type = PhysicsShapeType::Box) {
		auto mesh = m_resource_manager.loadMesh(model_path.lexically_normal());
		if (!mesh.isValid())
			return;

		std::string name = ve::pathToUtf8(model_path.stem());
		for (int j = 0; j < rows; j++) {
			for (int i = 0; i < cols; i++) {
				float roughness = glm::max(1.0f - static_cast<float>(j) / static_cast<float>(rows - 1), 0.05f);
				float metallic = static_cast<float>(i) / static_cast<float>(cols - 1);

				MaterialFactors factors{};
				factors.roughness_factor = roughness;
				factors.metallic_factor = metallic;

				auto mat_handle = m_resource_manager.createMaterial(
					"pbr_grid::" + name + "_r" + std::to_string(j) + "_m" + std::to_string(i),
					MaterialTextures{
						// MaterialFactors alone drives metallic/roughness; use unit MR texture so factors are not multiplied by the standard default's 0/1
						.metallic_roughness = m_resource_manager.load<VeTexture>("default_mr_unit"),
					},
					MaterialAlphaProps{
						.alpha_mode = AlphaMode::ALPHA_OPAQUE,
						.alpha_cutoff = 0.5f,
						.double_sided = false,
						.use_spec_gloss = false
					},
					factors);

				Entity e = m_registry.createEntity(name + "_r" + std::to_string(j) + "_m" + std::to_string(i));
				auto& tc = m_registry.addComponent<TransformComponent>(e);
				tc.setTranslation(base_translation + glm::vec3{(float)i * spacing_x, (float)j * spacing_y, z_offset});
				tc.setRotationEuler(base_rotation);
				tc.setScale(base_scale);
				auto& mc = m_registry.addComponent<MeshComponent>(e, mesh, mat_handle);
				mc.has_texture = 1.0f;

				auto& rb = m_registry.addComponent<RigidbodyComponent>(e);
				rb.setMotionType(PhysicsMotionType::Dynamic);
				rb.setShapeDesc({.type = shape_type});
			}
		}
	};

	// Spheres in a grid
	createPbrGrid(paths.sphere_model, 5, 5,
	              {0, 0, 0}, {0, 0, 0}, {1, 1, 1}, 4.0f, 4.0f, 1.0f,
	              PhysicsShapeType::Sphere);

	// Cubes in a grid
	createPbrGrid(paths.cube_model, 5, 5,
	              {-4.0f, 0, 0}, {0, 0, 0}, {1, 1, 1}, -4.0f, 4.0f, 1.0f);

	// Flat vases (bad normals) in a grid
	createPbrGrid(paths.flat_vase_model, 5, 5,
	              {-4.0f, -4.0f, 0}, {glm::radians(-180.0f), 0.0f, 0.0f}, {6.0f, 6.0f, 3.0f}, -4.0f, -4.0f, 0,
	              PhysicsShapeType::ConvexHull);

	// Smooth vases (interpolated normals) in a grid
	createPbrGrid(paths.smooth_vase_model, 5, 5,
	              {0, -4.0f, 0}, {glm::radians(-180.0f), 0.0f, 0.0f}, {6.0f, 6.0f, 3.0f}, 4.0f, -4.0f, 0,
	              PhysicsShapeType::ConvexHull);

	// Animated fox: Survey/Walk/Run blend space
	placeModel({
		.gltf_path = paths.fox_model.lexically_normal(),
		.translation = {22.0f, 22.0f, 0.0f},
		.rotation = {0.0f, 0.0f, glm::radians(-135.0f)},
		.scale = glm::vec3(0.03f),
		.on_loaded = [this](Entity wrapper) {
			m_registry.setName(wrapper, "Fox");
			auto* anim = m_registry.getComponent<AnimatorComponent>(wrapper);
			if (!anim)
				return;
			int survey = anim->findClip("Survey");
			int walk = anim->findClip("Walk");
			int run = anim->findClip("Run");
			if (survey < 0 || walk < 0 || run < 0)
				return;
			anim->setBlendSpace1D({
				{.position = 0.0f, .clip_index = static_cast<uint32_t>(survey)},
				{.position = 0.5f, .clip_index = static_cast<uint32_t>(walk)},
				{.position = 1.0f, .clip_index = static_cast<uint32_t>(run)},
			});
			anim->setBlendParameter(0.0f);
		},
	});
}

} // namespace ve
