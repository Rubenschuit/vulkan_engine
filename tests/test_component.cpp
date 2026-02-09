#include <catch2/catch_test_macros.hpp>
#include <scene/ve_component.hpp>
#include <scene/ve_game_object.hpp>
#include <glm/glm.hpp>

// Dummy component for testing type ID uniqueness
struct TestComponentA : ve::Component {};
struct TestComponentB : ve::Component {};

TEST_CASE("ComponentTypeIDSystem assigns unique IDs per type", "[component][typeid]") {
	size_t id_a = ve::Component::getTypeID<TestComponentA>();
	size_t id_b = ve::Component::getTypeID<TestComponentB>();
	size_t id_transform = ve::Component::getTypeID<ve::TransformComponent>();
	size_t id_point_light = ve::Component::getTypeID<ve::PointLightComponent>();
	size_t id_mesh = ve::Component::getTypeID<ve::MeshComponent>();

	REQUIRE(id_a != id_b);
	REQUIRE(id_a != id_transform);
	REQUIRE(id_b != id_transform);
	REQUIRE(id_transform != id_point_light);
	REQUIRE(id_point_light != id_mesh);

	// Same type returns same ID
	REQUIRE(ve::Component::getTypeID<TestComponentA>() == id_a);
	REQUIRE(ve::Component::getTypeID<ve::TransformComponent>() == id_transform);
}

TEST_CASE("VeGameObject addComponent returns existing component if already present", "[gameobject][component]") {
	auto obj = ve::VeGameObject::createGameObject();

	auto* t1 = obj.getComponent<ve::TransformComponent>();
	REQUIRE(t1 != nullptr);

	// addComponent of same type should return existing, not create duplicate
	auto* t2 = obj.addComponent<ve::TransformComponent>();
	REQUIRE(t2 == t1);
}

TEST_CASE("VeGameObject getComponent returns nullptr for missing component", "[gameobject][component]") {
	auto obj = ve::VeGameObject::createGameObject();

	REQUIRE(obj.getComponent<ve::PointLightComponent>() == nullptr);
	REQUIRE(obj.getComponent<ve::MeshComponent>() == nullptr);
}

TEST_CASE("VeGameObject removeComponent", "[gameobject][component]") {
	auto obj = ve::VeGameObject::createGameObject();

	// Add PointLight
	auto* pl = obj.addComponent<ve::PointLightComponent>();
	REQUIRE(pl != nullptr);
	REQUIRE(obj.getComponent<ve::PointLightComponent>() == pl);

	// Remove it
	bool removed = obj.removeComponent<ve::PointLightComponent>();
	REQUIRE(removed);
	REQUIRE(obj.getComponent<ve::PointLightComponent>() == nullptr);

	// Remove non-existent returns false
	removed = obj.removeComponent<ve::PointLightComponent>();
	REQUIRE_FALSE(removed);
}

TEST_CASE("Component owner is set when added", "[component][owner]") {
	auto obj = ve::VeGameObject::createGameObject();
	auto* transform = obj.getComponent<ve::TransformComponent>();

	REQUIRE(transform != nullptr);
	REQUIRE(transform->getOwner() == &obj);
}
