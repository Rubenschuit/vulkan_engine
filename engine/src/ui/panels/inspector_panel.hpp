#pragma once
#include "ui/editor_panel.hpp"
#include "ui/editor_state.hpp"

namespace ve {

class TransformComponent;
class MeshComponent;
class PointLightComponent;
class DirectionalLightComponent;

class VENGINE_API InspectorPanel : public EditorPanel {
public:
	void render(Registry* registry, EditorState& state, UIContext& context) override;
	const char* getName() const override { return "Inspector"; }

private:
	void renderEntityHeader(Registry& registry, Entity entity);
	void renderTransform(TransformComponent& transform);
	void renderMesh(MeshComponent& mesh);
	void renderPointLight(PointLightComponent& light);
	void renderDirectionalLight(DirectionalLightComponent& light);
};

} // namespace ve
