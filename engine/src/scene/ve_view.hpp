/* View - multi-component iterator for ECS Registry.
 *
 * Iterates entities that have ALL listed component types. Picks the smallest
 * pool as the driver and intersects with the others via sparse lookups.
 * Active filtering is ON by default. Supports range-for with structured bindings.
 *
 * Usage:
 *   for (auto [entity, pl, tc] : registry.view<PointLightComponent, TransformComponent>()) {
 *       // pl and tc are references
 *   }
 */
#pragma once
#include "ve_entity.hpp"
#include "ve_component_pool.hpp"
#include <tuple>
#include <cstdint>

namespace ve {

class Registry;

template <typename... Components>
class View {
	static_assert(sizeof...(Components) >= 1, "View requires at least one component type");

public:
	View(Registry& registry, ComponentPool<Components>&... pools)
		: m_registry(&registry), m_pools(pools...) {}

	View& includeInactive() & {
		m_filter_active = false;
		return *this;
	}
	View includeInactive() && {
		m_filter_active = false;
		return std::move(*this);
	}

	template <typename E>
	View& exclude() & {
		static_assert((!std::is_same_v<E, Components> && ...),
			"Cannot exclude a component that is in the view");
		assert(m_exclude_count < MAX_EXCLUDES);
		m_exclude_checks[m_exclude_count++] = [](Registry& reg, uint32_t entity_idx) -> bool {
			return reg.pool<E>().has(entity_idx);
		};
		return *this;
	}
	template <typename E>
	View exclude() && {
		static_assert((!std::is_same_v<E, Components> && ...),
			"Cannot exclude a component that is in the view");
		assert(m_exclude_count < MAX_EXCLUDES);
		m_exclude_checks[m_exclude_count++] = [](Registry& reg, uint32_t entity_idx) -> bool {
			return reg.pool<E>().has(entity_idx);
		};
		return std::move(*this);
	}

	uint32_t sizeHint() const {
		uint32_t min_size = UINT32_MAX;
		auto check = [&](auto& pool) {
			if (pool.size() < min_size)
				min_size = pool.size();
		};
		std::apply([&](auto&... pools) { (check(pools), ...); }, m_pools);
		return min_size;
	}

	// Callback-based iteration
	template <typename Func>
	void each(Func&& func) const {
		const uint32_t* driver_entities = nullptr;
		uint32_t driver_size = 0;
		findDriver(driver_entities, driver_size);
		for (uint32_t d = 0; d < driver_size; d++) {
			uint32_t entity_idx = driver_entities[d];
			if (matches(entity_idx))
				func(m_registry->entityFromIndex(entity_idx),
				     std::get<ComponentPool<Components>&>(m_pools).getRef(entity_idx)...);
		}
	}

	// Range-for support
	class Iterator {
	public:
		Iterator(const View* view, const uint32_t* entities, uint32_t pos, uint32_t size)
			: m_view(view), m_driver_entities(entities), m_pos(pos), m_size(size) {
			advance();
		}

		// Sentinel constructor
		Iterator(uint32_t end_pos) : m_view(nullptr), m_driver_entities(nullptr), m_pos(end_pos), m_size(end_pos) {}

		auto operator*() const -> std::tuple<Entity, Components&...> {
			uint32_t entity_idx = m_driver_entities[m_pos];
			return std::tuple<Entity, Components&...>{
				m_view->m_registry->entityFromIndex(entity_idx),
				std::get<ComponentPool<Components>&>(m_view->m_pools).getRef(entity_idx)...
			};
		}

		Iterator& operator++() {
			++m_pos;
			advance();
			return *this;
		}

		bool operator!=(const Iterator& other) const { return m_pos != other.m_pos; }

	private:
		void advance() {
			while (m_pos < m_size && !m_view->matches(m_driver_entities[m_pos]))
				++m_pos;
		}

		const View* m_view;
		const uint32_t* m_driver_entities;
		uint32_t m_pos;
		uint32_t m_size;
	};

	Iterator begin() const {
		const uint32_t* driver_entities = nullptr;
		uint32_t driver_size = 0;
		findDriver(driver_entities, driver_size);
		return Iterator(this, driver_entities, 0, driver_size);
	}

	Iterator end() const {
		const uint32_t* driver_entities = nullptr;
		uint32_t driver_size = 0;
		findDriver(driver_entities, driver_size);
		return Iterator(driver_size);
	}

private:
	static constexpr uint32_t MAX_EXCLUDES = 4;
	using ExcludeFunc = bool(*)(Registry&, uint32_t);

	Registry* m_registry;
	std::tuple<ComponentPool<Components>&...> m_pools;
	bool m_filter_active = true;
	ExcludeFunc m_exclude_checks[MAX_EXCLUDES] = {};
	uint32_t m_exclude_count = 0;

	void findDriver(const uint32_t*& out_entities, uint32_t& out_size) const {
		uint32_t min_size = UINT32_MAX;
		auto check = [&](auto& pool) {
			if (pool.size() < min_size) {
				min_size = pool.size();
				out_entities = pool.entityIndexData();
				out_size = pool.size();
			}
		};
		std::apply([&](auto&... pools) { (check(pools), ...); }, m_pools);
		if (min_size == UINT32_MAX) {
			out_entities = nullptr;
			out_size = 0;
		}
	}

	bool matches(uint32_t entity_idx) const {
		if (m_filter_active && !m_registry->isActiveAtIndex(entity_idx))
			return false;
		if (!(std::get<ComponentPool<Components>&>(m_pools).has(entity_idx) && ...))
			return false;
		for (uint32_t e = 0; e < m_exclude_count; e++)
			if (m_exclude_checks[e](*m_registry, entity_idx))
				return false;
		return true;
	}
};

} // namespace ve
