#include "scene/ve_scene.hpp"
#include "scene/ve_component.hpp"
#include "resources/ve_model.hpp"
#include "utils/ve_log.hpp"

#define GLM_FORCE_RADIANS
#include <glm/gtc/matrix_transform.hpp>

namespace ve {

VeScene::VeScene(VeDevice& device, VeResourceManager& resource_manager,
                 VeDescriptorPool& pool, VeDescriptorSetLayout& material_layout,
                 const std::string& name)
    : m_device(device), m_resource_manager(resource_manager),
      m_pool(pool), m_material_layout(material_layout),
      m_name(name), m_num_lights(0), m_num_shadow_casting_lights(0) {}

VeScene::~VeScene() {
	m_default_material_handle = ResourceHandle<VeMaterial>{};
}

void VeScene::addModel(const std::filesystem::path& gltf_path) {
	auto model = VeModel::load(m_resource_manager, gltf_path.lexically_normal(), &m_pool, &m_material_layout,
		/*extract_lights=*/true, /*flip_tex_coord_v=*/false);

	if (model) {
		model->addToScene(m_registry, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});

		if (!m_default_material_handle.isValid()) {
			for (auto& mc : m_registry.meshes()) {
				if (mc.hasMaterial() && mc.getMaterial()->hasDescriptorSet()) {
					m_default_material_handle = mc.getMaterialHandle();
					break;
				}
			}
		}

		m_models.push_back(std::move(model));
	} else {
		VE_LOGE("Failed to load GLTF model: " << gltf_path);
	}
}

void VeScene::update(float dt) {
	for (auto [entity, pl, tc] : m_registry.view<PointLightComponent, TransformComponent>()) {
		if (!pl.getRotates())
			continue;
		const float speed = 0.04f;
		const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), speed * dt, glm::vec3(0.0f, 0.0f, 1.0f));
		glm::vec4 pos{tc.getTranslation(), 1.0f};
		pos = rot * pos;
		tc.setTranslation(glm::vec3(pos));
	}
}

} // namespace ve
