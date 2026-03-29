#include <catch2/catch_test_macros.hpp>
#include <events/event_bus.hpp>

#include <atomic>
#include <thread>
#include <vector>

// ── Test event types ────────────────────────────────────────────────────────

struct TestEventA {
	int value = 0;
};

struct TestEventB {
	float x = 0.0f;
	float y = 0.0f;
};

// ── emitImmediate ───────────────────────────────────────────────────────────

TEST_CASE("EventBus emitImmediate delivers to subscriber", "[events]") {
	ve::EventBus bus;
	int received = -1;

	bus.subscribe<TestEventA>([&](const TestEventA& e) { received = e.value; });
	bus.emitImmediate(TestEventA{42});

	REQUIRE(received == 42);
}

TEST_CASE("EventBus emitImmediate with no subscribers does not crash", "[events]") {
	ve::EventBus bus;
	bus.emitImmediate(TestEventA{1});
}

TEST_CASE("EventBus emitImmediate delivers to multiple subscribers", "[events]") {
	ve::EventBus bus;
	int count = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) { count++; });
	bus.subscribe<TestEventA>([&](const TestEventA&) { count++; });
	bus.subscribe<TestEventA>([&](const TestEventA&) { count++; });
	bus.emitImmediate(TestEventA{0});

	REQUIRE(count == 3);
}

TEST_CASE("EventBus only delivers to matching event type", "[events]") {
	ve::EventBus bus;
	int a_count = 0;
	int b_count = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) { a_count++; });
	bus.subscribe<TestEventB>([&](const TestEventB&) { b_count++; });

	bus.emitImmediate(TestEventA{0});

	REQUIRE(a_count == 1);
	REQUIRE(b_count == 0);
}

// ── Unsubscribe ─────────────────────────────────────────────────────────────

TEST_CASE("EventBus unsubscribe stops delivery", "[events]") {
	ve::EventBus bus;
	int received = 0;

	auto id = bus.subscribe<TestEventA>([&](const TestEventA&) { received++; });
	bus.emitImmediate(TestEventA{0});
	REQUIRE(received == 1);

	bus.unsubscribe<TestEventA>(id);
	bus.emitImmediate(TestEventA{0});
	REQUIRE(received == 1);
}

TEST_CASE("EventBus unsubscribe only removes targeted subscriber", "[events]") {
	ve::EventBus bus;
	int count_a = 0;
	int count_b = 0;

	auto id_a = bus.subscribe<TestEventA>([&](const TestEventA&) { count_a++; });
	bus.subscribe<TestEventA>([&](const TestEventA&) { count_b++; });

	bus.unsubscribe<TestEventA>(id_a);
	bus.emitImmediate(TestEventA{0});

	REQUIRE(count_a == 0);
	REQUIRE(count_b == 1);
}

// ── Enqueue and flush ───────────────────────────────────────────────────────

TEST_CASE("EventBus enqueue does not deliver immediately", "[events]") {
	ve::EventBus bus;
	int received = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) { received++; });
	bus.enqueue(TestEventA{1});

	REQUIRE(received == 0);
}

TEST_CASE("EventBus flushEvents delivers queued events", "[events]") {
	ve::EventBus bus;
	int received = -1;

	bus.subscribe<TestEventA>([&](const TestEventA& e) { received = e.value; });
	bus.enqueue(TestEventA{99});
	bus.flushEvents();

	REQUIRE(received == 99);
}

TEST_CASE("EventBus flushEvents delivers multiple queued events in order", "[events]") {
	ve::EventBus bus;
	std::vector<int> values;

	bus.subscribe<TestEventA>([&](const TestEventA& e) { values.push_back(e.value); });
	bus.enqueue(TestEventA{1});
	bus.enqueue(TestEventA{2});
	bus.enqueue(TestEventA{3});
	bus.flushEvents();

	REQUIRE(values == std::vector<int>{1, 2, 3});
}

TEST_CASE("EventBus flush with empty queue does not crash", "[events]") {
	ve::EventBus bus;
	bus.flushEvents();
}

TEST_CASE("EventBus clearQueue discards pending events", "[events]") {
	ve::EventBus bus;
	int received = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) { received++; });
	bus.enqueue(TestEventA{1});
	bus.enqueue(TestEventA{2});
	bus.clearQueue();
	bus.flushEvents();

	REQUIRE(received == 0);
}

// ── Thread safety ───────────────────────────────────────────────────────────

TEST_CASE("EventBus handler subscribes during emitImmediate does not deadlock", "[events]") {
	ve::EventBus bus;
	int first_count = 0;
	int second_count = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) {
		first_count++;
		if (first_count == 1)
			bus.subscribe<TestEventA>([&](const TestEventA&) { second_count++; });
	});

	bus.emitImmediate(TestEventA{0});
	REQUIRE(first_count == 1);
	REQUIRE(second_count == 0);

	bus.emitImmediate(TestEventA{0});
	REQUIRE(first_count == 2);
	REQUIRE(second_count == 1);
}

TEST_CASE("EventBus handler unsubscribes self during emitImmediate does not deadlock", "[events]") {
	ve::EventBus bus;
	int count = 0;
	ve::EventSubscriptionId id{};

	id = bus.subscribe<TestEventA>([&](const TestEventA&) {
		count++;
		bus.unsubscribe<TestEventA>(id);
	});

	bus.emitImmediate(TestEventA{0});
	REQUIRE(count == 1);

	bus.emitImmediate(TestEventA{0});
	REQUIRE(count == 1);
}

TEST_CASE("EventBus handler subscribes during flushEvents does not deadlock", "[events]") {
	ve::EventBus bus;
	int second_count = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) {
		bus.subscribe<TestEventA>([&](const TestEventA&) { second_count++; });
	});

	bus.enqueue(TestEventA{0});
	bus.flushEvents();
	REQUIRE(second_count == 0);

	bus.enqueue(TestEventA{0});
	bus.flushEvents();
	REQUIRE(second_count == 1);
}

TEST_CASE("EventBus handler enqueues during flushEvents does not deadlock", "[events]") {
	ve::EventBus bus;
	int count = 0;

	bus.subscribe<TestEventA>([&](const TestEventA& e) {
		count++;
		if (e.value == 1)
			bus.enqueue(TestEventA{2});
	});

	bus.enqueue(TestEventA{1});
	bus.flushEvents();
	REQUIRE(count == 1);

	bus.flushEvents();
	REQUIRE(count == 2);
}

TEST_CASE("EventBus per-type queue preserves intra-type ordering", "[events]") {
	ve::EventBus bus;
	std::vector<int> values;

	bus.subscribe<TestEventA>([&](const TestEventA& e) { values.push_back(e.value); });
	for (int i = 0; i < 10; i++)
		bus.enqueue(TestEventA{i});
	bus.flushEvents();

	REQUIRE(values == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
}

TEST_CASE("EventBus flushEvents dispatches multiple event types", "[events]") {
	ve::EventBus bus;
	int a_count = 0;
	int b_count = 0;

	bus.subscribe<TestEventA>([&](const TestEventA&) { a_count++; });
	bus.subscribe<TestEventB>([&](const TestEventB&) { b_count++; });

	bus.enqueue(TestEventA{0});
	bus.enqueue(TestEventB{1.0f, 2.0f});
	bus.enqueue(TestEventA{0});
	bus.flushEvents();

	REQUIRE(a_count == 2);
	REQUIRE(b_count == 1);
}

TEST_CASE("EventBus thread-safe enqueue from multiple threads", "[events]") {
	ve::EventBus bus;
	std::atomic<int> total{0};

	bus.subscribe<TestEventA>([&](const TestEventA& e) {
		total.fetch_add(e.value, std::memory_order_relaxed);
	});

	constexpr int NUM_THREADS = 8;
	constexpr int EVENTS_PER_THREAD = 1000;

	std::vector<std::thread> threads;
	threads.reserve(NUM_THREADS);
	for (int t = 0; t < NUM_THREADS; t++) {
		threads.emplace_back([&bus]() {
			for (int i = 0; i < EVENTS_PER_THREAD; i++)
				bus.enqueue(TestEventA{1});
		});
	}

	for (auto& t : threads)
		t.join();

	bus.flushEvents();

	REQUIRE(total.load() == NUM_THREADS * EVENTS_PER_THREAD);
}
