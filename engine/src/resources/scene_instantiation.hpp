/* Scene instantiation: add a VeModel into a Registry.
 * Creates entities for every node, attaches Transform/Mesh/Skin components,
 * extracts lights and cameras, runs mesh deduplication by world
 * position, and consolidates the hierarchy under a single wrapper entity that
 * carries the root transform.
 */
#pragma once
#include "ve_export.hpp"
#include "resources/ve_model.hpp"
#include "scene/ve_registry.hpp"

#include <glm/glm.hpp>

namespace ve {

// Returns the wrapper Entity that carries the root transform
Entity instantiateModel(const VeModel& model, Registry& registry,
                        const std::string& name,
                        const glm::vec3& root_translation,
                        const glm::vec3& root_rotation,
                        const glm::vec3& root_scale);

}