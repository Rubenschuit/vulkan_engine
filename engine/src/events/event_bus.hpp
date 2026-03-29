/* EventBus - thread-safe engine-wide event system.
 *
 * Supports two dispatch modes:
 *   emitImmediate  - synchronous dispatch on the main thread
 *   enqueue/flush  - thread-safe deferred dispatch (safe to call from worker threads)
 *
 * Owned by VeApplication. Systems subscribe during initialisation and receive
 * events at well-defined flush points in the main loop.
 */
#pragma once
#include "ve_export.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ve {

// Tag base for events that carry references and must only use emitImmediate().
struct ImmediateOnly {};

using EventSubscriptionId = uint64_t;

class VENGINE_API EventBus {
public:
	EventBus() = default;
	~EventBus() = default;

	EventBus(const EventBus&) = delete;
	EventBus& operator=(const EventBus&) = delete;

	// Subscribe to an event type. Returns an ID for later unsubscription.
	template <typename EventT>
	EventSubscriptionId subscribe(std::function<void(const EventT&)> handler) {
		std::unique_lock lock(m_handler_mutex);
		EventSubscriptionId id = m_next_id++;
		auto& list = getOrCreateList<EventT>();
		list.entries.push_back({id, std::move(handler)});
		return id;
	}

	// Unsubscribe by event type and subscription ID.
	template <typename EventT>
	void unsubscribe(EventSubscriptionId id) {
		std::unique_lock lock(m_handler_mutex);
		auto it = m_handlers.find(std::type_index(typeid(EventT)));
		if (it == m_handlers.end())
			return;
		auto& entries = static_cast<HandlerList<EventT>&>(*it->second).entries;
		entries.erase(
			std::remove_if(entries.begin(), entries.end(),
				[id](const auto& e) { return e.id == id; }),
			entries.end());
	}

	// Dispatch an event immediately to all current subscribers.
	// Handlers may safely call subscribe/unsubscribe (snapshot-then-dispatch).
	// A handler that unsubscribes itself will still fire for the current event.
	template <typename EventT>
	void emitImmediate(const EventT& event) {
		snapshotAndDispatch(event);
	}

	// Queue an event for deferred dispatch. Thread-safe.
	// Cross-type dispatch order is undefined; intra-type order is preserved.
	template <typename EventT>
	void enqueue(EventT event) {
		static_assert(!std::is_base_of_v<ImmediateOnly, EventT>,
			"This event carries references and must not be enqueued. Use emitImmediate().");
		std::lock_guard lock(m_queue_mutex);
		auto key = std::type_index(typeid(EventT));
		auto& base = m_queues[key];
		if (!base)
			base = std::make_unique<TypedQueue<EventT>>();
		static_cast<TypedQueue<EventT>&>(*base).events.push_back(std::move(event));
	}

	void flushEvents();
	void clearQueue();

private:
	struct HandlerBase {
		virtual ~HandlerBase() = default;
	};

	template <typename EventT>
	struct HandlerEntry {
		EventSubscriptionId id;
		std::function<void(const EventT&)> handler;
	};

	template <typename EventT>
	struct HandlerList : HandlerBase {
		std::vector<HandlerEntry<EventT>> entries;
	};

	struct QueueBase {
		virtual ~QueueBase() = default;
		virtual void dispatchAll(EventBus& bus) = 0;
	};

	template <typename EventT>
	struct TypedQueue : QueueBase {
		std::vector<EventT> events;

		void dispatchAll(EventBus& bus) override {
			std::vector<EventT> batch;
			batch.swap(events);
			std::vector<HandlerEntry<EventT>> snapshot;
			{
				std::shared_lock lock(bus.m_handler_mutex);
				auto it = bus.m_handlers.find(std::type_index(typeid(EventT)));
				if (it == bus.m_handlers.end())
					return;
				snapshot = static_cast<HandlerList<EventT>&>(*it->second).entries;
			}
			for (auto& event : batch)
				for (auto& entry : snapshot)
					entry.handler(event);
		}
	};

	template <typename EventT>
	void snapshotAndDispatch(const EventT& event) {
		std::vector<HandlerEntry<EventT>> snapshot;
		{
			std::shared_lock lock(m_handler_mutex);
			auto it = m_handlers.find(std::type_index(typeid(EventT)));
			if (it == m_handlers.end())
				return;
			snapshot = static_cast<HandlerList<EventT>&>(*it->second).entries;
		}
		for (auto& entry : snapshot)
			entry.handler(event);
	}

	template <typename EventT>
	HandlerList<EventT>& getOrCreateList() {
		auto key = std::type_index(typeid(EventT));
		auto it = m_handlers.find(key);
		if (it != m_handlers.end())
			return static_cast<HandlerList<EventT>&>(*it->second);
		auto list = std::make_unique<HandlerList<EventT>>();
		auto& ref = *list;
		m_handlers[key] = std::move(list);
		return ref;
	}

	std::shared_mutex m_handler_mutex;
	std::unordered_map<std::type_index, std::unique_ptr<HandlerBase>> m_handlers;
	EventSubscriptionId m_next_id = 0;

	std::mutex m_queue_mutex;
	std::unordered_map<std::type_index, std::unique_ptr<QueueBase>> m_queues;
};

} // namespace ve
