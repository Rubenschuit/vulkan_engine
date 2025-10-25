// Not used yet

#pragma once
#include "ve_export.hpp"
#include "ve_game_object.hpp"
#include <string>
#include <unordered_map>

namespace ve {

class VENGINE_API VeScene {
public:
    VeScene(const std::string& name);
    ~VeScene();

    VeScene(const VeScene&) = delete;
    VeScene& operator=(const VeScene&) = delete;
    
       
    std::unordered_map<uint32_t, VeGameObject>& getGameObjects();
    const std::unordered_map<uint32_t, VeGameObject>& getGameObjects() const;

    
private:
    std::string m_name;
    std::unordered_map<uint32_t, VeGameObject> m_game_objects;

    friend class VeGameObject;
};

}