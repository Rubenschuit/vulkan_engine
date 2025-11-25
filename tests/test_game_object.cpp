#include <catch2/catch_test_macros.hpp>
#include <game/ve_game_object.hpp>
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
		obj.transform.translation = {1.0f, 2.0f, 3.0f};
		glm::mat4 expected = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}

	SECTION("Scale only") {
		obj.transform.scale = {2.0f, 2.0f, 2.0f};
		glm::mat4 expected = glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 2.0f});
		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}

	SECTION("Rotation only (Y-axis)") {
		obj.transform.rotation = {0.0f, glm::half_pi<float>(), 0.0f};
		glm::mat4 expected = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), {0.0f, 1.0f, 0.0f});
		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}

	SECTION("Rotation order (YXZ intrinsic)") {
		obj.transform.rotation = {0.5f, 0.5f, 0.5f}; // Radians

		glm::mat4 ry = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 1.0f, 0.0f});
		glm::mat4 rx = glm::rotate(glm::mat4(1.0f), 0.5f, {1.0f, 0.0f, 0.0f});
		glm::mat4 rz = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 0.0f, 1.0f});

		glm::mat4 expected = ry * rx * rz;

		REQUIRE(mat4Equal(obj.getTransform(), expected));
	}
}

TEST_CASE("VeGameObject point light factory", "[gameobject][factory]") {
	auto light = ve::VeGameObject::createPointLight(5.0f, 2.0f, {1.0f, 0.0f, 0.0f});

	REQUIRE(light.point_light_component.has_value());
	REQUIRE(light.point_light_component->intensity == 5.0f);
	REQUIRE(light.transform.scale.x == 2.0f);
	REQUIRE(light.color == glm::vec3(1.0f, 0.0f, 0.0f));
}

