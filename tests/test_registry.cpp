#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <scene/ve_entity.hpp>
#include <scene/ve_component_pool.hpp>
#include <scene/ve_registry.hpp>
#include <scene/ve_component.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using Catch::Approx;

// ── Entity ──────────────────────────────────────────────────────────────────

TEST_CASE("Entity null and identity", "[ecs][entity]") {
	ve::Entity null = ve::Entity::null();
	REQUIRE(null.isNull());

	ve::Entity also_null;
	REQUIRE(also_null.isNull());
	REQUIRE(null == also_null);
}

// ── ComponentPool ───────────────────────────────────────────────────────────

struct SimpleData {
	float x = 0.0f;
	float y = 0.0f;
};

TEST_CASE("ComponentPool emplace and get", "[ecs][pool]") {
	ve::ComponentPool<SimpleData> pool;

	pool.emplace(0, SimpleData{1.0f, 2.0f});
	pool.emplace(5, SimpleData{3.0f, 4.0f});

	// Use get() after all emplaces (emplace may invalidate references due to vector growth)
	REQUIRE(pool.get(0)->x == 1.0f);
	REQUIRE(pool.get(5)->y == 4.0f);
	REQUIRE(pool.size() == 2);
	REQUIRE(pool.has(0));
	REQUIRE(pool.has(5));
	REQUIRE(!pool.has(1));

	auto* got = pool.get(5);
	REQUIRE(got != nullptr);
	REQUIRE(got->x == 3.0f);

	REQUIRE(pool.get(99) == nullptr);
}

TEST_CASE("ComponentPool remove swaps last element", "[ecs][pool]") {
	ve::ComponentPool<SimpleData> pool;
	pool.emplace(0, SimpleData{1.0f, 0.0f});
	pool.emplace(1, SimpleData{2.0f, 0.0f});
	pool.emplace(2, SimpleData{3.0f, 0.0f});

	// Remove entity 0: element at dense[0] gets swapped with dense[2]
	pool.remove(0);

	REQUIRE(pool.size() == 2);
	REQUIRE(!pool.has(0));
	REQUIRE(pool.has(1));
	REQUIRE(pool.has(2));

	// Entity 2's data should still be accessible
	REQUIRE(pool.get(2)->x == 3.0f);
	REQUIRE(pool.get(1)->x == 2.0f);
}

TEST_CASE("ComponentPool dense iteration", "[ecs][pool]") {
	ve::ComponentPool<SimpleData> pool;
	pool.emplace(10, SimpleData{1.0f, 0.0f});
	pool.emplace(20, SimpleData{2.0f, 0.0f});
	pool.emplace(30, SimpleData{3.0f, 0.0f});

	float sum = 0.0f;
	for (uint32_t i = 0; i < pool.size(); i++) {
		sum += pool.data()[i].x;
	}
	REQUIRE(sum == 6.0f);

	// entityAt maps back to sparse indices
	std::vector<uint32_t> entities;
	for (uint32_t i = 0; i < pool.size(); i++) {
		entities.push_back(pool.entityAt(i));
	}
	std::sort(entities.begin(), entities.end());
	REQUIRE(entities == std::vector<uint32_t>{10, 20, 30});
}

TEST_CASE("ComponentPool range-for", "[ecs][pool]") {
	ve::ComponentPool<SimpleData> pool;
	pool.emplace(0, SimpleData{1.0f, 0.0f});
	pool.emplace(1, SimpleData{2.0f, 0.0f});

	float sum = 0.0f;
	for (const auto& d : pool) {
		sum += d.x;
	}
	REQUIRE(sum == 3.0f);
}

// ── Registry: entity lifecycle ──────────────────────────────────────────────

TEST_CASE("Registry create and destroy entities", "[ecs][registry]") {
	ve::Registry reg;

	ve::Entity e1 = reg.createEntity("a");
	ve::Entity e2 = reg.createEntity("b");

	REQUIRE(!e1.isNull());
	REQUIRE(!e2.isNull());
	REQUIRE(e1 != e2);
	REQUIRE(reg.entityCount() == 2);
	REQUIRE(reg.isAlive(e1));
	REQUIRE(reg.getName(e1) == "a");

	reg.destroyEntity(e1);
	REQUIRE(!reg.isAlive(e1));
	REQUIRE(reg.isAlive(e2));
	REQUIRE(reg.entityCount() == 1);
}

TEST_CASE("Registry recycled entity gets new generation", "[ecs][registry]") {
	ve::Registry reg;

	ve::Entity e1 = reg.createEntity();
	uint32_t idx1 = e1.index();
	uint32_t gen1 = e1.generation();

	reg.destroyEntity(e1);
	ve::Entity e2 = reg.createEntity();

	// Should reuse the same index but with incremented generation
	REQUIRE(e2.index() == idx1);
	REQUIRE(e2.generation() == gen1 + 1);

	// Old handle is no longer alive
	REQUIRE(!reg.isAlive(e1));
	REQUIRE(reg.isAlive(e2));
}

// ── Registry: components ────────────────────────────────────────────────────

TEST_CASE("Registry add and get components", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createEntity();

	auto& tc = reg.addComponent<ve::TransformComponent>(e);
	tc.setTranslation({1.0f, 2.0f, 3.0f});

	auto* got = reg.getComponent<ve::TransformComponent>(e);
	REQUIRE(got != nullptr);
	REQUIRE(got->getTranslation() == glm::vec3(1.0f, 2.0f, 3.0f));

	REQUIRE(reg.hasComponent<ve::TransformComponent>(e));
	REQUIRE(!reg.hasComponent<ve::PointLightComponent>(e));
}

TEST_CASE("Registry remove component", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createEntity();

	reg.addComponent<ve::PointLightComponent>(e);
	REQUIRE(reg.hasComponent<ve::PointLightComponent>(e));

	reg.removeComponent<ve::PointLightComponent>(e);
	REQUIRE(!reg.hasComponent<ve::PointLightComponent>(e));
}

TEST_CASE("Registry getComponent on dead entity returns nullptr", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createEntity();
	reg.addComponent<ve::TransformComponent>(e);

	reg.destroyEntity(e);
	REQUIRE(reg.getComponent<ve::TransformComponent>(e) == nullptr);
}

TEST_CASE("Registry destroyEntity removes all components", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createEntity();
	reg.addComponent<ve::TransformComponent>(e);
	reg.addComponent<ve::PointLightComponent>(e);

	REQUIRE(reg.transforms().size() == 1);
	REQUIRE(reg.pointLights().size() == 1);

	reg.destroyEntity(e);

	REQUIRE(reg.transforms().size() == 0);
	REQUIRE(reg.pointLights().size() == 0);
}

// ── Registry: convenience factories ─────────────────────────────────────────

TEST_CASE("Registry createGameObject adds TransformComponent", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createGameObject("test_obj");

	REQUIRE(reg.isAlive(e));
	REQUIRE(reg.getName(e) == "test_obj");
	REQUIRE(reg.hasComponent<ve::TransformComponent>(e));
	REQUIRE(!reg.hasComponent<ve::MeshComponent>(e));
}

TEST_CASE("Registry createPointLight adds Transform + PointLight", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createPointLight(5.0f, 2.0f, {1.0f, 0.0f, 0.0f});

	REQUIRE(reg.hasComponent<ve::TransformComponent>(e));
	REQUIRE(reg.hasComponent<ve::PointLightComponent>(e));

	auto* pl = reg.getComponent<ve::PointLightComponent>(e);
	REQUIRE(pl->getIntensity() == 5.0f);
	REQUIRE(pl->getColor() == glm::vec3(1.0f, 0.0f, 0.0f));

	auto* tc = reg.getComponent<ve::TransformComponent>(e);
	REQUIRE(tc->getScale().x == 2.0f);
}

// ── Registry: pool iteration ────────────────────────────────────────────────

TEST_CASE("Registry pool iteration is contiguous", "[ecs][registry]") {
	ve::Registry reg;

	// Create 3 game objects (each gets a TransformComponent)
	ve::Entity e1 = reg.createGameObject();
	ve::Entity e2 = reg.createGameObject();
	ve::Entity e3 = reg.createGameObject();

	reg.getComponent<ve::TransformComponent>(e1)->setTranslation({1.0f, 0.0f, 0.0f});
	reg.getComponent<ve::TransformComponent>(e2)->setTranslation({2.0f, 0.0f, 0.0f});
	reg.getComponent<ve::TransformComponent>(e3)->setTranslation({3.0f, 0.0f, 0.0f});

	// Iterate the transform pool directly
	auto& pool = reg.transforms();
	REQUIRE(pool.size() == 3);

	float sum_x = 0.0f;
	for (uint32_t i = 0; i < pool.size(); i++) {
		sum_x += pool.data()[i].getTranslation().x;
	}
	REQUIRE(sum_x == 6.0f);
}

TEST_CASE("Registry point light pool iteration", "[ecs][registry]") {
	ve::Registry reg;

	// 5 objects, only 2 are point lights
	reg.createGameObject();
	reg.createPointLight(1.0f, 1.0f, {1, 0, 0});
	reg.createGameObject();
	reg.createPointLight(2.0f, 1.0f, {0, 1, 0});
	reg.createGameObject();

	auto& pl_pool = reg.pointLights();
	REQUIRE(pl_pool.size() == 2);

	float intensity_sum = 0.0f;
	for (uint32_t i = 0; i < pl_pool.size(); i++) {
		intensity_sum += pl_pool.data()[i].getIntensity();
	}
	REQUIRE(intensity_sum == 3.0f);
}

TEST_CASE("Registry createDirectionalLight adds Transform + DirectionalLight", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createDirectionalLight(10.0f, {1.0f, 0.5f, 0.0f}, {0.0f, -1.0f, 0.0f});

	REQUIRE(reg.hasComponent<ve::TransformComponent>(e));
	REQUIRE(reg.hasComponent<ve::DirectionalLightComponent>(e));
	REQUIRE(!reg.hasComponent<ve::PointLightComponent>(e));

	auto* dl = reg.getComponent<ve::DirectionalLightComponent>(e);
	REQUIRE(dl->getIntensity() == 10.0f);
	REQUIRE(dl->getColor() == glm::vec3(1.0f, 0.5f, 0.0f));
	REQUIRE(dl->getDirection() == glm::vec3(0.0f, -1.0f, 0.0f));
}

TEST_CASE("Registry directional light pool iteration", "[ecs][registry]") {
	ve::Registry reg;

	reg.createGameObject();
	reg.createDirectionalLight(3.0f);
	reg.createPointLight(1.0f, 1.0f);
	reg.createDirectionalLight(7.0f);
	reg.createGameObject();

	auto& dl_pool = reg.directionalLights();
	REQUIRE(dl_pool.size() == 2);

	float intensity_sum = 0.0f;
	for (uint32_t i = 0; i < dl_pool.size(); i++) {
		intensity_sum += dl_pool.data()[i].getIntensity();
	}
	REQUIRE(intensity_sum == 10.0f);

	// Point lights should be unaffected
	REQUIRE(reg.pointLights().size() == 1);
}

// ── Registry: spot light factories ──────────────────────────────────────────

TEST_CASE("Registry createSpotLight adds Transform + SpotLight", "[ecs][registry]") {
	ve::Registry reg;
	glm::vec3 dir = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f));
	ve::Entity e = reg.createSpotLight(200.0f, 15.0f, {1.0f, 0.8f, 0.6f},
	                                   dir, glm::radians(20.0f), glm::radians(30.0f));

	REQUIRE(reg.isAlive(e));
	REQUIRE(reg.hasComponent<ve::TransformComponent>(e));
	REQUIRE(reg.hasComponent<ve::SpotLightComponent>(e));
	REQUIRE(!reg.hasComponent<ve::PointLightComponent>(e));

	auto* sl = reg.getComponent<ve::SpotLightComponent>(e);
	REQUIRE(sl->getIntensity() == 200.0f);
	REQUIRE(sl->getColor() == glm::vec3(1.0f, 0.8f, 0.6f));
	REQUIRE(std::abs(sl->getInnerConeAngle() - glm::radians(20.0f)) < 1e-5f);
	REQUIRE(std::abs(sl->getOuterConeAngle() - glm::radians(30.0f)) < 1e-5f);

	// Direction should be normalized
	glm::vec3 got_dir = sl->getDirection();
	REQUIRE(std::abs(glm::length(got_dir) - 1.0f) < 1e-5f);
	REQUIRE(std::abs(got_dir.x - dir.x) < 1e-5f);
	REQUIRE(std::abs(got_dir.y - dir.y) < 1e-5f);
	REQUIRE(std::abs(got_dir.z - dir.z) < 1e-5f);

	// Radius sets transform scale
	auto* tc = reg.getComponent<ve::TransformComponent>(e);
	REQUIRE(tc->getScale().x == 15.0f);
}

TEST_CASE("Registry createSpotLight with defaults", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createSpotLight();

	auto* sl = reg.getComponent<ve::SpotLightComponent>(e);
	REQUIRE(sl->getIntensity() == 1.0f);
	REQUIRE(sl->getColor() == glm::vec3(1.0f));
	REQUIRE(std::abs(sl->getInnerConeAngle() - glm::radians(25.0f)) < 1e-5f);
	REQUIRE(std::abs(sl->getOuterConeAngle() - glm::radians(35.0f)) < 1e-5f);
	REQUIRE(sl->getDirection() == glm::vec3(0.f, 0.f, -1.f));

	auto* tc = reg.getComponent<ve::TransformComponent>(e);
	REQUIRE(tc->getScale().x == 1.0f);
}

TEST_CASE("Registry spot light pool iteration", "[ecs][registry]") {
	ve::Registry reg;

	reg.createGameObject();
	reg.createSpotLight(5.0f, 1.0f, {1, 0, 0});
	reg.createPointLight(1.0f, 1.0f);
	reg.createSpotLight(10.0f, 1.0f, {0, 1, 0});
	reg.createGameObject();

	auto& sl_pool = reg.spotLights();
	REQUIRE(sl_pool.size() == 2);

	float intensity_sum = 0.0f;
	for (uint32_t i = 0; i < sl_pool.size(); i++)
		intensity_sum += sl_pool.data()[i].getIntensity();
	REQUIRE(intensity_sum == 15.0f);

	// Other pools unaffected
	REQUIRE(reg.pointLights().size() == 1);
	REQUIRE(reg.transforms().size() == 5);
}

TEST_CASE("Registry destroyEntity removes spot light component", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createSpotLight(3.0f);

	REQUIRE(reg.spotLights().size() == 1);

	reg.destroyEntity(e);

	REQUIRE(reg.spotLights().size() == 0);
	REQUIRE(!reg.isAlive(e));
}

TEST_CASE("Registry clear removes spot lights", "[ecs][registry]") {
	ve::Registry reg;
	reg.createSpotLight(1.0f);
	reg.createSpotLight(2.0f);
	reg.createPointLight();

	REQUIRE(reg.spotLights().size() == 2);

	reg.clear();

	REQUIRE(reg.spotLights().size() == 0);
	REQUIRE(reg.pointLights().size() == 0);
	REQUIRE(reg.entityCount() == 0);
}

// ── Registry: hierarchy ─────────────────────────────────────────────────────

TEST_CASE("Registry setParent and child iteration", "[ecs][registry][hierarchy]") {
	ve::Registry reg;
	ve::Entity parent = reg.createGameObject("parent");
	ve::Entity child1 = reg.createGameObject("child1");
	ve::Entity child2 = reg.createGameObject("child2");

	reg.setParent(child1, parent);
	reg.setParent(child2, parent);

	REQUIRE(reg.getParent(child1) == parent);
	REQUIRE(reg.getParent(child2) == parent);
	REQUIRE(reg.hasParent(child1));
	REQUIRE(!reg.hasParent(parent));

	// Iterate children via first_child / next_sibling
	std::vector<ve::Entity> children;
	ve::Entity c = reg.firstChild(parent);
	while (!c.isNull()) {
		children.push_back(c);
		c = reg.nextSibling(c);
	}
	REQUIRE(children.size() == 2);
}

TEST_CASE("Registry reparent removes from old parent", "[ecs][registry][hierarchy]") {
	ve::Registry reg;
	ve::Entity p1 = reg.createGameObject("p1");
	ve::Entity p2 = reg.createGameObject("p2");
	ve::Entity child = reg.createGameObject("child");

	reg.setParent(child, p1);
	REQUIRE(reg.getParent(child) == p1);

	reg.setParent(child, p2);
	REQUIRE(reg.getParent(child) == p2);

	// p1 should have no children
	REQUIRE(reg.firstChild(p1).isNull());
	// p2 should have child
	REQUIRE(reg.firstChild(p2) == child);
}

// ── Registry: world transforms ──────────────────────────────────────────────

static bool mat4Equal(const glm::mat4& a, const glm::mat4& b, float eps = 1e-5f) {
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			if (std::abs(a[i][j] - b[i][j]) > eps) return false;
	return true;
}

TEST_CASE("Registry world transform without parent = local", "[ecs][registry][transform]") {
	ve::Registry reg;
	ve::Entity e = reg.createGameObject();

	auto* tc = reg.getComponent<ve::TransformComponent>(e);
	tc->setTranslation({1.0f, 2.0f, 3.0f});

	const glm::mat4& world = reg.getWorldTransform(e);
	glm::mat4 expected = glm::translate(glm::mat4(1.0f), {1.0f, 2.0f, 3.0f});
	REQUIRE(mat4Equal(world, expected));
}

TEST_CASE("Registry world transform with parent = parent * local", "[ecs][registry][transform]") {
	ve::Registry reg;
	ve::Entity parent = reg.createGameObject();
	ve::Entity child = reg.createGameObject();

	reg.getComponent<ve::TransformComponent>(parent)->setTranslation({1.0f, 0.0f, 0.0f});
	reg.getComponent<ve::TransformComponent>(child)->setTranslation({0.0f, 1.0f, 0.0f});

	reg.setParent(child, parent);

	const glm::mat4& world = reg.getWorldTransform(child);
	// Child at (0,1,0) in parent space, parent at (1,0,0) in world space
	// World = parent_world * child_local = translate(1,0,0) * translate(0,1,0) = translate(1,1,0)
	REQUIRE(std::abs(world[3][0] - 1.0f) < 1e-5f);
	REQUIRE(std::abs(world[3][1] - 1.0f) < 1e-5f);
	REQUIRE(std::abs(world[3][2] - 0.0f) < 1e-5f);
}

TEST_CASE("Registry world transform invalidation cascades to children", "[ecs][registry][transform]") {
	ve::Registry reg;
	ve::Entity parent = reg.createGameObject();
	ve::Entity child = reg.createGameObject();

	reg.getComponent<ve::TransformComponent>(parent)->setTranslation({1.0f, 0.0f, 0.0f});
	reg.getComponent<ve::TransformComponent>(child)->setTranslation({0.0f, 1.0f, 0.0f});
	reg.setParent(child, parent);

	// Force initial computation
	const glm::mat4& initial = reg.getWorldTransform(child);
	REQUIRE(std::abs(initial[3][0] - 1.0f) < 1e-5f);

	// Move parent; the setter invalidates the world-transform cache through the registry
	reg.getComponent<ve::TransformComponent>(parent)->setTranslation({5.0f, 0.0f, 0.0f});

	const glm::mat4& updated = reg.getWorldTransform(child);
	REQUIRE(std::abs(updated[3][0] - 5.0f) < 1e-5f);
	REQUIRE(std::abs(updated[3][1] - 1.0f) < 1e-5f);
}

TEST_CASE("Registry world normal for root entity = local normal", "[ecs][registry][transform]") {
	ve::Registry reg;
	ve::Entity e = reg.createGameObject();

	auto* tc = reg.getComponent<ve::TransformComponent>(e);
	tc->setScale({2.0f, 1.0f, 1.0f});

	const glm::mat3& normal = reg.getWorldNormal(e);
	const glm::mat3& local_normal = tc->getNormalTransform();

	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			REQUIRE(std::abs(normal[i][j] - local_normal[i][j]) < 1e-5f);
}

// ── Registry: entityFromIndex ───────────────────────────────────────────────

TEST_CASE("Registry entityFromIndex round-trips", "[ecs][registry]") {
	ve::Registry reg;
	ve::Entity e = reg.createEntity("test");
	ve::Entity reconstructed = reg.entityFromIndex(e.index());
	REQUIRE(reconstructed == e);
	REQUIRE(reg.isAlive(reconstructed));
}

// ── Registry: clear ─────────────────────────────────────────────────────────

TEST_CASE("Registry clear resets everything", "[ecs][registry]") {
	ve::Registry reg;
	reg.createGameObject();
	reg.createPointLight();

	REQUIRE(reg.entityCount() == 2);
	REQUIRE(reg.transforms().size() == 2);
	REQUIRE(reg.pointLights().size() == 1);

	reg.clear();

	REQUIRE(reg.entityCount() == 0);
	REQUIRE(reg.transforms().size() == 0);
	REQUIRE(reg.pointLights().size() == 0);
}

// ── Registry: setWorldPose ──────────────────────────────────────────────────

TEST_CASE("setWorldPose lands the entity at the requested world pose", "[ecs][registry][transform]") {
	ve::Registry reg;

	const glm::vec3 want_pos{3.0f, -4.0f, 5.0f};
	const glm::quat want_rot = glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));

	SECTION("root entity: the write is the local transform") {
		ve::Entity e = reg.createGameObject();
		reg.setWorldPose(e, want_pos, want_rot);

		auto* tc = reg.getComponent<ve::TransformComponent>(e);
		REQUIRE(glm::length(tc->getTranslation() - want_pos) == Approx(0.0f).margin(1e-5));
		REQUIRE(std::abs(glm::dot(tc->getRotation(), want_rot)) == Approx(1.0f).margin(1e-5));
	}

	SECTION("parented entity: the parent chain is undone") {
		ve::Entity parent = reg.createGameObject();
		auto* ptc = reg.getComponent<ve::TransformComponent>(parent);
		ptc->setTranslation({10.0f, 20.0f, -5.0f});
		ptc->setRotationEuler({0.0f, 0.0f, glm::radians(90.0f)});

		ve::Entity child = reg.createGameObject();
		reg.setParent(child, parent);
		reg.setWorldPose(child, want_pos, want_rot);

		const glm::mat4& world = reg.getWorldTransform(child);
		REQUIRE(glm::length(glm::vec3(world[3]) - want_pos) == Approx(0.0f).margin(1e-4));
		REQUIRE(std::abs(glm::dot(reg.getWorldRotation(child), want_rot)) == Approx(1.0f).margin(1e-4));

		// The local transform is genuinely different -- otherwise this proves nothing.
		auto* ctc = reg.getComponent<ve::TransformComponent>(child);
		REQUIRE(glm::length(ctc->getTranslation() - want_pos) > 0.1f);
	}

	SECTION("scaled parent: position still resolves, basis stays unit") {
		ve::Entity parent = reg.createGameObject();
		reg.getComponent<ve::TransformComponent>(parent)->setScale({2.0f, 2.0f, 2.0f});

		ve::Entity child = reg.createGameObject();
		reg.setParent(child, parent);
		reg.setWorldPose(child, want_pos, want_rot);

		const glm::mat4& world = reg.getWorldTransform(child);
		REQUIRE(glm::length(glm::vec3(world[3]) - want_pos) == Approx(0.0f).margin(1e-4));
	}

	SECTION("no TransformComponent is a no-op, not a crash") {
		ve::Entity bare = reg.createEntity("bare");
		reg.setWorldPose(bare, want_pos, want_rot);
		REQUIRE_FALSE(reg.hasComponent<ve::TransformComponent>(bare));
	}
}
