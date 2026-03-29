#include "pch.hpp"
#include "events/event_bus.hpp"

namespace ve {

void EventBus::flushEvents() {
	decltype(m_queues) pending;
	{
		std::lock_guard lock(m_queue_mutex);
		pending.swap(m_queues);
	}
	for (auto& [type, queue] : pending)
		queue->dispatchAll(*this);
}

void EventBus::clearQueue() {
	std::lock_guard lock(m_queue_mutex);
	m_queues.clear();
}

} // namespace ve