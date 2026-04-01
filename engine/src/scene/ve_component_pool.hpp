/* ComponentPool - dense typed array with sparse index mapping.
 *
 * Stores components contiguously in a dense vector for cache-friendly iteration.
 * A sparse array maps entity indices to dense indices. Removal uses swap-and-pop
 * to keep the dense array packed.
 */
#pragma once
#include <cassert>
#include <cstdint>
#include <vector>
#include <utility>

namespace ve {

template <typename T>
class ComponentPool {
public:
	using value_type = T;
	static constexpr uint32_t INVALID = UINT32_MAX;

	void reserve(uint32_t capacity) {
		m_dense.reserve(capacity);
		m_dense_to_entity.reserve(capacity);
	}

	template <typename... Args>
	T& emplace(uint32_t entity_index, Args&&... args) {
		ensureSparseSize(entity_index);
		assert(m_sparse[entity_index] == INVALID && "Component already exists for this entity");
		uint32_t dense_idx = static_cast<uint32_t>(m_dense.size());
		m_sparse[entity_index] = dense_idx;
		m_dense.emplace_back(std::forward<Args>(args)...);
		m_dense_to_entity.push_back(entity_index);
		return m_dense.back();
	}

	void remove(uint32_t entity_index) {
		assert(has(entity_index) && "Entity does not have this component");
		uint32_t dense_idx = m_sparse[entity_index];
		uint32_t last_dense = static_cast<uint32_t>(m_dense.size()) - 1;
		if (dense_idx != last_dense) {
			m_dense[dense_idx] = std::move(m_dense[last_dense]);
			uint32_t moved_entity = m_dense_to_entity[last_dense];
			m_dense_to_entity[dense_idx] = moved_entity;
			m_sparse[moved_entity] = dense_idx;
		}
		m_dense.pop_back();
		m_dense_to_entity.pop_back();
		m_sparse[entity_index] = INVALID;
	}

	bool has(uint32_t entity_index) const {
		return entity_index < m_sparse.size() && m_sparse[entity_index] != INVALID;
	}

	T* get(uint32_t entity_index) {
		if (!has(entity_index)) return nullptr;
		return &m_dense[m_sparse[entity_index]];
	}

	const T* get(uint32_t entity_index) const {
		if (!has(entity_index)) return nullptr;
		return &m_dense[m_sparse[entity_index]];
	}

	T& getRef(uint32_t entity_index) {
		assert(has(entity_index));
		return m_dense[m_sparse[entity_index]];
	}

	const T& getRef(uint32_t entity_index) const {
		assert(has(entity_index));
		return m_dense[m_sparse[entity_index]];
	}

	// Dense array access for cache-friendly iteration
	T* data() { return m_dense.data(); }
	const T* data() const { return m_dense.data(); }
	uint32_t size() const { return static_cast<uint32_t>(m_dense.size()); }
	bool empty() const { return m_dense.empty(); }

	// Get entity index for a given dense index
	uint32_t entityAt(uint32_t dense_idx) const {
		assert(dense_idx < m_dense_to_entity.size());
		return m_dense_to_entity[dense_idx];
	}

	// Raw entity-index array (used by View iterator for driver iteration)
	const uint32_t* entityIndexData() const { return m_dense_to_entity.data(); }

	// Get dense index for a given entity index
	uint32_t denseIndex(uint32_t entity_index) const {
		assert(has(entity_index));
		return m_sparse[entity_index];
	}

	// Range-for support over dense array
	auto begin() { return m_dense.begin(); }
	auto end() { return m_dense.end(); }
	auto begin() const { return m_dense.begin(); }
	auto end() const { return m_dense.end(); }

	void clear() {
		m_dense.clear();
		m_dense_to_entity.clear();
		std::fill(m_sparse.begin(), m_sparse.end(), INVALID);
	}

private:
	void ensureSparseSize(uint32_t index) {
		if (index >= m_sparse.size())
			m_sparse.resize(index + 1, INVALID);
	}

	std::vector<T> m_dense;
	std::vector<uint32_t> m_dense_to_entity;
	std::vector<uint32_t> m_sparse;
};

} // namespace ve
