#include <catch2/catch_test_macros.hpp>
#include <scene/ve_registry.hpp>
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

TEST_CASE("Entity creation and IDs", "[entity][core]") {
	ve::Registry registry;
	auto e1 = registry.createEntity();
	auto e2 = registry.createEntity();

	REQUIRE(e1 != e2);
	REQUIRE(registry.isAlive(e1));
	REQUIRE(registry.isAlive(e2));
}

TEST_CASE("Entity transform matrix calculation", "[entity][transform]") {
	ve::Registry registry;
	auto e = registry.createGameObject();
	auto* tc = registry.getComponent<ve::TransformComponent>(e);
	REQUIRE(tc != nullptr);

	SECTION("Identity transform default") {
		REQUIRE(mat4Equal(tc->getTransform(), glm::mat4(1.0f)));
	}

	SECTION("Translation only") {
		tc->setTranslation({1.0f, 2.0f, 3.0f});
		glm::mat4 expected = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
		REQUIRE(mat4Equal(tc->getTransform(), expected));
	}

	SECTION("Scale only") {
		tc->setScale({2.0f, 2.0f, 2.0f});
		glm::mat4 expected = glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 2.0f});
		REQUIRE(mat4Equal(tc->getTransform(), expected));
	}

	SECTION("Rotation only (Y-axis)") {
		tc->setRotationEuler({0.0f, glm::half_pi<float>(), 0.0f});
		glm::mat4 expected = glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), {0.0f, 1.0f, 0.0f});
		REQUIRE(mat4Equal(tc->getTransform(), expected));
	}

	SECTION("Rotation order (ZYX: Rz*Ry*Rx)") {
		tc->setRotationEuler({0.5f, 0.5f, 0.5f});

		glm::mat4 rx = glm::rotate(glm::mat4(1.0f), 0.5f, {1.0f, 0.0f, 0.0f});
		glm::mat4 ry = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 1.0f, 0.0f});
		glm::mat4 rz = glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 0.0f, 1.0f});
		glm::mat4 expected = rz * ry * rx;

		REQUIRE(mat4Equal(tc->getTransform(), expected));
	}
}

TEST_CASE("Registry point light factory", "[entity][factory]") {
	ve::Registry registry;
	auto light = registry.createPointLight(5.0f, 2.0f, {1.0f, 0.0f, 0.0f});

	auto* pl = registry.getComponent<ve::PointLightComponent>(light);
	auto* transform = registry.getComponent<ve::TransformComponent>(light);
	REQUIRE(pl != nullptr);
	REQUIRE(pl->getIntensity() == 5.0f);
	REQUIRE(pl->getColor() == glm::vec3(1.0f, 0.0f, 0.0f));
	REQUIRE(transform->getScale().x == 2.0f);
}

TEST_CASE("Registry parent-child hierarchy", "[entity][hierarchy]") {
	ve::Registry registry;
	auto parent = registry.createGameObject();
	auto child = registry.createGameObject();

	registry.getComponent<ve::TransformComponent>(parent)->setTranslation({1.0f, 2.0f, 3.0f});
	registry.getComponent<ve::TransformComponent>(child)->setTranslation({0.0f, 1.0f, 0.0f});

	registry.setParent(child, parent);

	REQUIRE(registry.getParent(child) == parent);
	REQUIRE(registry.firstChild(parent) == child);

	// World transform of child should include parent's translation
	const glm::mat4& child_world = registry.getWorldTransform(child);
	REQUIRE(child_world[3][0] == 1.0f);
	REQUIRE(child_world[3][1] == 3.0f);
	REQUIRE(child_world[3][2] == 3.0f);
}
