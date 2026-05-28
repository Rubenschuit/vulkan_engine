#pragma once
#include "ui/editor_panel.hpp"
#include <cstdint>

namespace ve {

class VeRenderer;
class VeWindow;
class EventBus;
class ParticleBackend;

class VENGINE_API GraphicsPanel : public EditorPanel {
public:
	GraphicsPanel(VeRenderer& renderer, VeWindow& window, EventBus& event_bus)
		: m_renderer(renderer), m_window(window), m_event_bus(event_bus) {}

	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Graphics"; }

	void setParticleBackend(ParticleBackend* p) { m_particles = p; }
	void setMaxParticleCapacity(uint32_t v) { m_max_particle_capacity = v; }

private:
	VeRenderer& m_renderer;
	VeWindow& m_window;
	EventBus& m_event_bus;
	ParticleBackend* m_particles = nullptr;
	uint32_t m_max_particle_capacity = 0;
};

} // namespace ve
