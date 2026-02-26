#include <catch2/catch_test_macros.hpp>
#include "vulkan/ve_command_resource_manager.hpp"

TEST_CASE("ThreadSlot default is invalid", "[command]") {
	ve::ThreadSlot slot;
	REQUIRE(!slot.valid());
	REQUIRE(slot.id == UINT32_MAX);
}

TEST_CASE("ThreadSlot with valid id", "[command]") {
	ve::ThreadSlot slot{0};
	REQUIRE(slot.valid());
	REQUIRE(slot.id == 0);
}

TEST_CASE("ThreadSlot boundary values", "[command]") {
	ve::ThreadSlot slot_max{UINT32_MAX - 1};
	REQUIRE(slot_max.valid());

	ve::ThreadSlot slot_invalid{UINT32_MAX};
	REQUIRE(!slot_invalid.valid());
}
