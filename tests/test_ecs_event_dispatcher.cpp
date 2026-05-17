#include <catch2/catch_test_macros.hpp>
#include <scene/ecs_event_dispatcher.hpp>
#include <scene/ve_entity.hpp>

using namespace ve;

TEST_CASE("ComponentRemovedEvent bypasses batch suppression", "[ecs][dispatcher]") {
	EcsEventDispatcher d;
	int add_count = 0;
	int remove_count = 0;

	d.subscribe<ComponentAddedEvent<int>>(
		[&](const ComponentAddedEvent<int>&) { ++add_count; });
	d.subscribe<ComponentRemovedEvent<int>>(
		[&](const ComponentRemovedEvent<int>&) { ++remove_count; });

	Entity e = Entity::fromRaw(1);
	int dummy = 42;

	// Outside batch: both events fire.
	d.emit(ComponentAddedEvent<int>{e, dummy});
	d.emit(ComponentRemovedEvent<int>{e});
	REQUIRE(add_count == 1);
	REQUIRE(remove_count == 1);

	// Inside batch: ComponentAddedEvent is suppressed; ComponentRemovedEvent
	// still dispatches 
	d.beginBatch();
	d.emit(ComponentAddedEvent<int>{e, dummy});
	d.emit(ComponentRemovedEvent<int>{e});
	REQUIRE(add_count == 1);
	REQUIRE(remove_count == 2);
	d.endBatch();

	// After batch: normal dispatch resumes.
	d.emit(ComponentAddedEvent<int>{e, dummy});
	d.emit(ComponentRemovedEvent<int>{e});
	REQUIRE(add_count == 2);
	REQUIRE(remove_count == 3);
}