/* EcsEventDispatcher
 *
 * Events are plain structs. The dispatcher is owned by Registry,
 * so subscription lifetimes are scoped to the scene. Systems subscribe once per
 * scene load and never need to manually unsubscribe.
 *
 */
#pragma once
#include "ve_entity.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ve {

// ── Event types ─────────────────────────────────────────────────────────────

struct EntityCreatedEvent {
	Entity entity;
};

struct EntityDestroyedEvent {
	Entity entity;
};

template <typename T>
struct ComponentAddedEvent {
	Entity entity;
	T& component;
};

template <typename T>
struct ComponentRemovedEvent {
	Entity entity;
};

struct TransformInvalidatedEvent {
	Entity entity;
};

struct MeshDataChangedEvent {
	Entity entity;
};

struct RigidbodyChangedEvent {
	Entity entity;
};

struct LightDataChangedEvent {
	Entity entity;
};

// Deferred deletion request (emitted by UI, processed at safe frame boundary)
struct DeleteEntityRequest {
	Entity entity;
	bool recursive = false;
};

// ── EcsEventDispatcher ──────────────────────────────────────────────────────

using SubscriptionId = uint32_t;

class EcsEventDispatcher {
public:
	EcsEventDispatcher() = default;
	~EcsEventDispatcher() = default;

	EcsEventDispatcher(const EcsEventDispatcher&) = delete;
	EcsEventDispatcher& operator=(const EcsEventDispatcher&) = delete;

	template <typename EventT>
	SubscriptionId subscribe(std::function<void(const EventT&)> handler) {
		SubscriptionId id = m_next_id++;
		auto& list = getOrCreateList<EventT>();
		list.entries.push_back({id, std::move(handler)});
		return id;
	}

	template <typename EventT>
	void unsubscribe(SubscriptionId id) {
		auto it = m_handlers.find(std::type_index(typeid(EventT)));
		if (it == m_handlers.end())
			return;
		auto& entries = static_cast<HandlerList<EventT>&>(*it->second).entries;
		entries.erase(
			std::remove_if(entries.begin(), entries.end(),
				[id](const auto& e) { return e.id == id; }),
			entries.end());
	}

	template <typename EventT>
	void emit(const EventT& event) {
		if (m_batch_depth > 0)
			return;
		auto it = m_handlers.find(std::type_index(typeid(EventT)));
		if (it == m_handlers.end())
			return;
		auto& entries = static_cast<HandlerList<EventT>&>(*it->second).entries;
		for (auto& entry : entries)
			entry.handler(event);
	}

	bool isBatching() const { return m_batch_depth > 0; }
	void beginBatch() { m_batch_depth++; }
	void endBatch() {
		if (m_batch_depth > 0)
			m_batch_depth--;
	}

private:
	struct HandlerBase {
		virtual ~HandlerBase() = default;
	};

	template <typename EventT>
	struct HandlerEntry {
		SubscriptionId id;
		std::function<void(const EventT&)> handler;
	};

	template <typename EventT>
	struct HandlerList : HandlerBase {
		std::vector<HandlerEntry<EventT>> entries;
	};

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

	std::unordered_map<std::type_index, std::unique_ptr<HandlerBase>> m_handlers;
	SubscriptionId m_next_id = 0;
	uint32_t m_batch_depth = 0;
};

} // namespace ve
