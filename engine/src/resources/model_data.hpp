/* Plain data structs describing a loaded model.
 * Produced by the glTF loader, no engine dependencies.
 */
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <string>
#include <vector>

namespace ve {

struct ModelNode {
	std::string name;
	glm::vec3 translation{0.f};
	glm::quat rotation{1.f, 0.f, 0.f, 0.f};
	glm::vec3 scale{1.f};
	int mesh_idx = -1;
	int material_idx = -1;
	int skin_idx = -1;
};

struct ModelSkin {
	std::vector<int> joint_node_indices;
	std::vector<glm::mat4> inverse_bind_matrices;
	int skeleton_root_node = -1;
};

enum class ExtractedLightType { Point, Directional, Spot };

struct ExtractedLight {
	ExtractedLightType type = ExtractedLightType::Point;
	glm::vec3 position{0.f};
	glm::vec3 direction{0.f, 0.f, -1.f};
	glm::vec3 color{1.f};
	float intensity = 1.f;
	float range = 0.f;                       
	float inner_cone_angle = 0.f;                  
	float outer_cone_angle = glm::radians(45.0f);
	std::string name;
	int node_idx = -1; // glTF node that produced this light (-1 = unknown)
};

struct ExtractedCamera {
	bool perspective = true;
	float yfov_radians = glm::radians(55.0f);
	float ortho_size = 10.0f;
	float znear = 0.1f;
	float zfar = 1000.0f;
	std::string name;
	int node_idx = -1;
};

}