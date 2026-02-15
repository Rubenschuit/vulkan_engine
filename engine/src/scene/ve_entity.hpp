/* Entity - lightweight identifier for ECS.
 *
 * An entity is a 32-bit value encoding a 20-bit index and 12-bit generation.
 * The generation detects stale references after an entity slot is recycled.
 */
#pragma once
#include <cstdint>
#include <functional>

namespace ve {

class Entity {
public:
	static constexpr uint32_t INDEX_BITS = 20;
	static constexpr uint32_t GEN_BITS = 12;
	static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1;
	static constexpr uint32_t GEN_MASK = (1u << GEN_BITS) - 1;
	static constexpr uint32_t NULL_ID = UINT32_MAX;

	Entity() : m_id(NULL_ID) {}

	static Entity null() { return Entity{}; }
	bool isNull() const { return m_id == NULL_ID; }

	uint32_t index() const { return m_id & INDEX_MASK; }
	uint32_t generation() const { return (m_id >> INDEX_BITS) & GEN_MASK; }
	uint32_t id() const { return m_id; }

	bool operator==(Entity other) const { return m_id == other.m_id; }
	bool operator!=(Entity other) const { return m_id != other.m_id; }
	bool operator<(Entity other) const { return m_id < other.m_id; }

private:
	friend class Registry;
	Entity(uint32_t index, uint32_t gen)
		: m_id(((gen & GEN_MASK) << INDEX_BITS) | (index & INDEX_MASK)) {}
	uint32_t m_id;
};

} // namespace ve

namespace std {
template <>
struct hash<ve::Entity> {
	size_t operator()(ve::Entity e) const noexcept {
		return std::hash<uint32_t>{}(e.id());
	}
};
} // namespace std
