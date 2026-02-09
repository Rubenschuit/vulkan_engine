#include "scene/ve_scene.hpp"

namespace ve {

VeScene::VeScene(VeDevice& device, const std::string& name)
    : m_device(device), m_name(name), m_num_lights(0), m_num_shadow_casting_lights(0) {}

VeScene::~VeScene() {}

std::unordered_map<uint32_t, VeGameObject>& VeScene::getGameObjects() {
    return m_game_objects;
}

const std::unordered_map<uint32_t, VeGameObject>& VeScene::getGameObjects() const {
    return m_game_objects;
}

} // namespace ve

