#include <catch2/catch_test_macros.hpp>
#include <scene/ve_game_object.hpp>
#include <scene/ve_component.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Helper to compare matrices
static bool mat4Equal(const glm::mat4& a, const glm::mat4& b, float eps = 1e-5f) {
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			if (std::abs(a[i][j] - b[i][j]) > eps) return false;
		}
	}
	return true;
}

TEST_CASE("VeGameObject creation and IDs", "[gameobject][core]") {
	auto obj1 = ve::VeGameObject::createGameObject();
	auto obj2 = ve::VeGameObject::createGameObject();

	REQUIRE(obj1.getId() != obj2.getId());
}

TEST_CASE("VeGameObject transform matrix calculation", "[gameobject][transform]") {
	auto obj = ve::VeGameObject::createGameObject();

	SECTION("Identity transform default") {
		glm::mat4 transform = obj.getTransform();
		REQUIRE(mat4Equal(transform, glm::mat4(1.0f)));
	}

	SECTION("Translation only") {
		obj.getComponent<ve::TransformComponent>()->translation = {1.0f, 2.0f, 3.0f};
		glm::mat4 expected = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}

	SECTION("Scale only") {
		obj.getComponent<ve::TransformComponent>()->scale = {2.0f, 2.0f, 2.0f};
		glm::mat4 expected = glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 2.0f});
		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}

	SECTION("Rotation only (Y-axis)") {
		obj.getComponent<ve::TransformComponent>()->rotation = {0.0f, glm::half_pi<float>(), 0.0f};
		glm::mat4 expected = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), {0.0f, 1.0f, 0.0f});
		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}

	SECTION("Rotation order (YXZ intrinsic)") {
		obj.getComponent<ve::TransformComponent>()->rotation = {0.5f, 0.5f, 0.5f}; // Radians

		glm::mat4 ry = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 1.0f, 0.0f});
		glm::mat4 rx = glm::rotate(glm::mat4(1.0f), 0.5f, {1.0f, 0.0f, 0.0f});
		glm::mat4 rz = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 0.0f, 1.0f});

		glm::mat4 expected = ry * rx * rz;

		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}
}

TEST_CASE("VeGameObject point light factory", "[gameobject][factory]") {
	auto light = ve::VeGameObject::createPointLight(5.0f, 2.0f, {1.0f, 0.0f, 0.0f});

	auto* pl = light.getComponent<ve::PointLightComponent>();
	auto* transform = light.getComponent<ve::TransformComponent>();
	auto* material = light.getComponent<ve::MaterialComponent>();
	REQUIRE(pl != nullptr);
	REQUIRE(pl->intensity == 5.0f);
	REQUIRE(transform->scale.x == 2.0f);
	REQUIRE(material->color == glm::vec3(1.0f, 0.0f, 0.0f));
}

TEST_CASE("VeGameObject parent-child hierarchy", "[gameobject][hierarchy]") {
	auto parent = ve::VeGameObject::createGameObject();
	auto child = ve::VeGameObject::createGameObject();

	parent.getComponent<ve::TransformComponent>()->translation = {1.0f, 2.0f, 3.0f};
	child.getComponent<ve::TransformComponent>()->translation = {0.0f, 1.0f, 0.0f};

	parent.addChild(&child);

	REQUIRE(child.getParent() == &parent);
	REQUIRE(parent.getChildren().size() == 1);
	REQUIRE(parent.getChildren()[0] == &child);

	// World transform of child should include parent's translation
	glm::mat4 child_world = child.getTransform();
	REQUIRE(child_world[3][0] == 1.0f);
	REQUIRE(child_world[3][1] == 3.0f);
	REQUIRE(child_world[3][2] == 3.0f);
}

