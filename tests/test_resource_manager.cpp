// Tests for VeResourceManager's frame-indexed deferred unload (rescue queue).
#include <catch2/catch_test_macros.hpp>
#include <resources/ve_resource.hpp>
#include <resources/ve_resource_manager.hpp>
#include <vulkan/ve_device.hpp>
#include <platform/ve_window.hpp>
#include <events/event_bus.hpp>
#include <ve_config.hpp>

#include <memory>
#include <string>

namespace {

class MockResource : public ve::Resource {
public:
	MockResource(const std::string& id, bool* unload_flag)
		: ve::Resource(id), m_unload_flag(unload_flag) {
		setLoaded(true); // registerExisting bypasses load(); mark as loaded.
	}

protected:
	bool doLoad() override { return true; }
	void doUnload() override {
		if (m_unload_flag)
			*m_unload_flag = true;
	}

private:
	bool* m_unload_flag;
};

// MockMaterial: holds a child handle and releases it from doUnload().
// Used for test #6 (recursive release).
class MockMaterial : public ve::Resource {
public:
	MockMaterial(const std::string& id, ve::ResourceHandle<MockResource> child, bool* unload_flag)
		: ve::Resource(id), m_child(std::move(child)), m_unload_flag(unload_flag) {
		setLoaded(true);
	}

protected:
	bool doLoad() override { return true; }
	void doUnload() override {
		m_child = ve::ResourceHandle<MockResource>{}; // drop child -> queue child for retirement
		if (m_unload_flag)
			*m_unload_flag = true;
	}

private:
	ve::ResourceHandle<MockResource> m_child;
	bool* m_unload_flag;
};

// Holds a VeDevice and an EventBus the manager can reference. The tests never
// trigger load(), so the device is never actually used; we just need valid
// objects for the reference bindings.
struct ManagerFixture {
	ve::VeWindow window{16, 16, "ResourceManagerTest"};
	ve::VeDevice device{window};
	ve::EventBus event_bus;
	ve::VeResourceManager manager{device, event_bus};
};

} // namespace

TEST_CASE("ResourceHandle copy and destroy track refcount", "[resource][refcount]") {
	ManagerFixture fix;
	bool unloaded = false;
	auto handle = fix.manager.registerExisting<MockResource>(
		"r1", std::make_shared<MockResource>("r1", &unloaded));

	REQUIRE(handle.isValid());
	REQUIRE(fix.manager.pendingUnloadCount() == 0);

	{
		auto copy = handle;
		REQUIRE(copy.get() == handle.get());
		REQUIRE(!unloaded);
		REQUIRE(fix.manager.pendingUnloadCount() == 0);
	}

	// Copy went out of scope but original still alive.
	REQUIRE(!unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
}

TEST_CASE("Last release defers unload (rescue queue)", "[resource][defer]") {
	ManagerFixture fix;
	bool unloaded = false;
	{
		auto handle = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
	}
	// Handle gone, refcount hit 0 -> queued, NOT destroyed.
	REQUIRE(!unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 1);
}

TEST_CASE("Resource destroyed after MAX_FRAMES_IN_FLIGHT ticks", "[resource][tick]") {
	ManagerFixture fix;
	bool unloaded = false;
	{
		auto handle = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
	}
	for (uint32_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i)
		fix.manager.tickFrame();
	REQUIRE(unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
}

TEST_CASE("Premature tick does not destroy", "[resource][tick]") {
	ManagerFixture fix;
	bool unloaded = false;
	{
		auto handle = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
	}
	for (uint32_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT - 1; ++i)
		fix.manager.tickFrame();
	REQUIRE(!unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 1);
}

TEST_CASE("Rescue: load() during deferred window keeps the same instance", "[resource][rescue]") {
	ManagerFixture fix;
	bool unloaded = false;
	MockResource* original_ptr = nullptr;
	{
		auto first = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
		original_ptr = first.get();
	}
	REQUIRE(fix.manager.pendingUnloadCount() == 1);

	// Re-acquire the same id by addRef'ing — load() goes through file IO, but
	// addRef matches the load() rescue path (it bumps the cached entry).
	fix.manager.addRef<MockResource>("r1");
	MockResource* rescued_ptr = fix.manager.getResource<MockResource>("r1");
	REQUIRE(rescued_ptr == original_ptr);

	for (uint32_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i)
		fix.manager.tickFrame();

	// Pending entry processed but refcount > 0 -> resource kept alive.
	REQUIRE(!unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
	REQUIRE(fix.manager.getResource<MockResource>("r1") == original_ptr);
}

TEST_CASE("Recursive release: material drops child texture in doUnload", "[resource][recursive]") {
	ManagerFixture fix;
	bool child_unloaded = false;
	bool material_unloaded = false;

	auto child = fix.manager.registerExisting<MockResource>(
		"tex", std::make_shared<MockResource>("tex", &child_unloaded));
	auto material = fix.manager.registerExisting<MockMaterial>(
		"mat", std::make_shared<MockMaterial>("mat", child, &material_unloaded));

	// Drop our local child handle so only material holds it (refcount=1).
	child = ve::ResourceHandle<MockResource>{};
	REQUIRE(!child_unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);

	// Drop material -> queued.
	material = ve::ResourceHandle<MockMaterial>{};
	REQUIRE(!material_unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 1);

	// First MAX_FRAMES_IN_FLIGHT ticks: material's doUnload runs and queues
	// the texture for ANOTHER MAX_FRAMES_IN_FLIGHT frames.
	for (uint32_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i)
		fix.manager.tickFrame();
	REQUIRE(material_unloaded);
	REQUIRE(!child_unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 1);

	// Second window: texture finally destroyed.
	for (uint32_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i)
		fix.manager.tickFrame();
	REQUIRE(child_unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
}

TEST_CASE("Rescue + re-release across a frame boundary defers destruction", "[resource][rescue][cross-frame]") {
	ManagerFixture fix;
	bool unloaded = false;
	{
		auto h = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
	}
	// Frame 0: refcount 0, entry A queued at retire = MAX_FRAMES_IN_FLIGHT.
	REQUIRE(fix.manager.pendingUnloadCount() == 1);

	// Advance to frame 1
	fix.manager.tickFrame();
	REQUIRE(!unloaded);

	// Rescue then re-release at frame 1: entry B queued at retire = 1 + MAX.
	fix.manager.addRef<MockResource>("r1");
	fix.manager.release<MockResource>("r1");
	REQUIRE(fix.manager.pendingUnloadCount() == 2);

	// advance to frame MAX_FRAMES_IN_FLIGHT
	for (uint32_t i = 1; i < ve::MAX_FRAMES_IN_FLIGHT; ++i)
		fix.manager.tickFrame();
	REQUIRE(!unloaded);

	// One more tick to frame 1 + MAX_FRAMES_IN_FLIGHT: B is now due
	fix.manager.tickFrame();
	REQUIRE(unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
}

TEST_CASE("Duplicate retirement entries: destroy once, stale entry pops silently", "[resource][duplicate]") {
	ManagerFixture fix;
	bool unloaded = false;

	{
		auto h = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
	}
	REQUIRE(fix.manager.pendingUnloadCount() == 1);

	// Rescue via addRef, then drop again -> second entry queued.
	fix.manager.addRef<MockResource>("r1");
	fix.manager.release<MockResource>("r1");
	REQUIRE(fix.manager.pendingUnloadCount() == 2);
	REQUIRE(!unloaded);

	// Process both entries: first destroys, second pops silently.
	for (uint32_t i = 0; i < ve::MAX_FRAMES_IN_FLIGHT; ++i)
		fix.manager.tickFrame();
	REQUIRE(unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
}

TEST_CASE("flushPendingUnloads drains queue ignoring retire_frame", "[resource][flush]") {
	ManagerFixture fix;
	bool a_unloaded = false;
	bool b_unloaded = false;

	{
		auto a = fix.manager.registerExisting<MockResource>(
			"a", std::make_shared<MockResource>("a", &a_unloaded));
		auto b = fix.manager.registerExisting<MockResource>(
			"b", std::make_shared<MockResource>("b", &b_unloaded));
	}
	REQUIRE(fix.manager.pendingUnloadCount() == 2);

	fix.manager.flushPendingUnloads();
	REQUIRE(a_unloaded);
	REQUIRE(b_unloaded);
	REQUIRE(fix.manager.pendingUnloadCount() == 0);
}

TEST_CASE("Manager destructor flushes pending unloads", "[resource][destructor]") {
	bool unloaded = false;
	{
		ManagerFixture fix;
		auto h = fix.manager.registerExisting<MockResource>(
			"r1", std::make_shared<MockResource>("r1", &unloaded));
		h = ve::ResourceHandle<MockResource>{};
		REQUIRE(fix.manager.pendingUnloadCount() == 1);
		REQUIRE(!unloaded);
	}
	// Fixture (and manager) destructed.
	REQUIRE(unloaded);
}