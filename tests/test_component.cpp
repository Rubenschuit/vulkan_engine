#include <catch2/catch_test_macros.hpp>
#include <scene/ve_component.hpp>
#include <scene/ve_registry.hpp>
#include <glm/glm.hpp>

// Dummy component for testing type ID uniqueness
struct TestComponentA : ve::Component {};
struct TestComponentB : ve::Component {};

TEST_CASE("ComponentTypeIDSystem assigns unique IDs per type", "[component][typeid]") {
	size_t id_a = ve::Component::getTypeID<TestComponentA>();
	size_t id_b = ve::Component::getTypeID<TestComponentB>();
	size_t id_transform = ve::Component::getTypeID<ve::TransformComponent>();
	size_t id_point_light = ve::Component::getTypeID<ve::PointLightComponent>();
	size_t id_dir_light = ve::Component::getTypeID<ve::DirectionalLightComponent>();
	size_t id_mesh = ve::Component::getTypeID<ve::MeshComponent>();

	REQUIRE(id_a != id_b);
	REQUIRE(id_a != id_transform);
	REQUIRE(id_b != id_transform);
	REQUIRE(id_transform != id_point_light);
	REQUIRE(id_point_light != id_dir_light);
	REQUIRE(id_dir_light != id_mesh);
	REQUIRE(id_point_light != id_mesh);

	// Same type returns same ID
	REQUIRE(ve::Component::getTypeID<TestComponentA>() == id_a);
	REQUIRE(ve::Component::getTypeID<ve::TransformComponent>() == id_transform);
	REQUIRE(ve::Component::getTypeID<ve::DirectionalLightComponent>() == id_dir_light);
}

TEST_CASE("Registry getComponent returns nullptr for missing component", "[registry][component]") {
	ve::Registry registry;
	auto e = registry.createEntity("test");

	REQUIRE(registry.getComponent<ve::PointLightComponent>(e) == nullptr);
	REQUIRE(registry.getComponent<ve::MeshComponent>(e) == nullptr);
}

TEST_CASE("Registry removeComponent", "[registry][component]") {
	ve::Registry registry;
	auto e = registry.createEntity("test");

	// Add PointLight
	auto& pl = registry.addComponent<ve::PointLightComponent>(e);
	REQUIRE(registry.getComponent<ve::PointLightComponent>(e) == &pl);

	// Remove it
	registry.removeComponent<ve::PointLightComponent>(e);
	REQUIRE(registry.getComponent<ve::PointLightComponent>(e) == nullptr);
}

TEST_CASE("Registry DirectionalLightComponent add/remove", "[registry][component]") {
	ve::Registry registry;
	auto e = registry.createEntity("test");

	auto& dl = registry.addComponent<ve::DirectionalLightComponent>(e);
	REQUIRE(registry.getComponent<ve::DirectionalLightComponent>(e) == &dl);
	REQUIRE(dl.getEntity() == e);
	REQUIRE(dl.getRegistry() == &registry);

	registry.removeComponent<ve::DirectionalLightComponent>(e);
	REQUIRE(registry.getComponent<ve::DirectionalLightComponent>(e) == nullptr);
}

TEST_CASE("Component context is set when added via Registry", "[component][context]") {
	ve::Registry registry;
	auto e = registry.createGameObject();
	auto* transform = registry.getComponent<ve::TransformComponent>(e);

	REQUIRE(transform != nullptr);
	REQUIRE(transform->getEntity() == e);
	REQUIRE(transform->getRegistry() == &registry);
}
